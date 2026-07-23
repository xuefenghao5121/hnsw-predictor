// benchmark_fadvise.cpp - cached I/O + posix_fadvise(DONTNEED) benchmark
//
// F0: Full-memory (cache = num_blocks, cached I/O)
// F1: cached I/O + fadvise, cache=64, no prefetch
// F2: cached I/O + fadvise, cache=256, no prefetch
// F3: cached I/O (no fadvise), cache=64 (page cache 不释放, 对照组)
// F4: cached I/O + fadvise, cache=128
//
// 用法:
//   ./benchmark_fadvise <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50]

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
#include <malloc.h>
#include <vector>

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

// 获取 page cache 占用 (通过 /proc/self/smaps 统计)
static size_t getPageCache_MB() {
    // 简单方法：读 /proc/meminfo 的 Cached 字段
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.substr(0, 6) == "Cached:") {
            size_t val = 0;
            std::sscanf(line.c_str(), "Cached: %zu kB", &val);
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
    size_t page_cache_mb = 0;
    size_t cache_slots = 0;
    bool use_fadvise = false;
};

BenchResult runBenchmark(
    DiskHNSW& hnsw,
    const std::vector<float>& queries,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    int dim, int k, const std::string& name, bool use_fadvise)
{
    BenchResult r;
    r.name = name;
    r.use_fadvise = use_fadvise;

    size_t nq = queries.size() / dim;
    std::vector<double> latencies(nq);
    std::vector<std::vector<SearchResult>> results(nq);

    hnsw.resetCacheStats();

    // Warmup (10 queries)
    for (size_t q = 0; q < std::min(10UL, nq); q++) {
        hnsw.searchKnn(&queries[q * dim], k);
    }
    if (use_fadvise) hnsw.dropPageCache();

    // Timed search
    auto t0 = std::chrono::high_resolution_clock::now();
    for (size_t q = 0; q < nq; q++) {
        auto q0 = std::chrono::high_resolution_clock::now();
        results[q] = hnsw.searchKnn(&queries[q * dim], k);
        auto q1 = std::chrono::high_resolution_clock::now();
        latencies[q] = std::chrono::duration<double, std::micro>(q1 - q0).count();
        // 每次查询后释放 page cache
        if (use_fadvise) hnsw.dropPageCache();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double total_s = std::chrono::duration<double>(t1 - t0).count();

    r.lat = computeLatency(latencies);
    r.qps = nq / total_s;
    r.rss_mb = getRSS_MB();
    r.page_cache_mb = getPageCache_MB();

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
    std::cout << "  Page cache:   " << r.page_cache_mb << " MB" << std::endl;
    std::cout << "  Fadvise:      " << (r.use_fadvise ? "ON" : "OFF") << std::endl;
}

void printComparison(const std::vector<BenchResult>& results) {
    std::cout << "\n\n========== cached I/O + fadvise Benchmark ==========" << std::endl;
    std::cout << "\n| Config | Mean(ms) | P99(ms) | QPS | Hit% | RSS(MB) | PageCache(MB) | R@HNSW |" << std::endl;
    std::cout << "|--------|----------|---------|-----|------|---------|---------------|--------|" << std::endl;
    for (const auto& r : results) {
        std::cout << "| " << r.name
                  << " | " << std::fixed << std::setprecision(2) << r.lat.mean / 1000
                  << " | " << r.lat.p99 / 1000
                  << " | " << std::setprecision(1) << r.qps
                  << " | " << r.hit_rate
                  << " | " << r.rss_mb
                  << " | " << r.page_cache_mb
                  << " | " << std::setprecision(2) << r.recall_hnsw << "% |" << std::endl;
    }

    if (!results.empty()) {
        auto& base = results[0];
        std::cout << "\nRelative to F0 (full-memory):" << std::endl;
        std::cout << "| Config | Latency(x) | QPS(%) | RSS+Cache(MB) | Mem Saved |" << std::endl;
        std::cout << "|--------|-----------|--------|--------------|-----------|" << std::endl;
        for (const auto& r : results) {
            double lat_r = r.lat.mean / base.lat.mean;
            double qps_r = r.qps / base.qps * 100;
            size_t total_mem = r.rss_mb + r.page_cache_mb;
            size_t base_mem = base.rss_mb + base.page_cache_mb;
            double mem_s = 100.0 - (double)total_mem / base_mem * 100;
            std::cout << "| " << r.name
                      << " | " << std::setprecision(2) << lat_r << "x"
                      << " | " << qps_r << "%"
                      << " | " << total_mem
                      << " | " << mem_s << "% |" << std::endl;
        }
    }
    std::cout << "\n===================================================\n" << std::endl;
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

    std::cout << "=== cached I/O + fadvise Benchmark ===" << std::endl;
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

    // Free base_data
    std::vector<float>().swap(base_data);
    std::cout << "  base_data freed" << std::endl;

    // Get num_blocks
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    std::cout << "  Blocks: " << num_blocks << std::endl;

    std::vector<BenchResult> all_results;
    std::vector<std::vector<SearchResult>> hnsw_baseline;

    // Helper: force allocator to release freed memory to OS
    auto trim_memory = []() {
#ifdef __GLIBC__
        malloc_trim(0);
#endif
    };

    // cached I/O config (no O_DIRECT)
    IOConfig cached_config;  // default: use_odirect=false, drop_page_cache=false
    // cached I/O + per-read fadvise
    IOConfig fadvise_config;
    fadvise_config.drop_page_cache = true;  // each pread followed by fadvise(DONTNEED)

    // ---- F0: Full-memory (cache = num_blocks, cached I/O) ----
    std::cout << "\n[2] F0: Full-memory (cache=" << num_blocks << ")..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        for (size_t q = 0; q < num_query; q++) {
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
        }
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F0: Full-mem", false);
        r.cache_slots = num_blocks;
        printResult(r);
        all_results.push_back(r);
    }
    trim_memory();

    // ---- F1: cached I/O + fadvise per-query, cache=64 ----
    std::cout << "\n[3] F1: cached+fadvise, cache=64..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F1: c64+fadv", true);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }
    trim_memory();

    // ---- F2: cached I/O + fadvise per-query, cache=256 ----
    std::cout << "\n[4] F2: cached+fadvise, cache=256..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 256, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F2: c256+fadv", true);
        r.cache_slots = 256;
        printResult(r);
        all_results.push_back(r);
    }
    trim_memory();

    // ---- F3: cached I/O (NO fadvise), cache=64 (page cache 不释放) ----
    std::cout << "\n[5] F3: cached (no fadvise), cache=64..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        auto r = runBenchmark(hnsw, query_data, gt_data, hnsw_baseline,
                              dim, k, "F3: c64+nofadv", false);
        r.cache_slots = 64;
        printResult(r);
        all_results.push_back(r);
    }
    trim_memory();

    printComparison(all_results);
    return 0;
}
