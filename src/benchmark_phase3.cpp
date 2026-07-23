// benchmark_phase3.cpp - Phase 3 Redesign Benchmark
//
// E0: Full-memory (cache = num_blocks, 全部在 DRAM)
// E1: DiskHNSW O_DIRECT, cache=64, 无预取
// E2: DiskHNSW O_DIRECT, cache=64, 图引导预取
// E3: DiskHNSW O_DIRECT, cache=256, 无预取
// E4: DiskHNSW O_DIRECT, cache=256, 图引导预取
//
// 用法:
//   ./benchmark_phase3 <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50]

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
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

using SearchResult = std::pair<float, uint64_t>;

// ============================================================
// Utility
// ============================================================

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

struct LatencyStats { double mean, p50, p95, p99, total_ms; };

static LatencyStats computeLatency(std::vector<double>& times) {
    std::sort(times.begin(), times.end());
    double sum = 0;
    for (double t : times) sum += t;
    size_t n = times.size();
    return { sum/n, times[n/2], times[(size_t)(n*0.95)], times[(size_t)(n*0.99)], sum/1000.0 };
}

// ============================================================
// Result
// ============================================================

struct BenchResult {
    std::string name;
    double recall_gt = 0;
    double recall_hnsw = 0;
    LatencyStats lat{};
    double qps = 0;
    double hit_rate = 0;
    size_t rss_mb = 0;
    size_t cache_slots = 0;
    bool prefetch = false;
    // prefetch stats
    size_t pf_submitted = 0;
    size_t pf_completed = 0;
    size_t pf_skipped = 0;
    size_t pf_failed = 0;
};

// ============================================================
// Run benchmark
// ============================================================

BenchResult runBenchmark(
    DiskHNSW& hnsw,
    const std::vector<float>& queries,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    int dim, int k, const std::string& name, bool prefetch)
{
    BenchResult r;
    r.name = name;
    r.prefetch = prefetch;

    size_t nq = queries.size() / dim;
    std::vector<double> latencies(nq);
    std::vector<std::vector<SearchResult>> results(nq);

    hnsw.resetCacheStats();
    if (prefetch) hnsw.resetGraphPrefetchStats();

    // Warmup (10 queries)
    for (size_t q = 0; q < std::min(10UL, nq); q++) {
        hnsw.searchKnn(&queries[q * dim], k);
    }

    // Timed search
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t q = 0; q < nq; q++) {
        auto q0 = std::chrono::high_resolution_clock::now();
        results[q] = hnsw.searchKnn(&queries[q * dim], k);
        auto q1 = std::chrono::high_resolution_clock::now();
        latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double total_s = std::chrono::duration<double>(t1 - t0).count();

    r.lat = computeLatency(latencies);
    r.qps = nq / total_s;
    r.rss_mb = getRSS_MB();

    // Recall
    size_t correct_gt = 0, correct_hnsw = 0;
    for (size_t q = 0; q < nq; q++) {
        std::set<uint64_t> gt_set(gt_data[q].begin(), gt_data[q].end());
        std::set<uint64_t> hnsw_set;
        for (const auto& [d, id] : hnsw_baseline[q]) hnsw_set.insert(id);
        for (const auto& [d, id] : results[q]) {
            if (gt_set.count(id)) correct_gt++;
            if (hnsw_set.count(id)) correct_hnsw++;
        }
    }
    r.recall_gt = (double)correct_gt / (nq * k) * 100;
    r.recall_hnsw = (double)correct_hnsw / (nq * k) * 100;

    // Cache stats
    auto& stats = hnsw.getCacheStats();
    size_t total = stats.total_accesses.load();
    r.hit_rate = total > 0 ? (double)stats.cache_hits.load() / total * 100 : 0;

    // Prefetch stats
    if (prefetch) {
        auto& pf = hnsw.getGraphPrefetchStats();
        r.pf_submitted = pf.prefetch_submitted;
        r.pf_completed = pf.prefetch_completed;
        r.pf_skipped = pf.prefetch_skipped;
        r.pf_failed = pf.prefetch_failed;
    }

    return r;
}

void printResult(const BenchResult& r) {
    std::cout << "\n=== " << r.name << " ===" << std::endl;
    std::cout << "  Recall@GT:    " << r.recall_gt << "%" << std::endl;
    std::cout << "  Recall@HNSW:  " << r.recall_hnsw << "%" << std::endl;
    std::cout << "  Mean:         " << r.lat.mean << " us (" << r.lat.mean/1000 << " ms)" << std::endl;
    std::cout << "  P50:          " << r.lat.p50 << " us" << std::endl;
    std::cout << "  P95:          " << r.lat.p95 << " us" << std::endl;
    std::cout << "  P99:          " << r.lat.p99 << " us" << std::endl;
    std::cout << "  QPS:          " << r.qps << std::endl;
    std::cout << "  Cache hit:    " << r.hit_rate << "%" << std::endl;
    std::cout << "  RSS:          " << r.rss_mb << " MB" << std::endl;
    if (r.prefetch) {
        std::cout << "  Prefetch:     submitted=" << r.pf_submitted
                  << " completed=" << r.pf_completed
                  << " skipped=" << r.pf_skipped
                  << " failed=" << r.pf_failed << std::endl;
    }
}

void printComparison(const std::vector<BenchResult>& results) {
    std::cout << "\n\n========== Phase 3 Redesign Benchmark ==========" << std::endl;
    std::cout << "\n| Config | Mean(ms) | P99(ms) | QPS | Hit% | RSS(MB) | R@HNSW | Prefetch |" << std::endl;
    std::cout << "|--------|----------|---------|-----|------|---------|--------|----------|" << std::endl;
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << std::fixed << std::setprecision(2) << r.lat.mean / 1000
                  << " | " << r.lat.p99 / 1000
                  << " | " << std::setprecision(1) << r.qps
                  << " | " << std::setprecision(1) << r.hit_rate
                  << " | " << r.rss_mb
                  << " | " << std::setprecision(2) << r.recall_hnsw << "%"
                  << " | " << (r.prefetch ? "ON" : "OFF") << " |" << std::endl;
    }

    if (!results.empty()) {
        auto& base = results[0];  // E0
        std::cout << "\nRelative to E0 (full-memory):" << std::endl;
        std::cout << "| Config | Latency(x) | QPS(%) | Mem(%) | Mem Saved |" << std::endl;
        std::cout << "|--------|-----------|--------|--------|-----------|" << std::endl;
        for (const auto& r : results) {
            double lat_r = r.lat.mean / base.lat.mean;
            double qps_r = r.qps / base.qps * 100;
            double mem_r = (double)r.rss_mb / base.rss_mb * 100;
            double mem_s = 100.0 - mem_r;
            std::cout << "| " << r.name
                      << " | " << std::setprecision(2) << lat_r << "x"
                      << " | " << qps_r << "%"
                      << " | " << mem_r << "%"
                      << " | " << mem_s << "% |" << std::endl;
        }
    }

    // Prefetch comparison (E1 vs E2, E3 vs E4)
    if (results.size() >= 5) {
        std::cout << "\nPrefetch effectiveness:" << std::endl;
        std::cout << "| Pair | No-PF Mean(ms) | PF Mean(ms) | Speedup | Hit% No-PF | Hit% PF |" << std::endl;
        std::cout << "|------|----------------|-------------|---------|------------|---------|" << std::endl;

        auto cmp = [](const BenchResult& a, const BenchResult& b) {
            std::cout << "| " << a.name << " vs " << b.name
                      << " | " << std::setprecision(2) << a.lat.mean / 1000
                      << " | " << b.lat.mean / 1000
                      << " | " << a.lat.mean / b.lat.mean << "x"
                      << " | " << a.hit_rate << "%"
                      << " | " << b.hit_rate << "% |" << std::endl;
        };
        cmp(results[1], results[2]);  // E1 vs E2
        cmp(results[3], results[4]);  // E3 vs E4
    }

    std::cout << "\n===============================================\n" << std::endl;
}

// ============================================================
// Main
// ============================================================

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

    std::cout << "=== Phase 3 Redesign Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef << std::endl;

    // Load data
    std::cout << "\n[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    if (num_query > 100) {
        query_data.resize(100 * dim);
        num_query = 100;
    }
    auto gt_data = read_gt(gt_path, num_query, k);
    std::cout << "  Base: " << num_base << ", Query: " << num_query << ", dim=" << dim << std::endl;

    // 释放 base_data 节省内存（DiskHNSW 不需要全量向量）
    std::vector<float>().swap(base_data);
    std::cout << "  base_data freed" << std::endl;

    // Get num_blocks from blocks file header
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    uint32_t block_size = bfhdr.block_size;
    std::cout << "  Blocks: " << num_blocks << ", block_size=" << block_size << std::endl;

    std::vector<BenchResult> all_results;
    std::vector<std::vector<SearchResult>> hnsw_baseline;

    IOConfig odirect_config;
    odirect_config.use_odirect = true;
    odirect_config.drop_page_cache = true;

    // ---- E0: Full-memory (cache = num_blocks) ----
    std::cout << "\n[2] E0: Full-memory (cache=" << num_blocks << ")..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        for (size_t q = 0; q < num_query; q++) {
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
        }
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "E0: Full-mem", false);
        r.cache_slots = num_blocks;
        printResult(r);
        all_results.push_back(r);
    }

    // ---- E1: O_DIRECT, cache=64, no prefetch ----
    std::cout << "\n[3] E1: O_DIRECT, cache=64, no prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            64, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "E1: OD c64", false);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }

    // ---- E2: O_DIRECT, cache=64, graph prefetch ----
    std::cout << "\n[4] E2: O_DIRECT, cache=64, graph prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            64, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        hnsw.enableGraphPrefetch(true);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "E2: OD c64+pf", true);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }

    // ---- E3: O_DIRECT, cache=256, no prefetch ----
    std::cout << "\n[5] E3: O_DIRECT, cache=256, no prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            256, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "E3: OD c256", false);
        r.cache_slots = 256;
        printResult(r);
        all_results.push_back(r);
    }

    // ---- E4: O_DIRECT, cache=256, graph prefetch ----
    std::cout << "\n[6] E4: O_DIRECT, cache=256, graph prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            256, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        hnsw.enableGraphPrefetch(true);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "E4: OD c256+pf", true);
        r.cache_slots = 256;
        printResult(r);
        all_results.push_back(r);
    }

    printComparison(all_results);

    // Save JSON
    std::ofstream jout("logs/phase3_benchmark.json");
    jout << std::fixed << std::setprecision(4);
    jout << "{\"benchmark\":\"phase3_redesign\","
         << "\"k\":" << k << ",\"ef\":" << ef
         << ",\"num_query\":" << num_query
         << ",\"results\":[";
    for (size_t i = 0; i < all_results.size(); i++) {
        auto& r = all_results[i];
        if (i > 0) jout << ",";
        jout << "{\"name\":\"" << r.name << "\""
             << ",\"recall_gt\":" << r.recall_gt
             << ",\"recall_hnsw\":" << r.recall_hnsw
             << ",\"mean_us\":" << r.lat.mean
             << ",\"p50_us\":" << r.lat.p50
             << ",\"p95_us\":" << r.lat.p95
             << ",\"p99_us\":" << r.lat.p99
             << ",\"qps\":" << r.qps
             << ",\"hit_rate\":" << r.hit_rate
             << ",\"rss_mb\":" << r.rss_mb
             << ",\"cache_slots\":" << r.cache_slots
             << ",\"prefetch\":" << (r.prefetch ? "true" : "false")
             << ",\"pf_submitted\":" << r.pf_submitted
             << ",\"pf_completed\":" << r.pf_completed
             << ",\"pf_skipped\":" << r.pf_skipped
             << "}";
    }
    jout << "]}" << std::endl;
    jout.close();
    std::cout << "JSON saved to logs/phase3_benchmark.json" << std::endl;

    return 0;
}
