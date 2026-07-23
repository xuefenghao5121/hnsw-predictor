// benchmark_d4_prefetch.cpp - D4 (OD c1024) Graph Prefetch Benchmark
//
// F0: Full-memory (cache = num_blocks, pread)
// F1: O_DIRECT c1024, no prefetch
// F2: O_DIRECT c1024 + graph prefetch (io_uring)
// F4: O_DIRECT c512, no prefetch (参照)
// F5: O_DIRECT c512 + graph prefetch (参照)
//
// 用法:
//   ./benchmark_d4_prefetch <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50]

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
    double recall_gt = 0;
    double recall_hnsw = 0;
    LatencyStats lat{};
    double qps = 0;
    double hit_rate = 0;
    size_t rss_mb = 0;
    size_t cache_slots = 0;
    size_t cache_mb = 0;
    std::string prefetch_mode = "none";
    // Graph stats
    size_t pf_submitted = 0;
    size_t pf_completed = 0;
    size_t pf_skipped = 0;
    size_t pf_inflight_end = 0;
};

BenchResult runBenchmark(
    DiskHNSW& hnsw,
    const std::vector<float>& queries,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    int dim, int k, const std::string& name, const std::string& mode)
{
    BenchResult r;
    r.name = name;
    r.prefetch_mode = mode;

    size_t nq = queries.size() / dim;
    std::vector<double> latencies(nq);
    std::vector<std::vector<SearchResult>> results(nq);

    hnsw.resetCacheStats();
    if (mode.find("graph") != std::string::npos) hnsw.resetGraphPrefetchStats();

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

void printPrefetcherDetail(const GraphPrefetcher::Stats& stats) {
    std::cout << "\n  --- GraphPrefetcher Detailed Stats ---" << std::endl;
    std::cout << "  submit_calls:     " << stats.submit_calls << std::endl;
    std::cout << "  total_submit_us:  " << stats.total_submit_us << " (avg=" 
              << (stats.submit_calls > 0 ? stats.total_submit_us / stats.submit_calls : 0) << " us/call)" << std::endl;
    std::cout << "  reap_calls:       " << stats.reap_calls << std::endl;
    std::cout << "  total_reap_us:    " << stats.total_reap_us << " (avg=" 
              << (stats.reap_calls > 0 ? stats.total_reap_us / stats.reap_calls : 0) << " us/call)" << std::endl;
    std::cout << "  wait_calls:       " << stats.wait_calls << std::endl;
    std::cout << "  total_wait_us:    " << stats.total_wait_us << " (avg=" 
              << (stats.wait_calls > 0 ? stats.total_wait_us / stats.wait_calls : 0) << " us/call)" << std::endl;
    double total_pf_time = stats.total_submit_us + stats.total_reap_us + stats.total_wait_us;
    std::cout << "  total_pf_time_us: " << total_pf_time << " (" << total_pf_time/1000 << " ms)" << std::endl;
    std::cout << "  submitted:        " << stats.prefetch_submitted << std::endl;
    std::cout << "  completed:        " << stats.prefetch_completed << std::endl;
    std::cout << "  failed:           " << stats.prefetch_failed << std::endl;
    std::cout << "  skipped:          " << stats.prefetch_skipped << std::endl;
}

void printComparison(const std::vector<BenchResult>& results) {
    std::cout << "\n\n========== D4 Graph Prefetch Benchmark ==========" << std::endl;
    std::cout << "\n| Config | Mean(ms) | P99(ms) | QPS | Hit% | RSS(MB) | Cache(MB) | R@HNSW | PF Mode |" << std::endl;
    std::cout << "|--------|----------|---------|-----|------|---------|-----------|--------|---------|" << std::endl;
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << std::fixed << std::setprecision(2) << r.lat.mean / 1000
                  << " | " << r.lat.p99 / 1000
                  << " | " << std::setprecision(1) << r.qps
                  << " | " << r.hit_rate
                  << " | " << r.rss_mb
                  << " | " << r.cache_mb
                  << " | " << std::setprecision(2) << r.recall_hnsw << "%"
                  << " | " << r.prefetch_mode << " |" << std::endl;
    }

    if (!results.empty()) {
        auto& base = results[0];  // F0
        std::cout << "\nRelative to F0 (full-memory):" << std::endl;
        std::cout << "| Config | Latency(x) | QPS(%) | RSS(MB) | Mem Saved |" << std::endl;
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

    // Prefetch effectiveness comparison (F1 vs F2, F4 vs F5)
    std::cout << "\nPrefetch effectiveness:" << std::endl;
    std::cout << "| Pair | No-PF Mean(ms) | PF Mean(ms) | Speedup | Hit% No-PF | Hit% PF | PF Submitted | PF Completed |" << std::endl;
    std::cout << "|------|----------------|-------------|---------|------------|---------|---------------|--------------|" << std::endl;
    for (size_t i = 0; i < results.size(); i++) {
        if (results[i].prefetch_mode == "none" && i + 1 < results.size()) {
            const auto& noPf = results[i];
            const auto& pf = results[i + 1];
            if (pf.prefetch_mode.find("graph") != std::string::npos) {
                double speedup = noPf.lat.mean / pf.lat.mean;
                std::cout << "| " << noPf.name << " vs " << pf.name
                          << " | " << std::fixed << std::setprecision(2) << noPf.lat.mean / 1000
                          << " | " << pf.lat.mean / 1000
                          << " | " << std::setprecision(2) << speedup << "x"
                          << " | " << noPf.hit_rate
                          << " | " << pf.hit_rate
                          << " | " << pf.pf_submitted
                          << " | " << pf.pf_completed << " |" << std::endl;
            }
        }
    }

    std::cout << "\n===============================================\n" << std::endl;
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
    int k = (argc > 8) ? std::atoi(argv[8]) : 10;
    int ef = (argc > 9) ? std::atoi(argv[9]) : 50;

    std::cout << "=== D4 Graph Prefetch Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef << std::endl;

    // Load data
    std::cout << "\n[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    if (num_query > 200) { query_data.resize(200 * dim); num_query = 200; }
    auto gt_data = read_gt(gt_path, num_query, k);
    std::cout << "  Base: " << num_base << ", Query: " << num_query << ", dim=" << dim << std::endl;
    std::vector<float>().swap(base_data);

    // Get block info
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    uint32_t block_size = bfhdr.block_size;
    std::cout << "  Blocks: " << num_blocks << ", block_size=" << block_size << std::endl;

    auto cache_mb = [&](size_t slots) { return slots * block_size / (1024*1024); };

    std::vector<BenchResult> all_results;
    std::vector<std::vector<SearchResult>> hnsw_baseline;
    auto trim = []() { malloc_trim(0); };

    IOConfig odirect_config;
    odirect_config.use_odirect = true;
    odirect_config.drop_page_cache = true;

    // ---- F0: Full-memory ----
    std::cout << "\n[2] F0: Full-memory (cache=" << num_blocks << ")..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        for (size_t q = 0; q < num_query; q++)
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F0: Full", "none");
        r.cache_slots = num_blocks;
        r.cache_mb = cache_mb(num_blocks);
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- F1: OD c1024, no prefetch ----
    std::cout << "\n[3] F1: OD c1024, no prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            1024, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F1: c1024", "none");
        r.cache_slots = 1024;
        r.cache_mb = cache_mb(1024);
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- F2: OD c1024 + graph prefetch ----
    std::cout << "\n[4] F2: OD c1024 + graph prefetch..." << std::endl;
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
                              dim, k, "F2: c1024+gp", "graph");
        r.cache_slots = 1024;
        r.cache_mb = cache_mb(1024);
        printResult(r);
        // Print detailed prefetcher stats
        auto& pf_stats = hnsw.getGraphPrefetchStats();
        printPrefetcherDetail(pf_stats);
        all_results.push_back(r);
    }
    trim();

    // ---- F4: OD c512, no prefetch (参照) ----
    std::cout << "\n[6] F4: OD c512, no prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            512, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F4: c512", "none");
        r.cache_slots = 512;
        r.cache_mb = cache_mb(512);
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- F5: OD c512 + graph prefetch (参照) ----
    std::cout << "\n[7] F5: OD c512 + graph prefetch..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            512, dim, odirect_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        hnsw.enableGraphPrefetch(true);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F5: c512+gp", "graph");
        r.cache_slots = 512;
        r.cache_mb = cache_mb(512);
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    printComparison(all_results);

    // Save JSON
    std::ofstream jout("logs/d4_prefetch_benchmark.json");
    jout << std::fixed << std::setprecision(4);
    jout << "{\"benchmark\":\"d4_prefetch\","
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
             << ",\"p99_us\":" << r.lat.p99
             << ",\"qps\":" << r.qps
             << ",\"hit_rate\":" << r.hit_rate
             << ",\"rss_mb\":" << r.rss_mb
             << ",\"cache_slots\":" << r.cache_slots
             << ",\"pf_mode\":\"" << r.prefetch_mode << "\""
             << ",\"pf_submitted\":" << r.pf_submitted
             << ",\"pf_completed\":" << r.pf_completed
             << ",\"pf_skipped\":" << r.pf_skipped
             << "}";
    }
    jout << "]}" << std::endl;
    jout.close();
    std::cout << "JSON saved to logs/d4_prefetch_benchmark.json" << std::endl;

    return 0;
}
