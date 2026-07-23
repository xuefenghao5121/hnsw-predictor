// benchmark_mmap.cpp - mmap vs pread vs full-memory benchmark
//
// M0: Full-memory (cache = num_blocks, pread cached)
// M1: c64, pread cached (baseline)
// M2: c64, mmap + MADV_RANDOM
// M3: c256, mmap + MADV_RANDOM
// M4: c64, mmap + MADV_RANDOM + inter-query fadvise(WILLNEED)
// M5: c512, mmap + MADV_RANDOM
//
// 用法:
//   ./benchmark_mmap <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50]

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
    uint32_t nq, kk;
    in.read(reinterpret_cast<char*>(&nq), sizeof(uint32_t));
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
    std::string io_mode;
};

BenchResult runBenchmark(
    DiskHNSW& hnsw,
    const std::vector<float>& queries,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    int dim, int k, const std::string& name)
{
    BenchResult r;
    r.name = name;

    size_t nq = queries.size() / dim;
    std::vector<double> latencies(nq);
    std::vector<std::vector<SearchResult>> results(nq);

    hnsw.resetCacheStats();

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
}

void printComparison(const std::vector<BenchResult>& results) {
    std::cout << "\n\n========== mmap Benchmark ==========" << std::endl;
    std::cout << "\n| Config | Mean(ms) | P99(ms) | QPS | Hit% | RSS(MB) | R@HNSW |" << std::endl;
    std::cout << "|--------|----------|---------|-----|------|---------|--------|" << std::endl;
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << std::fixed << std::setprecision(2) << r.lat.mean / 1000
                  << " | " << r.lat.p99 / 1000
                  << " | " << std::setprecision(1) << r.qps
                  << " | " << r.hit_rate
                  << " | " << r.rss_mb
                  << " | " << std::setprecision(2) << r.recall_hnsw << "% |" << std::endl;
    }

    if (!results.empty()) {
        auto& base = results[0];
        std::cout << "\nRelative to M0 (full-memory):" << std::endl;
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
    std::cout << "\n===================================\n" << std::endl;
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

    std::cout << "=== mmap Benchmark ===" << std::endl;
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

    IOConfig cached_config;  // default: pread cached
    IOConfig mmap_config;
    mmap_config.use_mmap = true;

    // ---- M0: Full-memory (pread cached) ----
    std::cout << "\n[2] M0: Full-memory..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        for (size_t q = 0; q < num_query; q++) {
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
        }
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline, dim, k, "M0: Full-mem");
        r.cache_slots = num_blocks;
        r.io_mode = "cached";
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- M1: c64, pread cached (baseline) ----
    std::cout << "\n[3] M1: c64 pread..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline, dim, k, "M1: c64 pread");
        r.cache_slots = 64;
        r.io_mode = "cached";
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- M2: c64, mmap ----
    std::cout << "\n[4] M2: c64 mmap..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            64, dim, mmap_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline, dim, k, "M2: c64 mmap");
        r.cache_slots = 64;
        r.io_mode = "mmap";
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- M3: c256, mmap ----
    std::cout << "\n[5] M3: c256 mmap..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            256, dim, mmap_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline, dim, k, "M3: c256 mmap");
        r.cache_slots = 256;
        r.io_mode = "mmap";
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    // ---- M4: c512, mmap ----
    std::cout << "\n[6] M4: c512 mmap..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            512, dim, mmap_config);
        DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline, dim, k, "M4: c512 mmap");
        r.cache_slots = 512;
        r.io_mode = "mmap";
        printResult(r);
        all_results.push_back(r);
    }
    trim();

    printComparison(all_results);
    return 0;
}
