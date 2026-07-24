// benchmark_full.cpp - Comprehensive Multi-Config Multi-QuerySet Benchmark
//
// Tests 7 configurations across 4 query sets (100, 500, 1000, 1000+shuffle)
//
// Configurations:
//   F0: Full-memory (cache=num_blocks, no prefetch) - upper bound
//   F1: OD c1024, no prefetch
//   F2: OD c1024 + Graph Prefetch
//   F3: OD c512, no prefetch
//   F4: OD c512 + Graph Prefetch
//   F5: OD c256, no prefetch
//   F6: OD c256 + Graph Prefetch
//
// Usage:
//   ./benchmark_full <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50] [num_queries=all] [shuffle=0]
//
// For batch testing, use --run-all which runs 100/500/1000/1000+shuffle automatically.

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
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>
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
    std::string config_id;
    double recall_gt = 0;
    double recall_hnsw = 0;
    LatencyStats lat{};
    double qps = 0;
    double hit_rate = 0;
    size_t rss_mb = 0;
    size_t cache_slots = 0;
    size_t cache_mb = 0;
    std::string prefetch_mode = "none";
    size_t pf_submitted = 0;
    size_t pf_completed = 0;
    size_t pf_skipped = 0;
};

BenchResult runBenchmark(
    DiskHNSW& hnsw,
    const std::vector<float>& queries,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    int dim, int k, const std::string& name, const std::string& config_id,
    const std::string& mode)
{
    BenchResult r;
    r.name = name;
    r.config_id = config_id;
    r.prefetch_mode = mode;

    size_t nq = queries.size() / dim;
    std::vector<double> latencies(nq);
    std::vector<std::vector<SearchResult>> results(nq);

    hnsw.resetCacheStats();
    if (mode.find("graph") != std::string::npos) hnsw.resetGraphPrefetchStats();

    // Warmup (10 queries or nq, whichever is smaller)
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
    if (mode.find("graph") != std::string::npos) {
        auto& pf = hnsw.getGraphPrefetchStats();
        r.pf_submitted = pf.prefetch_submitted;
        r.pf_completed = pf.prefetch_completed;
        r.pf_skipped = pf.prefetch_skipped;
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
    if (r.prefetch_mode.find("graph") != std::string::npos) {
        std::cout << "  Graph PF:     submitted=" << r.pf_submitted
                  << " completed=" << r.pf_completed
                  << " skipped=" << r.pf_skipped << std::endl;
    }
}

// Run all 7 configs for a given query set
std::vector<BenchResult> runAllConfigs(
    const std::string& graph_path,
    const std::string& bfs_path,
    const std::string& blocks_path,
    const std::string& route_path,
    const std::vector<float>& query_data,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    uint32_t num_blocks, uint32_t block_size, int dim, int k, int ef)
{
    std::vector<BenchResult> results;
    auto cache_mb = [&](size_t slots) { return slots * block_size / (1024*1024); };
    auto trim = []() { malloc_trim(0); };

    IOConfig odirect_config;
    odirect_config.use_odirect = true;
    odirect_config.drop_page_cache = true;

    // ---- F0: Full-memory ----
    std::cout << "\n  [F0] Full-memory (cache=" << num_blocks << ")..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F0: Full", "F0", "none");
        r.cache_slots = num_blocks;
        r.cache_mb = cache_mb(num_blocks);
        printResult(r);
        results.push_back(r);
    }
    trim();

    // ---- F1: OD c1024, no prefetch (SKIP - user requested F0 vs F2 only) ----
    // ---- F2: OD c1024 + graph prefetch ----
    std::cout << "\n  [F2] OD c1024 + graph prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            1024, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        hnsw.enableGraphPrefetch(true);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F2: c1024+gp", "F2", "graph");
        r.cache_slots = 1024;
        r.cache_mb = cache_mb(1024);
        printResult(r);
        results.push_back(r);
    }
    trim();

    // ---- F3-F6: SKIPPED (user requested F0 vs F2 only) ----

    return results;
}

void printComparisonTable(const std::vector<BenchResult>& results) {
    std::cout << "\n  | Config | Mean(ms) | P50(ms) | P95(ms) | P99(ms) | QPS | Hit% | RSS(MB) | Cache(MB) | R@HNSW | PF |" << std::endl;
    std::cout << "  |--------|----------|---------|---------|---------|-----|------|---------|-----------|--------|----|" << std::endl;
    for (const auto& r : results) {
        std::cout << "  | " << r.name
                  << " | " << std::fixed << std::setprecision(2) << r.lat.mean / 1000
                  << " | " << r.lat.p50 / 1000
                  << " | " << r.lat.p95 / 1000
                  << " | " << r.lat.p99 / 1000
                  << " | " << std::setprecision(1) << r.qps
                  << " | " << r.hit_rate
                  << " | " << r.rss_mb
                  << " | " << r.cache_mb
                  << " | " << std::setprecision(2) << r.recall_hnsw << "%"
                  << " | " << r.prefetch_mode << " |" << std::endl;
    }
}

void saveJSON(const std::string& path,
              const std::string& query_set_name,
              size_t num_queries, bool shuffled,
              int k, int ef,
              const std::vector<BenchResult>& results) {
    std::ofstream jout(path, std::ios::app);
    jout << std::fixed << std::setprecision(4);
    jout << "{\"query_set\":\"" << query_set_name << "\""
         << ",\"num_queries\":" << num_queries
         << ",\"shuffled\":" << (shuffled ? "true" : "false")
         << ",\"k\":" << k << ",\"ef\":" << ef
         << ",\"results\":[";
    for (size_t i = 0; i < results.size(); i++) {
        auto& r = results[i];
        if (i > 0) jout << ",";
        jout << "{\"config_id\":\"" << r.config_id << "\""
             << ",\"name\":\"" << r.name << "\""
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
             << ",\"cache_mb\":" << r.cache_mb
             << ",\"pf_mode\":\"" << r.prefetch_mode << "\""
             << ",\"pf_submitted\":" << r.pf_submitted
             << ",\"pf_completed\":" << r.pf_completed
             << ",\"pf_skipped\":" << r.pf_skipped
             << "}";
    }
    jout << "]}" << std::endl;
    jout.close();
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph> <bfs> <blocks> <route> <data> <query> <gt>"
                  << " [k=10] [ef=50] [num_queries=all] [shuffle=0]"
                  << std::endl;
        std::cerr << "  Set num_queries=0 for all queries in the file." << std::endl;
        std::cerr << "  shuffle=1 shuffles query order with seed 42." << std::endl;
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
    size_t num_queries = (argc > 10) ? (size_t)std::atoll(argv[10]) : 0;  // 0 = all
    bool shuffle = (argc > 11) ? std::atoi(argv[11]) != 0 : false;

    std::cout << "=== Full Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef
              << ", num_queries=" << (num_queries == 0 ? "all" : std::to_string(num_queries))
              << ", shuffle=" << (shuffle ? "yes" : "no") << std::endl;

    // Load data
    std::cout << "\n[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    std::vector<float>().swap(base_data);  // free base data

    // Truncate queries
    if (num_queries > 0 && num_queries < num_query) {
        query_data.resize(num_queries * dim);
        num_query = num_queries;
    }

    // Shuffle queries if requested (seed=42 for reproducibility)
    std::string query_set_name = "q" + std::to_string(num_query);
    if (shuffle) {
        std::mt19937 rng(42);
        std::vector<size_t> idx(num_query);
        for (size_t i = 0; i < num_query; i++) idx[i] = i;
        std::shuffle(idx.begin(), idx.end(), rng);

        std::vector<float> shuffled_data(num_query * dim);
        for (size_t i = 0; i < num_query; i++) {
            std::memcpy(&shuffled_data[i * dim], &query_data[idx[i] * dim], dim * sizeof(float));
        }
        query_data = std::move(shuffled_data);
        query_set_name += "_shuffle";
    }

    auto gt_data = read_gt(gt_path, num_query, k);
    std::cout << "  Base: " << num_base << ", Query: " << num_query
              << ", dim=" << dim
              << ", QuerySet: " << query_set_name << std::endl;

    // Get block info
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    uint32_t block_size = bfhdr.block_size;
    std::cout << "  Blocks: " << num_blocks << ", block_size=" << block_size << std::endl;

    // Build HNSW baseline (full-memory, used for recall@HNSW comparison)
    std::cout << "\n[2] Building HNSW baseline (full-memory)..." << std::endl;
    std::vector<std::vector<SearchResult>> hnsw_baseline;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        for (size_t q = 0; q < num_query; q++)
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
    }
    std::cout << "  Baseline built." << std::endl;

    // Run all configs
    std::cout << "\n[3] Running 7 configurations..." << std::endl;
    auto results = runAllConfigs(graph_path, bfs_path, blocks_path, route_path,
                                 query_data, gt_data, hnsw_baseline,
                                 num_blocks, block_size, dim, k, ef);

    // Print comparison
    printComparisonTable(results);

    // Save JSON
    std::string json_path = "logs/full_benchmark_" + query_set_name + ".jsonl";
    // Clear file first
    { std::ofstream f(json_path, std::ios::trunc); }
    saveJSON(json_path, query_set_name, num_query, shuffle, k, ef, results);
    std::cout << "\n  JSON saved to " << json_path << std::endl;

    // Verify recall
    bool all_ok = true;
    for (const auto& r : results) {
        if (r.recall_hnsw < 99.99) {
            std::cerr << "  WARNING: " << r.name << " recall@HNSW = " << r.recall_hnsw << "% < 100%" << std::endl;
            all_ok = false;
        }
    }
    if (all_ok) {
        std::cout << "\n  ✅ All configs recall@HNSW = 100%" << std::endl;
    }

    return 0;
}
