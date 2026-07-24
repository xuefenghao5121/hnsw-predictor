// benchmark_pipeline.cpp - 多查询流水线 benchmark
//
// 比较: 串行搜索 vs 多查询流水线搜索
// 流水线: P 个查询并发推进, 某查询卡 I/O 时切到另一查询, 共享 io_uring 批量提交
//
// 用法:
//   ./benchmark_pipeline <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50] [num_queries=200] [pipeline_depth=4]

#include "common.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include "disk_hnsw.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <malloc.h>

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
    in.close();
    return gt;
}

struct LatencyStats { double mean, p50, p95, p99; };

static LatencyStats computeLatency(std::vector<double>& times) {
    std::sort(times.begin(), times.end());
    double sum = 0;
    for (double t : times) sum += t;
    size_t n = times.size();
    return { sum/n, times[n/2], times[(size_t)(n*0.95)], times[(size_t)(n*0.99)] };
}

struct BenchResult {
    std::string name;
    double recall_hnsw = 0;
    LatencyStats lat{};
    double qps = 0;
    double hit_rate = 0;
    size_t rss_mb = 0;
    uint64_t pf_submitted = 0;
    uint64_t pf_skipped = 0;
};

void printResult(const BenchResult& r) {
    std::cout << "\n=== " << r.name << " ===" << std::endl;
    std::cout << "  Recall@HNSW:  " << r.recall_hnsw << "%" << std::endl;
    std::cout << "  Mean:         " << r.lat.mean << " us (" << r.lat.mean/1000 << " ms)" << std::endl;
    std::cout << "  P50:          " << r.lat.p50 << " us" << std::endl;
    std::cout << "  P95:          " << r.lat.p95 << " us" << std::endl;
    std::cout << "  P99:          " << r.lat.p99 << " us" << std::endl;
    std::cout << "  QPS:          " << r.qps << std::endl;
    std::cout << "  Cache hit:    " << r.hit_rate << "%" << std::endl;
    std::cout << "  RSS:          " << r.rss_mb << " MB" << std::endl;
    std::cout << "  PF submitted: " << r.pf_submitted << std::endl;
    std::cout << "  PF skipped:   " << r.pf_skipped << std::endl;
}

void printComparison(const std::vector<BenchResult>& results, int pipeline_depth) {
    std::cout << "\n\n========== Pipeline Search Benchmark (depth=" << pipeline_depth << ") ==========" << std::endl;
    std::cout << "\n| Config | Mean(ms) | P99(ms) | QPS | Hit% | RSS(MB) | R@HNSW | PF_sub | PF_skip |" << std::endl;
    std::cout << "|--------|----------|---------|-----|------|---------|--------|--------|---------|" << std::endl;
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << std::fixed << std::setprecision(2) << r.lat.mean / 1000
                  << " | " << r.lat.p99 / 1000
                  << " | " << std::setprecision(1) << r.qps
                  << " | " << r.hit_rate
                  << " | " << r.rss_mb
                  << " | " << std::setprecision(2) << r.recall_hnsw << "%"
                  << " | " << r.pf_submitted
                  << " | " << r.pf_skipped << " |" << std::endl;
    }

    if (results.size() >= 2) {
        auto& base = results[0];
        std::cout << "\nRelative to baseline (sequential):" << std::endl;
        std::cout << "| Config | Latency(x) | QPS(%) | Mem(MB) | Mem Saved |" << std::endl;
        std::cout << "|--------|-----------|--------|---------|-----------|" << std::endl;
        for (size_t i = 1; i < results.size(); i++) {
            auto& r = results[i];
            double lat_r = r.lat.mean / base.lat.mean;
            double qps_r = r.qps / base.qps * 100;
            double mem_s = 100.0 - (double)r.rss_mb / base.rss_mb * 100;
            std::cout << "| " << r.name
                      << " | " << std::setprecision(2) << lat_r << "x"
                      << " | " << qps_r << "%"
                      << " | " << r.rss_mb
                      << " | " << mem_s << "% |" << std::endl;
        }
    }
    std::cout << "\n=========================================================\n" << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50] [num_queries=200] [pipeline_depth=4]"
                  << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string blocks_path = argv[3];
    std::string route_path = argv[4];
    std::string data_path = argv[5];
    std::string query_path = argv[6];
    std::string gt_path = argv[7];
    int k = argc > 8 ? std::atoi(argv[8]) : 10;
    int ef = argc > 9 ? std::atoi(argv[9]) : 50;
    size_t num_queries = argc > 10 ? (size_t)std::atoll(argv[10]) : 200;
    int pipeline_depth = argc > 11 ? std::atoi(argv[11]) : 4;

    if (pipeline_depth < 1) pipeline_depth = 1;

    std::cout << "=== Pipeline Search Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef << ", num_queries=" << num_queries
              << ", pipeline_depth=" << pipeline_depth << std::endl;

    // Load data
    std::cout << "\n[1] Loading data..." << std::endl;
    int dim;
    size_t num_base;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_queries);
    if (query_data.size() / dim < num_queries) num_queries = query_data.size() / dim;
    auto gt_data = read_gt(gt_path, num_queries, k);
    std::cout << "  Base: " << num_base << ", Query: " << num_queries << ", dim=" << dim << std::endl;
    std::vector<float>().swap(base_data);

    // Get num_blocks
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    std::cout << "  Blocks: " << num_blocks << std::endl;

    std::vector<BenchResult> all_results;
    std::vector<std::vector<SearchResult>> hnsw_baseline;
    auto trim = []() { malloc_trim(0); };

    // ---- G0: Full-memory baseline ----
    std::cout << "\n[2] G0: Full-memory..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t q = 0; q < num_queries; q++) {
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_s = std::chrono::duration<double>(t1 - t0).count();

        BenchResult r;
        r.name = "F0: Full-mem";
        r.lat.mean = (total_s / num_queries) * 1e6;
        r.lat.p50 = r.lat.mean;
        r.lat.p95 = r.lat.mean;
        r.lat.p99 = r.lat.mean;
        r.qps = num_queries / total_s;
        r.rss_mb = getRSS_MB();
        r.recall_hnsw = 100.0;
        r.hit_rate = 100.0;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G1: Sequential F2 (c1024+GP, baseline) ----
    std::cout << "\n[3] G1: Sequential F2 (c1024+GP)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        // Use OD c1024
        IOConfig io_config;
        io_config.use_odirect = true;
        io_config.drop_page_cache = true;
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy), 1024, dim, io_config);
        DiskHNSW hnsw_cache(graph_path, bfs_path, std::move(cache));
        hnsw_cache.setEf(ef);
        hnsw_cache.enableGraphPrefetch(true);

        // Warmup
        for (size_t q = 0; q < std::min(10UL, num_queries); q++) {
            hnsw_cache.searchKnn(&query_data[q * dim], k);
        }

        hnsw_cache.resetCacheStats();
        hnsw_cache.resetGraphPrefetchStats();

        std::vector<double> latencies(num_queries);
        std::vector<std::vector<SearchResult>> results(num_queries);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t q = 0; q < num_queries; q++) {
            auto q0 = std::chrono::high_resolution_clock::now();
            results[q] = hnsw_cache.searchKnn(&query_data[q * dim], k);
            auto q1 = std::chrono::high_resolution_clock::now();
            latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_s = std::chrono::duration<double>(t1 - t0).count();

        BenchResult r;
        r.name = "G1: Seq F2";
        r.lat = computeLatency(latencies);
        r.qps = num_queries / total_s;
        r.rss_mb = getRSS_MB();

        // Recall: use stored results from the timed loop, skip re-search
        // (re-search adds unnecessary latency and reloads from cache)
        size_t correct = 0;
        for (size_t q = 0; q < num_queries; q++) {
            std::set<uint64_t> hnsw_set;
            for (const auto& [d, id] : hnsw_baseline[q]) hnsw_set.insert(id);
            for (const auto& [d, id] : results[q]) {
                if (hnsw_set.count(id)) correct++;
            }
        }
        r.recall_hnsw = (double)correct / (num_queries * k) * 100;

        auto& stats = hnsw_cache.getCacheStats();
        size_t total = stats.total_accesses.load();
        r.hit_rate = total > 0 ? (double)stats.cache_hits.load() / total * 100 : 0;

        auto& pf_stats = hnsw_cache.getGraphPrefetchStats();
        r.pf_submitted = pf_stats.prefetch_submitted;
        r.pf_skipped = pf_stats.prefetch_skipped;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G2: Pipeline F2 (c1024+GP, pipeline_depth) ----
    std::cout << "\n[4] G2: Pipeline F2 (c1024+GP, depth=" << pipeline_depth << ")..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        // Use OD c1024
        IOConfig io_config;
        io_config.use_odirect = true;
        io_config.drop_page_cache = true;
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy), 1024, dim, io_config);
        DiskHNSW hnsw_cache(graph_path, bfs_path, std::move(cache));
        hnsw_cache.setEf(ef);
        hnsw_cache.enableGraphPrefetch(true);

        // Warmup (sequential search to warm cache)
        for (size_t q = 0; q < std::min(10UL, num_queries); q++) {
            hnsw_cache.searchKnn(&query_data[q * dim], k);
        }

        hnsw_cache.resetCacheStats();
        hnsw_cache.resetGraphPrefetchStats();

        std::vector<double> latencies(num_queries);
        auto t0 = std::chrono::high_resolution_clock::now();
        auto results = hnsw_cache.pipelineSearch(query_data, k, pipeline_depth);
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_s = std::chrono::duration<double>(t1 - t0).count();

        BenchResult r;
        r.name = "G2: Pipe F2";
        r.lat.mean = (total_s / num_queries) * 1e6;
        r.lat.p50 = r.lat.mean;
        r.lat.p95 = r.lat.mean;
        r.lat.p99 = r.lat.mean;
        r.qps = num_queries / total_s;
        r.rss_mb = getRSS_MB();

        // Recall
        size_t correct = 0;
        for (size_t q = 0; q < num_queries; q++) {
            std::set<uint64_t> hnsw_set;
            for (const auto& [d, id] : hnsw_baseline[q]) hnsw_set.insert(id);
            for (const auto& [d, id] : results[q]) {
                if (hnsw_set.count(id)) correct++;
            }
        }
        r.recall_hnsw = (double)correct / (num_queries * k) * 100;

        auto& stats = hnsw_cache.getCacheStats();
        size_t total = stats.total_accesses.load();
        r.hit_rate = total > 0 ? (double)stats.cache_hits.load() / total * 100 : 0;

        auto& pf_stats = hnsw_cache.getGraphPrefetchStats();
        r.pf_submitted = pf_stats.prefetch_submitted;
        r.pf_skipped = pf_stats.prefetch_skipped;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G3: Pipeline F2 with smaller cache (c256) ----
    std::cout << "\n[5] G3: Pipeline F2 (c256+GP, depth=" << pipeline_depth << ")..." << std::endl;
    {
        IOConfig io_config;
        io_config.use_odirect = true;
        io_config.drop_page_cache = true;
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy), 256, dim, io_config);
        DiskHNSW hnsw_cache(graph_path, bfs_path, std::move(cache));
        hnsw_cache.setEf(ef);
        hnsw_cache.enableGraphPrefetch(true);

        // Warmup
        for (size_t q = 0; q < std::min(10UL, num_queries); q++) {
            hnsw_cache.searchKnn(&query_data[q * dim], k);
        }

        hnsw_cache.resetCacheStats();
        hnsw_cache.resetGraphPrefetchStats();

        auto t0 = std::chrono::high_resolution_clock::now();
        auto results = hnsw_cache.pipelineSearch(query_data, k, pipeline_depth);
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_s = std::chrono::duration<double>(t1 - t0).count();

        BenchResult r;
        r.name = "G3: Pipe c256";
        r.lat.mean = (total_s / num_queries) * 1e6;
        r.lat.p50 = r.lat.mean;
        r.lat.p95 = r.lat.mean;
        r.lat.p99 = r.lat.mean;
        r.qps = num_queries / total_s;
        r.rss_mb = getRSS_MB();

        size_t correct = 0;
        for (size_t q = 0; q < num_queries; q++) {
            std::set<uint64_t> hnsw_set;
            for (const auto& [d, id] : hnsw_baseline[q]) hnsw_set.insert(id);
            for (const auto& [d, id] : results[q]) {
                if (hnsw_set.count(id)) correct++;
            }
        }
        r.recall_hnsw = (double)correct / (num_queries * k) * 100;

        auto& stats = hnsw_cache.getCacheStats();
        size_t total = stats.total_accesses.load();
        r.hit_rate = total > 0 ? (double)stats.cache_hits.load() / total * 100 : 0;

        auto& pf_stats = hnsw_cache.getGraphPrefetchStats();
        r.pf_submitted = pf_stats.prefetch_submitted;
        r.pf_skipped = pf_stats.prefetch_skipped;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G4: Sequential F2 with smaller cache (c256) for comparison ----
    std::cout << "\n[6] G4: Sequential F2 (c256+GP)..." << std::endl;
    {
        IOConfig io_config;
        io_config.use_odirect = true;
        io_config.drop_page_cache = true;
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy), 256, dim, io_config);
        DiskHNSW hnsw_cache(graph_path, bfs_path, std::move(cache));
        hnsw_cache.setEf(ef);
        hnsw_cache.enableGraphPrefetch(true);

        for (size_t q = 0; q < std::min(10UL, num_queries); q++) {
            hnsw_cache.searchKnn(&query_data[q * dim], k);
        }

        hnsw_cache.resetCacheStats();
        hnsw_cache.resetGraphPrefetchStats();

        std::vector<double> latencies(num_queries);
        std::vector<std::vector<SearchResult>> results(num_queries);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (size_t q = 0; q < num_queries; q++) {
            auto q0 = std::chrono::high_resolution_clock::now();
            results[q] = hnsw_cache.searchKnn(&query_data[q * dim], k);
            auto q1 = std::chrono::high_resolution_clock::now();
            latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_s = std::chrono::duration<double>(t1 - t0).count();

        BenchResult r;
        r.name = "G4: Seq c256";
        r.lat = computeLatency(latencies);
        r.qps = num_queries / total_s;
        r.rss_mb = getRSS_MB();

        size_t correct = 0;
        for (size_t q = 0; q < num_queries; q++) {
            std::set<uint64_t> hnsw_set;
            for (const auto& [d, id] : hnsw_baseline[q]) hnsw_set.insert(id);
            for (const auto& [d, id] : results[q]) {
                if (hnsw_set.count(id)) correct++;
            }
        }
        r.recall_hnsw = (double)correct / (num_queries * k) * 100;

        auto& stats = hnsw_cache.getCacheStats();
        size_t total = stats.total_accesses.load();
        r.hit_rate = total > 0 ? (double)stats.cache_hits.load() / total * 100 : 0;

        auto& pf_stats = hnsw_cache.getGraphPrefetchStats();
        r.pf_submitted = pf_stats.prefetch_submitted;
        r.pf_skipped = pf_stats.prefetch_skipped;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    printComparison(all_results, pipeline_depth);
    return 0;
}