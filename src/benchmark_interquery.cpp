// benchmark_interquery.cpp - 查询间预取 benchmark
//
// G0: Full-memory (baseline)
// G1: c64, cached I/O, no prefetch (baseline)
// G2: c64, cached I/O + inter-query prefetch (last 1 query)
// G3: c64, cached I/O + inter-query prefetch (last 5 queries)
// G4: c256, cached I/O + inter-query prefetch (last 5 queries)
// G5: c64, cached I/O + inter-query prefetch (last 5) + in-query graph prefetch
//
// 用法:
//   ./benchmark_interquery <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50]

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
    size_t cache_slots = 0;
    int prefetch_window = 0;  // N for last-N-queries prefetch
    bool graph_prefetch = false;
};

// Run benchmark with inter-query prefetch
BenchResult runBenchmark(
    DiskHNSW& hnsw,
    const std::vector<float>& queries,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    int dim, int k, const std::string& name,
    int prefetch_window, bool graph_prefetch)
{
    BenchResult r;
    r.name = name;
    r.prefetch_window = prefetch_window;
    r.graph_prefetch = graph_prefetch;

    size_t nq = queries.size() / dim;
    std::vector<double> latencies(nq);
    std::vector<std::vector<SearchResult>> results(nq);

    hnsw.resetCacheStats();
    if (graph_prefetch) hnsw.resetGraphPrefetchStats();

    // History of block sets for inter-query prefetch
    std::vector<std::set<uint32_t>> block_history;

    // Warmup (10 queries, build initial history)
    for (size_t q = 0; q < std::min(10UL, nq); q++) {
        hnsw.startRecordingBlocks();
        hnsw.searchKnn(&queries[q * dim], k);
        hnsw.stopRecordingBlocks();
        block_history.push_back(hnsw.getRecordedBlocks());
    }

    // Timed search with inter-query prefetch
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t q = 0; q < nq; q++) {
        // Inter-query prefetch: prefetch union of last N queries' blocks
        if (prefetch_window > 0 && !block_history.empty()) {
            hnsw.prefetchRecentBlocks(block_history, prefetch_window);
        }

        // Record blocks accessed during this query
        hnsw.startRecordingBlocks();

        auto q0 = std::chrono::high_resolution_clock::now();
        results[q] = hnsw.searchKnn(&queries[q * dim], k);
        auto q1 = std::chrono::high_resolution_clock::now();

        hnsw.stopRecordingBlocks();
        latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();

        // Add this query's blocks to history
        block_history.push_back(hnsw.getRecordedBlocks());
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double total_s = std::chrono::duration<double>(t1 - t0).count();

    r.lat = computeLatency(latencies);
    r.qps = nq / total_s;
    r.rss_mb = getRSS_MB();

    // Recall
    size_t correct_hnsw = 0;
    for (size_t q = 0; q < nq; q++) {
        std::set<uint64_t> hnsw_set;
        for (const auto& [d, id] : hnsw_baseline[q]) hnsw_set.insert(id);
        for (const auto& [d, id] : results[q]) {
            if (hnsw_set.count(id)) correct_hnsw++;
        }
    }
    r.recall_hnsw = (double)correct_hnsw / (nq * k) * 100;

    auto& stats = hnsw.getCacheStats();
    size_t total = stats.total_accesses.load();
    r.hit_rate = total > 0 ? (double)stats.cache_hits.load() / total * 100 : 0;

    return r;
}

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
    std::cout << "  PF window:    " << r.prefetch_window << std::endl;
    std::cout << "  Graph PF:     " << (r.graph_prefetch ? "ON" : "OFF") << std::endl;
}

void printComparison(const std::vector<BenchResult>& results) {
    std::cout << "\n\n========== Inter-Query Prefetch Benchmark ==========" << std::endl;
    std::cout << "\n| Config | Mean(ms) | P99(ms) | QPS | Hit% | RSS(MB) | R@HNSW | PF Win |" << std::endl;
    std::cout << "|--------|----------|---------|-----|------|---------|--------|--------|" << std::endl;
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << std::fixed << std::setprecision(2) << r.lat.mean / 1000
                  << " | " << r.lat.p99 / 1000
                  << " | " << std::setprecision(1) << r.qps
                  << " | " << r.hit_rate
                  << " | " << r.rss_mb
                  << " | " << std::setprecision(2) << r.recall_hnsw << "%"
                  << " | " << r.prefetch_window << " |" << std::endl;
    }

    if (!results.empty()) {
        auto& base = results[0];
        std::cout << "\nRelative to G0 (full-memory):" << std::endl;
        std::cout << "| Config | Latency(x) | QPS(%) | Mem(MB) | Mem Saved |" << std::endl;
        std::cout << "|--------|-----------|--------|---------|-----------|" << std::endl;
        for (const auto& r : results) {
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
    std::cout << "\n====================================================\n" << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50]"
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

    std::cout << "=== Inter-Query Prefetch Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef << std::endl;

    // Load data
    std::cout << "\n[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    if (num_query > 200) {
        query_data.resize(200 * dim);
        num_query = 200;
    }
    auto gt_data = read_gt(gt_path, num_query, k);
    std::cout << "  Base: " << num_base << ", Query: " << num_query << ", dim=" << dim << std::endl;
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

    // ---- G0: Full-memory ----
    std::cout << "\n[2] G0: Full-memory..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        for (size_t q = 0; q < num_query; q++) {
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
        }
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "G0: Full-mem", 0, false);
        r.cache_slots = num_blocks;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G1: c64, no prefetch ----
    std::cout << "\n[3] G1: c64, no prefetch..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "G1: c64 nopf", 0, false);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G2: c64, inter-query prefetch (last 1) ----
    std::cout << "\n[4] G2: c64, prefetch(1)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "G2: c64 pf(1)", 1, false);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G3: c64, inter-query prefetch (last 5) ----
    std::cout << "\n[5] G3: c64, prefetch(5)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "G3: c64 pf(5)", 5, false);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G4: c256, inter-query prefetch (last 5) ----
    std::cout << "\n[6] G4: c256, prefetch(5)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 256, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "G4: c256 pf(5)", 5, false);
        r.cache_slots = 256;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- G5: c64, inter-query prefetch(5) + graph prefetch ----
    std::cout << "\n[7] G5: c64, prefetch(5) + graph pf..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        hnsw.enableGraphPrefetch(false);  // no O_DIRECT, just cached I/O
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "G5: c64 pf(5)+gpf", 5, true);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    printComparison(all_results);
    return 0;
}
