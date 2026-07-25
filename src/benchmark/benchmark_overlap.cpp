// benchmark_overlap.cpp - I/O overlap benchmark
//
// Tests:
//   F0: hnswlib native full-memory (true baseline, no block framework)
//   F2-single: c1024+GP, single query, blocking search
//   F2-batch: c1024+GP, batch query, non-blocking search
//   F2-event: event-driven batch search
//   F2-concurrent: multi-threaded concurrent search
//
// Usage:
//   ./benchmark_overlap <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50] [num_queries=200]
//   Env: INDEX_PATH=<hnswlib_index.bin>  (required for F0 baseline)
//        CACHE_MB=<cache_size_mb>  (default 256)

#include "hnswlib/hnswlib.h"
#include "common.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include "disk_hnsw.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>
#include <malloc.h>
#include <thread>
#include <atomic>

using SearchResult = std::pair<float, uint64_t>;

static size_t getRSS_MB() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.substr(0, 6) == "VmRSS:") {
            size_t val = 0;
            std::sscanf(line.c_str(), "VmRSS: %zu kB", &val);
            return val / 1024;
        }
    }
    return 0;
}

static std::vector<std::vector<uint64_t>> read_gt(const std::string& path, size_t n, int k) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("Cannot open GT: " + path);
    uint32_t n_queries, kk;
    in.read(reinterpret_cast<char*>(&n_queries), sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(&kk), sizeof(uint32_t));
    std::vector<std::vector<uint64_t>> gt(n);
    for (size_t i = 0; i < n; i++) {
        gt[i].resize(k);
        in.read(reinterpret_cast<char*>(gt[i].data()), k * sizeof(uint64_t));
    }
    return gt;
}

struct LatencyStats { double mean, p50, p95, p99, total_ms; };

static LatencyStats computeLatency(std::vector<double>& times) {
    std::sort(times.begin(), times.end());
    double sum = 0;
    for (double t : times) sum += t;
    size_t n = times.size();
    return { sum/n, times[n/2], times[(size_t)(n*0.95)], times[(size_t)(n*0.99)], sum/1000.0 };
}

struct BenchResult {
    std::string name;
    double recall_hnsw = 0;
    LatencyStats lat{};
    double qps = 0;
    double hit_rate = 0;
    size_t rss_mb = 0;
    size_t pf_submitted = 0;
    size_t pf_skipped = 0;
    size_t pf_failed = 0;
};

void printResult(const BenchResult& r) {
    std::cout << "\n=== " << r.name << " ===" << std::endl;
    std::cout << "  Recall@HNSW:  " << r.recall_hnsw << "%" << std::endl;
    std::cout << "  Mean:         " << r.lat.mean/1000 << " ms" << std::endl;
    std::cout << "  P50:          " << r.lat.p50/1000 << " ms" << std::endl;
    std::cout << "  P95:          " << r.lat.p95/1000 << " ms" << std::endl;
    std::cout << "  P99:          " << r.lat.p99/1000 << " ms" << std::endl;
    std::cout << "  QPS:          " << r.qps << std::endl;
    std::cout << "  Cache hit:    " << r.hit_rate << "%" << std::endl;
    std::cout << "  RSS:          " << r.rss_mb << " MB" << std::endl;
    if (r.pf_submitted > 0) {
        std::cout << "  Graph PF:     submitted=" << r.pf_submitted
                  << " skipped=" << r.pf_skipped
                  << " failed=" << r.pf_failed << std::endl;
    }
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::cerr << "Usage: " << argv[0] << " <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50] [num_queries=200]" << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string blocks_path = argv[3];
    std::string route_path = argv[4];
    std::string data_path = argv[5];
    std::string query_path = argv[6];
    std::string gt_path = argv[7];
    int k = (argc > 8) ? std::atoi(argv[8]) : 10;
    int ef = (argc > 9) ? std::atoi(argv[9]) : 50;
    size_t num_queries = (argc > 10) ? (size_t)std::atoll(argv[10]) : 200;

    std::cout << "=== I/O Overlap Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef << ", num_queries=" << num_queries << std::endl;

    // Load data (header only for base, full for queries)
    int dim;
    size_t num_base, num_query;
    // base_data 不需要加载, 只需要 dim 和 num_base
    {
        std::ifstream in(data_path, std::ios::binary);
        int32_t d; in.read(reinterpret_cast<char*>(&d), sizeof(int32_t));
        dim = d;
        in.seekg(0, std::ios::end);
        size_t file_size = in.tellg();
        size_t record_size = sizeof(int32_t) + dim * sizeof(float);
        num_base = file_size / record_size;
    }
    auto query_data = read_fvecs(query_path, dim, num_query);

    if (num_queries > num_query) num_queries = num_query;
    query_data.resize(num_queries * dim);
    num_query = num_queries;

    auto gt_data = read_gt(gt_path, num_query, k);

    // Get block info
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    uint32_t block_size = bfhdr.block_size;
    std::cout << "Blocks: " << num_blocks << ", block_size=" << block_size << std::endl;

    // Build HNSW baseline using hnswlib native (true full-memory)
    // F0 baseline: 加载 hnswlib 原生索引 (可被 SKIP_F0=1 跳过)
    bool skip_f0 = std::getenv("SKIP_F0") != nullptr;
    hnswlib::HierarchicalNSW<float>* hnsw_alg = nullptr;
    std::vector<std::vector<SearchResult>> hnsw_baseline;
    if (!skip_f0) {
        const char* index_path_env = std::getenv("INDEX_PATH");
        if (!index_path_env) {
            std::cerr << "ERROR: INDEX_PATH env var required (or set SKIP_F0=1)" << std::endl;
            return 1;
        }
        std::cout << "\n[1] Loading hnswlib index (native baseline)..." << std::endl;
        hnswlib::L2Space hnsw_space(dim);
        auto load_t0 = std::chrono::high_resolution_clock::now();
        hnsw_alg = new hnswlib::HierarchicalNSW<float>(&hnsw_space, std::string(index_path_env));
        hnsw_alg->setEf(ef);
        auto load_t1 = std::chrono::high_resolution_clock::now();
        std::cout << "  hnswlib index loaded in " << std::chrono::duration<double>(load_t1 - load_t0).count() << "s" << std::endl;
        std::cout << "  RSS after load: " << getRSS_MB() << " MB" << std::endl;

        for (size_t q = 0; q < num_query; q++) {
            auto result = hnsw_alg->searchKnn(&query_data[q * dim], k);
            std::vector<SearchResult> sr;
            while (!result.empty()) {
                auto& [dist, id] = result.top();
                sr.push_back({(float)dist, (uint32_t)id});
                result.pop();
            }
            hnsw_baseline.push_back(std::move(sr));
        }
    } else {
        std::cout << "\n[1] SKIP_F0=1, using ann-benchmarks GT as recall reference" << std::endl;
        // 用 GT 构造 baseline, 这样 F2 的 recall 计算不需要 hnswlib
        hnsw_baseline.resize(num_query);
        for (size_t q = 0; q < num_query; q++) {
            for (size_t i = 0; i < (size_t)k && i < gt_data[q].size(); i++) {
                hnsw_baseline[q].push_back({0.0f, (uint32_t)gt_data[q][i]});
            }
        }
    }

    IOConfig odirect_config;
    odirect_config.use_odirect = true;
    odirect_config.drop_page_cache = true;

    std::vector<BenchResult> all_results;

    // ---- F0: hnswlib native (skip if SKIP_F0) ----
    if (!skip_f0) {
    std::cout << "\n[F0] hnswlib native (full-memory)..." << std::endl;
        // Warmup
        for (size_t q = 0; q < std::min(10UL, num_query); q++)
            hnsw_alg->searchKnn(&query_data[q * dim], k);

        std::vector<double> latencies(num_query);
        std::vector<std::vector<SearchResult>> results(num_query);

        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t q = 0; q < num_query; q++) {
            auto q0 = std::chrono::high_resolution_clock::now();
            auto raw = hnsw_alg->searchKnn(&query_data[q * dim], k);
            auto q1 = std::chrono::high_resolution_clock::now();
            latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
            while (!raw.empty()) {
                auto& [dist, id] = raw.top();
                results[q].push_back({(float)dist, (uint32_t)id});
                raw.pop();
            }
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_s = std::chrono::duration<double>(t1 - t0).count();

        BenchResult r;
        r.name = "F0: hnswlib-native";
        r.lat = computeLatency(latencies);
        r.qps = num_query / total_s;
        r.rss_mb = getRSS_MB();

        size_t correct = 0;
        for (size_t q = 0; q < num_query; q++) {
            std::set<uint64_t> hset;
            for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
            for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
        }
        r.recall_hnsw = (double)correct / (num_query * k) * 100;
        r.hit_rate = 0; // N/A for native

        printResult(r);
        all_results.push_back(r);
    }
    if (hnsw_alg) { delete hnsw_alg; hnsw_alg = nullptr; }
    malloc_trim(0);
    std::cout << "\n[F2-single] c1024+GP, blocking search..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            1024, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        hnsw.enableGraphPrefetch(true);
        hnsw.resetCacheStats();
        if (hnsw.isGraphPrefetchEnabled()) hnsw.resetGraphPrefetchStats();

        for (size_t q = 0; q < std::min(10UL, num_query); q++)
            hnsw.searchKnn(&query_data[q * dim], k);

        std::vector<double> latencies(num_query);
        std::vector<std::vector<SearchResult>> results(num_query);

        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t q = 0; q < num_query; q++) {
            auto q0 = std::chrono::high_resolution_clock::now();
            results[q] = hnsw.searchKnn(&query_data[q * dim], k);
            auto q1 = std::chrono::high_resolution_clock::now();
            latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        BenchResult r;
        r.name = "F2-single: c1024+GP (blocking)";
        r.lat = computeLatency(latencies);
        r.qps = num_query / std::chrono::duration<double>(t1 - t0).count();
        r.rss_mb = getRSS_MB();

        size_t correct = 0;
        for (size_t q = 0; q < num_query; q++) {
            std::set<uint64_t> hset;
            for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
            for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
        }
        r.recall_hnsw = (double)correct / (num_query * k) * 100;

        auto& stats = hnsw.getCacheStats();
        r.hit_rate = stats.total_accesses > 0 ?
            (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
        auto pf = hnsw.getGraphPrefetchStats();
        r.pf_submitted = pf.prefetch_submitted;
        r.pf_skipped = pf.prefetch_skipped;
        r.pf_failed = pf.prefetch_failed;

        printResult(r);
        all_results.push_back(r);
    }
    // 释放 hnswlib index, 之后的 F2 测试不应该包含它的内存
    delete hnsw_alg;
    malloc_trim(0);


    // ---- F2-batch: c1024+GP, batch non-blocking search ----
    const char* skip_env = std::getenv("SKIP_CONFIGS");
    std::string skip_configs = skip_env ? skip_env : "";
    auto shouldSkip = [&](const std::string& name) {
        return skip_configs.find(name) != std::string::npos;
    };
    for (size_t bs : {4}) {
        if (bs > num_query) continue;
        if (shouldSkip("batch")) continue;
        std::cout << "\n[F2-batch-" << bs << "] c1024+GP, batch non-blocking (bs=" << bs << ")..." << std::endl;
        {
            auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
            auto policy = std::make_unique<LRUPolicy>();
            auto cache = std::make_unique<BlockCache>(
                blocks_path, std::move(layout), std::move(policy),
                1024, dim, odirect_config);
            DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
            hnsw.setEf(ef);
            hnsw.enableGraphPrefetch(true);
            hnsw.resetCacheStats();
            if (hnsw.isGraphPrefetchEnabled()) hnsw.resetGraphPrefetchStats();

            // Warmup
            std::vector<float> warmup_q(std::min(10UL, num_query) * dim);
            std::memcpy(warmup_q.data(), query_data.data(), warmup_q.size() * sizeof(float));
            hnsw.batchSearch(warmup_q, k, bs);

            // Timed search (逐查询计时, 报告 batch QPS)
            std::vector<double> batch_latencies(num_query);
            std::vector<std::vector<SearchResult>> results(num_query);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t q = 0; q < num_query; q++) {
                auto q0 = std::chrono::high_resolution_clock::now();
                results[q] = hnsw.searchKnn(&query_data[q * dim], k);
                auto q1 = std::chrono::high_resolution_clock::now();
                batch_latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double total_s = std::chrono::duration<double>(t1 - t0).count();

            BenchResult r;
            r.name = "F2-batch-" + std::to_string(bs) + ": c1024+GP (non-blocking)";
            r.lat = computeLatency(batch_latencies);
            r.qps = num_query / total_s;
            r.rss_mb = getRSS_MB();

            size_t correct = 0;
            for (size_t q = 0; q < num_query; q++) {
                std::set<uint64_t> hset;
                for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
                for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
            }
            r.recall_hnsw = (double)correct / (num_query * k) * 100;

            auto& stats = hnsw.getCacheStats();
            r.hit_rate = stats.total_accesses > 0 ?
                (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
            auto pf = hnsw.getGraphPrefetchStats();
            r.pf_submitted = pf.prefetch_submitted;
            r.pf_skipped = pf.prefetch_skipped;
        r.pf_failed = pf.prefetch_failed;

            printResult(r);
            all_results.push_back(r);
        }
        malloc_trim(0);
    }

    // ---- F2-single-256MB: 同内存 (256MB) 对比, slots 按 block_size 自动计算 ----
    // 缓存大小从环境变量读取, 默认 256MB
    size_t cache_mb = []() {
        const char* e = std::getenv("CACHE_MB");
        return e ? std::atol(e) : 256;
    }();
    size_t mem_slots = (size_t)cache_mb * 1024 * 1024 / block_size;
    std::cout << "\n[F2-single-" << cache_mb << "MB] c" << mem_slots << "+GP, blocking..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            mem_slots, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        hnsw.enableGraphPrefetch(true);
        hnsw.resetCacheStats();
        if (hnsw.isGraphPrefetchEnabled()) hnsw.resetGraphPrefetchStats();

        for (size_t q = 0; q < std::min(10UL, num_query); q++)
            hnsw.searchKnn(&query_data[q * dim], k);

        std::vector<double> latencies(num_query);
        std::vector<std::vector<SearchResult>> results(num_query);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t q = 0; q < num_query; q++) {
            auto q0 = std::chrono::high_resolution_clock::now();
            results[q] = hnsw.searchKnn(&query_data[q * dim], k);
            auto q1 = std::chrono::high_resolution_clock::now();
            latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
        }
        auto t1 = std::chrono::high_resolution_clock::now();

        BenchResult r;
        r.name = "F2-single: c" + std::to_string(mem_slots) + "+GP (256MB)";
        r.lat = computeLatency(latencies);
        r.qps = num_query / std::chrono::duration<double>(t1 - t0).count();
        r.rss_mb = getRSS_MB();
        size_t correct = 0;
        for (size_t q = 0; q < num_query; q++) {
            std::set<uint64_t> hset;
            for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
            for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
        }
        r.recall_hnsw = (double)correct / (num_query * k) * 100;
        auto& stats = hnsw.getCacheStats();
        r.hit_rate = stats.total_accesses > 0 ?
            (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
        auto pf = hnsw.getGraphPrefetchStats();
        r.pf_submitted = pf.prefetch_submitted;
        r.pf_skipped = pf.prefetch_skipped;
        r.pf_failed = pf.prefetch_failed;
        printResult(r);
        all_results.push_back(r);
    }
    malloc_trim(0);

    // ---- F2-beam: Cache-Aware Beam Search ----
    // 测试多个 beam width: B=2, 4, 8
    if (!shouldSkip("beam"))
    for (int bw : {2, 4, 8}) {
        std::cout << "\n[F2-beam-" << bw << "] c" << mem_slots << "+GP, beam search (B=" << bw << ")..." << std::endl;
        {
            auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
            auto policy = std::make_unique<LRUPolicy>();
            auto cache = std::make_unique<BlockCache>(
                blocks_path, std::move(layout), std::move(policy),
                mem_slots, dim, odirect_config);
            DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
            hnsw.setEf(ef);
            hnsw.enableGraphPrefetch(true);
            hnsw.resetCacheStats();
            if (hnsw.isGraphPrefetchEnabled()) hnsw.resetGraphPrefetchStats();

            // 设置 BEAM_WIDTH 环境变量 (searchKnn 内部读取)
            setenv("BEAM_WIDTH", std::to_string(bw).c_str(), 1);

            // Warmup
            for (size_t q = 0; q < std::min(10UL, num_query); q++)
                hnsw.searchKnn(&query_data[q * dim], k);

            std::vector<double> latencies(num_query);
            std::vector<std::vector<SearchResult>> results(num_query);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t q = 0; q < num_query; q++) {
                auto q0 = std::chrono::high_resolution_clock::now();
                results[q] = hnsw.searchKnn(&query_data[q * dim], k);
                auto q1 = std::chrono::high_resolution_clock::now();
                latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
            }
            auto t1 = std::chrono::high_resolution_clock::now();

            // 恢复环境变量
            unsetenv("BEAM_WIDTH");

            BenchResult r;
            r.name = "F2-beam-" + std::to_string(bw) + ": c" + std::to_string(mem_slots) + "+GP (256MB)";
            r.lat = computeLatency(latencies);
            r.qps = num_query / std::chrono::duration<double>(t1 - t0).count();
            r.rss_mb = getRSS_MB();
            size_t correct = 0;
            for (size_t q = 0; q < num_query; q++) {
                std::set<uint64_t> hset;
                for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
                for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
            }
            r.recall_hnsw = (double)correct / (num_query * k) * 100;
            auto& stats = hnsw.getCacheStats();
            r.hit_rate = stats.total_accesses > 0 ?
                (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
            auto pf = hnsw.getGraphPrefetchStats();
            r.pf_submitted = pf.prefetch_submitted;
            r.pf_skipped = pf.prefetch_skipped;
            r.pf_failed = pf.prefetch_failed;
            printResult(r);
            all_results.push_back(r);
        }
        malloc_trim(0);
    }

    // ---- F2-batch-io: 批量并行 I/O 搜索 ----
    // 测试多个 batch size: N=8, 16, 32
    if (!shouldSkip("batchio"))
    for (int bn : {8, 16, 32}) {
        std::cout << "\n[F2-batch-io-" << bn << "] c" << mem_slots << "+GP, batch I/O (N=" << bn << ")..." << std::endl;
        {
            auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
            auto policy = std::make_unique<LRUPolicy>();
            auto cache = std::make_unique<BlockCache>(
                blocks_path, std::move(layout), std::move(policy),
                mem_slots, dim, odirect_config);
            DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
            hnsw.setEf(ef);
            hnsw.enableGraphPrefetch(true);
            hnsw.resetCacheStats();
            if (hnsw.isGraphPrefetchEnabled()) hnsw.resetGraphPrefetchStats();

            // 设置 BATCH_IO_N 环境变量 (searchKnn 内部读取)
            setenv("BATCH_IO_N", std::to_string(bn).c_str(), 1);

            // Warmup
            for (size_t q = 0; q < std::min(10UL, num_query); q++)
                hnsw.searchKnn(&query_data[q * dim], k);

            std::vector<double> latencies(num_query);
            std::vector<std::vector<SearchResult>> results(num_query);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t q = 0; q < num_query; q++) {
                auto q0 = std::chrono::high_resolution_clock::now();
                results[q] = hnsw.searchKnn(&query_data[q * dim], k);
                auto q1 = std::chrono::high_resolution_clock::now();
                latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
            }
            auto t1 = std::chrono::high_resolution_clock::now();

            // 恢复环境变量
            unsetenv("BATCH_IO_N");

            BenchResult r;
            r.name = "F2-batch-io-" + std::to_string(bn) + ": c" + std::to_string(mem_slots) + "+GP (256MB)";
            r.lat = computeLatency(latencies);
            r.qps = num_query / std::chrono::duration<double>(t1 - t0).count();
            r.rss_mb = getRSS_MB();
            size_t correct = 0;
            for (size_t q = 0; q < num_query; q++) {
                std::set<uint64_t> hset;
                for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
                for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
            }
            r.recall_hnsw = (double)correct / (num_query * k) * 100;
            auto& stats = hnsw.getCacheStats();
            r.hit_rate = stats.total_accesses > 0 ?
                (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
            auto pf = hnsw.getGraphPrefetchStats();
            r.pf_submitted = pf.prefetch_submitted;
            r.pf_skipped = pf.prefetch_skipped;
            r.pf_failed = pf.prefetch_failed;
            printResult(r);
            all_results.push_back(r);
        }
        malloc_trim(0);
    }

    // ---- F2-event: Event-driven multi-query concurrency ----
    // 测试 event-driven batch search: batch=4, batch=8
    if (!shouldSkip("event"))
    for (size_t bs : {4, 8}) {
        if (bs > num_query) continue;
        std::cout << "\n[F2-event-" << bs << "] c" << mem_slots << "+GP, event-driven (bs=" << bs << ")..." << std::endl;
        {
            auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
            auto policy = std::make_unique<LRUPolicy>();
            auto cache = std::make_unique<BlockCache>(
                blocks_path, std::move(layout), std::move(policy),
                mem_slots, dim, odirect_config);
            DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
            hnsw.setEf(ef);
            hnsw.enableGraphPrefetch(true);
            hnsw.resetCacheStats();
            if (hnsw.isGraphPrefetchEnabled()) hnsw.resetGraphPrefetchStats();

            // Warmup with a few queries
            std::vector<float> warmup_q(std::min(10UL, num_query) * dim);
            std::memcpy(warmup_q.data(), query_data.data(), warmup_q.size() * sizeof(float));
            hnsw.batchSearchEventDriven(warmup_q, k, bs);

            // Timed search: batch event-driven
            // 每次处理 bs 个查询, 记录总时间和每查询延迟
            std::vector<double> latencies(num_query);
            std::vector<std::vector<SearchResult>> results(num_query);

            auto t0 = std::chrono::high_resolution_clock::now();
            for (size_t batch_start = 0; batch_start < num_query; batch_start += bs) {
                size_t batch_end = std::min(batch_start + bs, num_query);
                size_t cur_bs = batch_end - batch_start;

                auto q0 = std::chrono::high_resolution_clock::now();
                auto batch_results = hnsw.batchSearchEventDriven(
                    std::vector<float>(query_data.begin() + batch_start * dim,
                                       query_data.begin() + batch_end * dim),
                    k, cur_bs);
                auto q1 = std::chrono::high_resolution_clock::now();

                double batch_us = std::chrono::duration<double, std::micro>(q1 - q0).count();
                double per_query_us = batch_us / cur_bs;
                for (size_t i = 0; i < cur_bs; i++) {
                    latencies[batch_start + i] = per_query_us;
                    results[batch_start + i] = std::move(batch_results[i]);
                }
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double total_s = std::chrono::duration<double>(t1 - t0).count();

            BenchResult r;
            r.name = "F2-event-" + std::to_string(bs) + ": c" + std::to_string(mem_slots) + "+GP (256MB)";
            r.lat = computeLatency(latencies);
            r.qps = num_query / total_s;
            r.rss_mb = getRSS_MB();

            size_t correct = 0;
            for (size_t q = 0; q < num_query; q++) {
                std::set<uint64_t> hset;
                for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
                for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
            }
            r.recall_hnsw = (double)correct / (num_query * k) * 100;

            auto& stats = hnsw.getCacheStats();
            r.hit_rate = stats.total_accesses > 0 ?
                (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
            auto pf = hnsw.getGraphPrefetchStats();
            r.pf_submitted = pf.prefetch_submitted;
            r.pf_skipped = pf.prefetch_skipped;
            r.pf_failed = pf.prefetch_failed;

            printResult(r);
            all_results.push_back(r);
        }
        malloc_trim(0);
    }

    // ---- F2-concurrent: multi-threaded concurrent search ----
    // Tests 4 configs: concurrent-4-nb, concurrent-8-nb, concurrent-4, concurrent-8
    if (!shouldSkip("concurrent"))
    for (auto& [nt, nb] : std::vector<std::pair<int,int>>{{4,1},{8,1},{4,0},{8,0}}) {
        std::string config_name = "F2-concurrent-" + std::to_string(nt) + (nb ? "-nb" : "");
        std::cout << "\n[" << config_name << "] c" << mem_slots << "+GP, " << nt
                  << " threads, " << (nb ? "non-blocking" : "blocking") << "..." << std::endl;

        // Set NONBLOCK env var
        if (nb) setenv("NONBLOCK", "1", 1);
        else setenv("NONBLOCK", "0", 1);

        {
            auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
            auto policy = std::make_unique<LRUPolicy>();
            auto cache = std::make_unique<BlockCache>(
                blocks_path, std::move(layout), std::move(policy),
                mem_slots, dim, odirect_config);
            DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
            hnsw.setEf(ef);
            hnsw.enableGraphPrefetch(true);
            hnsw.resetCacheStats();
            if (hnsw.isGraphPrefetchEnabled()) hnsw.resetGraphPrefetchStats();

            // Warmup (single-threaded, same as benchmark)
            for (size_t q = 0; q < std::min(10UL, num_query); q++)
                hnsw.searchKnn(&query_data[q * dim], k);

            // Timed concurrent search
            std::vector<std::vector<SearchResult>> results(num_query);
            std::atomic<size_t> next_q{0};
            std::vector<double> latencies(num_query);

            auto worker = [&]() {
                while (true) {
                    size_t q = next_q.fetch_add(1);
                    if (q >= num_query) break;
                    auto q0 = std::chrono::high_resolution_clock::now();
                    results[q] = hnsw.searchKnn(&query_data[q * dim], k);
                    auto q1 = std::chrono::high_resolution_clock::now();
                    latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
                }
            };

            std::vector<std::thread> threads;
            threads.reserve(nt);
            auto t0 = std::chrono::high_resolution_clock::now();
            for (int i = 0; i < nt; i++) threads.emplace_back(worker);
            for (auto& t : threads) t.join();
            auto t1 = std::chrono::high_resolution_clock::now();
            double total_s = std::chrono::duration<double>(t1 - t0).count();

            BenchResult r;
            r.name = config_name + ": c" + std::to_string(mem_slots) + "+GP (256MB)";
            r.lat = computeLatency(latencies);
            r.qps = num_query / total_s;
            r.rss_mb = getRSS_MB();

            size_t correct = 0;
            for (size_t q = 0; q < num_query; q++) {
                std::set<uint64_t> hset;
                for (const auto& [d, id] : hnsw_baseline[q]) hset.insert(id);
                for (const auto& [d, id] : results[q]) if (hset.count(id)) correct++;
            }
            r.recall_hnsw = (double)correct / (num_query * k) * 100;

            auto& stats = hnsw.getCacheStats();
            r.hit_rate = stats.total_accesses > 0 ?
                (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
            auto pf = hnsw.getGraphPrefetchStats();
            r.pf_submitted = pf.prefetch_submitted;
            r.pf_skipped = pf.prefetch_skipped;
            r.pf_failed = pf.prefetch_failed;

            printResult(r);
            all_results.push_back(r);
        }
        malloc_trim(0);
    }

    // Restore NONBLOCK env
    unsetenv("NONBLOCK");

    // Summary
    std::cout << "\n\n=== SUMMARY ===" << std::endl;
    std::cout << "| Config | Mean(ms) | P99(ms) | QPS | Hit% | R@HNSW | PF_sub | PF_skip | PF_fail |" << std::endl;
    std::cout << "|--------|----------|---------|-----|------|--------|-------------|------------|" << std::endl;
    for (const auto& r : all_results) {
        std::cout << "| " << r.name << " | " << r.lat.mean/1000 << " | "
                  << r.lat.p99/1000 << " | " << r.qps << " | "
                  << r.hit_rate << " | " << r.recall_hnsw << " | "
                  << r.pf_submitted << " | " << r.pf_skipped << " | " << r.pf_failed << " |" << std::endl;
    }

    return 0;
}
