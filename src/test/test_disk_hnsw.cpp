// test_disk_hnsw.cpp - Task 2.2 测试程序
//
// 测试内容：
// 1. 加载全内存HNSW索引和DiskHNSW
// 2. 对比搜索结果的recall和一致性
// 3. 打印BlockCache统计信息
// 4. 支持可插拔布局和替换策略
// 5. 支持 I/O 模式配置（cached / direct / simulated）
//
// 用法: ./test_disk_hnsw <index.bin> <graph.bin> <bfs.bin> <blocks.bin> <route.bin> <data.fvecs> <query.fvecs> [k=10] [ef=50] [cache_slots=64] [--io-mode=cached|direct|simulated] [--latency=100] [--layout=bfs|random] [--policy=lru|lfu|lru-k]

#include "hnswlib/hnswlib.h"
#include "common.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include "disk_hnsw.h"

#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <cstring>

using hnswlib::tableint;

// 读取查询数据（fvecs格式）
std::vector<float> read_query_fvecs(const std::string& path, int& dim, size_t& count) {
    return read_fvecs(path, dim, count);
}

// 计算recall@k
double computeRecall(const std::vector<std::vector<uint64_t>>& ground_truth,
                     const std::vector<std::vector<uint64_t>>& predicted,
                     size_t k) {
    size_t total_hits = 0;
    size_t total = 0;

    for (size_t i = 0; i < ground_truth.size(); i++) {
        std::set<uint64_t> gt_set(ground_truth[i].begin(),
                                   std::min(ground_truth[i].begin() + k, ground_truth[i].end()));
        for (size_t j = 0; j < std::min(k, predicted[i].size()); j++) {
            if (gt_set.count(predicted[i][j]) > 0) {
                total_hits++;
            }
        }
        total += k;
    }

    return total > 0 ? (double)total_hits / total : 0.0;
}

// 暴力计算ground truth
std::vector<std::vector<uint64_t>> computeGroundTruth(
        const std::vector<float>& base_data,
        const std::vector<float>& query_data,
        size_t num_base, size_t num_query, int dim, size_t k) {
    
    std::cout << "Computing ground truth (brute force)..." << std::endl;
    std::vector<std::vector<uint64_t>> gt(num_query);

    for (size_t q = 0; q < num_query; q++) {
        const float* query = &query_data[q * dim];
        
        std::vector<std::pair<float, uint64_t>> dists(num_base);
        for (size_t b = 0; b < num_base; b++) {
            const float* base = &base_data[b * dim];
            float d = 0;
            for (int d_idx = 0; d_idx < dim; d_idx++) {
                float t = query[d_idx] - base[d_idx];
                d += t * t;
            }
            dists[b] = {d, b};
        }
        
        std::partial_sort(dists.begin(), dists.begin() + k, dists.end());
        
        gt[q].reserve(k);
        for (size_t i = 0; i < k; i++) {
            gt[q].push_back(dists[i].second);
        }
    }

    return gt;
}

// 解析命令行参数
struct CmdArgs {
    std::string io_mode = "cached";   // cached | direct | simulated
    double latency = 100.0;            // 模拟延迟（微秒）
    std::string layout = "bfs";        // bfs | random
    std::string policy = "lru";        // lru | lfu | lru-k
};

CmdArgs parseExtraArgs(int argc, char** argv, int start_idx) {
    CmdArgs args;
    for (int i = start_idx; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--io-mode=", 0) == 0) {
            args.io_mode = arg.substr(10);
        } else if (arg.rfind("--latency=", 0) == 0) {
            args.latency = std::stod(arg.substr(10));
        } else if (arg.rfind("--layout=", 0) == 0) {
            args.layout = arg.substr(9);
        } else if (arg.rfind("--policy=", 0) == 0) {
            args.policy = arg.substr(9);
        }
    }
    return args;
}

// 构建 IOConfig
IOConfig buildIOConfig(const std::string& mode, double latency) {
    IOConfig config;
    if (mode == "direct") {
        config.use_odirect = true;
        config.drop_page_cache = true;
        config.simulated_latency_us = 0.0;
    } else if (mode == "simulated") {
        config.use_odirect = false;
        config.drop_page_cache = true;
        config.simulated_latency_us = latency;
    } else { // cached
        config.use_odirect = false;
        config.drop_page_cache = false;
        config.simulated_latency_us = 0.0;
    }
    return config;
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::cerr << "Usage: " << argv[0] 
                  << " <index.bin> <graph.bin> <bfs.bin> <blocks.bin> <route.bin>"
                  << " <data.fvecs> <query.fvecs> [k=10] [ef=50] [cache_slots=64]"
                  << " [--io-mode=cached|direct|simulated] [--latency=100]"
                  << " [--layout=bfs|random] [--policy=lru|lfu|lru-k]"
                  << std::endl;
        return 1;
    }

    std::string index_path = argv[1];
    std::string graph_path = argv[2];
    std::string bfs_path = argv[3];
    std::string blocks_path = argv[4];
    std::string route_path = argv[5];
    std::string data_path = argv[6];
    std::string query_path = argv[7];
    
    // 解析可选位置参数
    size_t k = 10, ef = 50, cache_slots = 64;
    int opt_idx = 8;
    if (argc > opt_idx && argv[opt_idx][0] != '-') {
        k = std::stoul(argv[opt_idx++]);
    }
    if (argc > opt_idx && argv[opt_idx][0] != '-') {
        ef = std::stoul(argv[opt_idx++]);
    }
    if (argc > opt_idx && argv[opt_idx][0] != '-') {
        cache_slots = std::stoul(argv[opt_idx++]);
    }

    // 解析 -- 开头的额外参数
    CmdArgs cmd_args = parseExtraArgs(argc, argv, opt_idx);

    std::cout << "=== Task 2.2: DiskHNSW Test (Refactored) ===" << std::endl;
    std::cout << "Parameters: k=" << k << ", ef=" << ef << ", cache_slots=" << cache_slots << std::endl;
    std::cout << "  io_mode=" << cmd_args.io_mode
              << ", latency=" << cmd_args.latency << "us"
              << ", layout=" << cmd_args.layout
              << ", policy=" << cmd_args.policy << std::endl;

    // 构建 IOConfig
    IOConfig io_config = buildIOConfig(cmd_args.io_mode, cmd_args.latency);

    // ---- 1. 加载数据 ----
    int dim;
    size_t num_base, num_query;
    std::cout << "\n[1] Loading data..." << std::endl;
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<float> base_data = read_fvecs(data_path, dim, num_base);
    std::vector<float> query_data = read_fvecs(query_path, dim, num_query);
    auto t1 = std::chrono::high_resolution_clock::now();
    std::cout << "  Base: " << num_base << " vectors, dim=" << dim << std::endl;
    std::cout << "  Query: " << num_query << " vectors" << std::endl;
    std::cout << "  Load time: " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;

    // ---- 2. 加载全内存HNSW ----
    std::cout << "\n[2] Loading full-memory HNSW index..." << std::endl;
    t0 = std::chrono::high_resolution_clock::now();
    hnswlib::L2Space space(dim);
    auto* hnsw = new hnswlib::HierarchicalNSW<float>(&space, index_path);
    hnsw->setEf(ef);
    t1 = std::chrono::high_resolution_clock::now();
    std::cout << "  Loaded: " << hnsw->getCurrentElementCount() << " nodes" << std::endl;
    std::cout << "  Load time: " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;

    // ---- 3. 构建可插拔组件 ----
    std::cout << "\n[3] Building pluggable components..." << std::endl;

    // 先读取 blocks.bin 头获取 num_blocks
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;

    // 构建 LayoutProvider
    std::unique_ptr<LayoutProvider> layout;
    if (cmd_args.layout == "random") {
        layout = std::make_unique<RandomLayoutProvider>(num_base, num_blocks, 42);
    } else {
        layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
    }

    // 构建 ReplacementPolicy
    std::unique_ptr<ReplacementPolicy> policy;
    if (cmd_args.policy == "lfu") {
        policy = std::make_unique<LFUPolicy>();
    } else if (cmd_args.policy == "lru-k") {
        policy = std::make_unique<LRUKPolicy>();
    } else {
        policy = std::make_unique<LRUPolicy>();
    }

    // 构建 BlockCache
    auto cache = std::make_unique<BlockCache>(
        blocks_path, std::move(layout), std::move(policy),
        cache_slots, dim, io_config);

    // ---- 4. 构建 DiskHNSW ----
    std::cout << "\n[4] Loading DiskHNSW..." << std::endl;
    t0 = std::chrono::high_resolution_clock::now();

    // DiskHNSW 仍需要 graph_path 和 bfs_path
    // 但 BlockCache 已经通过可插拔接口构造
    DiskHNSW diskHnsw(graph_path, bfs_path, std::move(cache));
    diskHnsw.setEf(ef);
    t1 = std::chrono::high_resolution_clock::now();
    std::cout << "  Load time: " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;

    // ---- 5. 计算Ground Truth ----
    std::cout << "\n[5] Computing ground truth..." << std::endl;
    t0 = std::chrono::high_resolution_clock::now();
    auto ground_truth = computeGroundTruth(base_data, query_data, num_base, num_query, dim, k);
    t1 = std::chrono::high_resolution_clock::now();
    std::cout << "  GT compute time: " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;

    // ---- 6. 运行全内存HNSW搜索 ----
    std::cout << "\n[6] Running full-memory HNSW search..." << std::endl;
    std::vector<std::vector<uint64_t>> hnsw_results(num_query);
    std::vector<double> hnsw_times(num_query);

    t0 = std::chrono::high_resolution_clock::now();
    for (size_t q = 0; q < num_query; q++) {
        auto qt0 = std::chrono::high_resolution_clock::now();
        auto result = hnsw->searchKnn(&query_data[q * dim], k);
        auto qt1 = std::chrono::high_resolution_clock::now();
        hnsw_times[q] = std::chrono::duration<double, std::micro>(qt1 - qt0).count();

        while (!result.empty()) {
            hnsw_results[q].push_back(result.top().second);
            result.pop();
        }
        std::reverse(hnsw_results[q].begin(), hnsw_results[q].end());
    }
    t1 = std::chrono::high_resolution_clock::now();
    double hnsw_total_time = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ---- 7. 运行DiskHNSW搜索 ----
    std::cout << "\n[7] Running DiskHNSW search..." << std::endl;
    std::vector<std::vector<uint64_t>> disk_results(num_query);
    std::vector<double> disk_times(num_query);

    diskHnsw.resetCacheStats();

    t0 = std::chrono::high_resolution_clock::now();
    for (size_t q = 0; q < num_query; q++) {
        auto qt0 = std::chrono::high_resolution_clock::now();
        auto result = diskHnsw.searchKnn(&query_data[q * dim], k);
        auto qt1 = std::chrono::high_resolution_clock::now();
        disk_times[q] = std::chrono::duration<double, std::micro>(qt1 - qt0).count();

        for (const auto& [dist, label] : result) {
            disk_results[q].push_back(label);
        }
    }
    t1 = std::chrono::high_resolution_clock::now();
    double disk_total_time = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ---- 8. 对比结果 ----
    std::cout << "\n[8] Comparing results..." << std::endl;

    double hnsw_recall_gt = computeRecall(ground_truth, hnsw_results, k);
    double disk_recall_gt = computeRecall(ground_truth, disk_results, k);

    size_t exact_match_count = 0;
    size_t total_compared = 0;
    for (size_t q = 0; q < num_query; q++) {
        std::set<uint64_t> hnsw_set(hnsw_results[q].begin(), hnsw_results[q].end());
        for (size_t j = 0; j < std::min(k, disk_results[q].size()); j++) {
            if (hnsw_set.count(disk_results[q][j]) > 0) {
                exact_match_count++;
            }
            total_compared++;
        }
    }
    double disk_vs_hnsw_recall = total_compared > 0 ? (double)exact_match_count / total_compared : 0.0;

    auto computeStats = [](const std::vector<double>& times) {
        std::vector<double> sorted = times;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0;
        for (double t : sorted) sum += t;
        return std::tuple<double, double, double, double>(
            sum / sorted.size(),
            sorted[sorted.size() / 2],
            sorted[(size_t)(sorted.size() * 0.95)],
            sorted[(size_t)(sorted.size() * 0.99)]
        );
    };

    auto [hnsw_mean, hnsw_p50, hnsw_p95, hnsw_p99] = computeStats(hnsw_times);
    auto [disk_mean, disk_p50, disk_p95, disk_p99] = computeStats(disk_times);

    // ---- 9. 打印结果 ----
    std::cout << "\n=== Test Results ===" << std::endl;
    std::cout << "Dataset: " << num_base << " base, " << num_query << " query, dim=" << dim << std::endl;
    std::cout << "Parameters: k=" << k << ", ef=" << ef << ", cache_slots=" << cache_slots << std::endl;
    std::cout << "IO mode: " << cmd_args.io_mode
              << ", latency=" << cmd_args.latency << "us"
              << ", layout=" << cmd_args.layout
              << ", policy=" << cmd_args.policy << std::endl;
    std::cout << std::endl;

    std::cout << "--- Recall ---" << std::endl;
    std::cout << "  Full-memory HNSW recall@k vs GT:  " << (hnsw_recall_gt * 100) << "%" << std::endl;
    std::cout << "  DiskHNSW recall@k vs GT:          " << (disk_recall_gt * 100) << "%" << std::endl;
    std::cout << "  DiskHNSW recall@k vs HNSW:        " << (disk_vs_hnsw_recall * 100) << "%" << std::endl;
    std::cout << std::endl;

    std::cout << "--- Latency (microseconds) ---" << std::endl;
    std::cout << "  Full-memory HNSW:" << std::endl;
    std::cout << "    Mean: " << hnsw_mean << " us" << std::endl;
    std::cout << "    P50:  " << hnsw_p50 << " us" << std::endl;
    std::cout << "    P95:  " << hnsw_p95 << " us" << std::endl;
    std::cout << "    P99:  " << hnsw_p99 << " us" << std::endl;
    std::cout << "    Total: " << hnsw_total_time << " ms" << std::endl;
    std::cout << "  DiskHNSW:" << std::endl;
    std::cout << "    Mean: " << disk_mean << " us" << std::endl;
    std::cout << "    P50:  " << disk_p50 << " us" << std::endl;
    std::cout << "    P95:  " << disk_p95 << " us" << std::endl;
    std::cout << "    P99:  " << disk_p99 << " us" << std::endl;
    std::cout << "    Total: " << disk_total_time << " ms" << std::endl;
    std::cout << std::endl;

    std::cout << "--- BlockCache Stats ---" << std::endl;
    const auto& stats = diskHnsw.getCacheStats();
    std::cout << "  Total accesses:  " << stats.total_accesses << std::endl;
    std::cout << "  Cache hits:      " << stats.cache_hits << std::endl;
    std::cout << "  Cache misses:    " << stats.cache_misses << std::endl;
    std::cout << "  Hit rate:        " << (diskHnsw.getCacheStats().total_accesses > 0 ?
        (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0) << "%" << std::endl;
    std::cout << "  Evictions:       " << stats.evictions << std::endl;
    std::cout << "  Disk reads:      " << stats.disk_reads << std::endl;
    std::cout << "  Cached blocks:   " << diskHnsw.getNumCachedBlocks() << "/" << diskHnsw.getCacheSlots() << std::endl;
    std::cout << std::endl;

    // ---- 10. 判定 ----
    std::cout << "=== Verdict ===" << std::endl;
    bool recall_ok = disk_vs_hnsw_recall >= 0.95;
    std::cout << "  DiskHNSW vs HNSW recall >= 95%: " << (recall_ok ? "PASS ✅" : "FAIL ❌") << std::endl;
    std::cout << "  DiskHNSW vs GT recall: " << (disk_recall_gt * 100) << "%" << std::endl;
    std::cout << "  Latency ratio (disk/hnsw): " << (hnsw_mean > 0 ? disk_mean / hnsw_mean : 0) << "x" << std::endl;

    delete hnsw;
    return recall_ok ? 0 : 1;
}
