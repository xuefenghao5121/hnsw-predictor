// benchmark_overlap.cpp - I/O overlap benchmark
//
// Tests 4 modes:
//   F0: Full-memory (baseline)
//   F2-single: c1024+GP, single query, blocking search (original)
//   F2-nonblock: c1024+GP, single query, non-blocking search (new)
//   F2-batch: c1024+GP, batch query, non-blocking search (new)
//
// Usage:
//   ./benchmark_overlap <graph> <bfs> <blocks> <route> <data> <query> <gt> [k=10] [ef=50] [num_queries=200]

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

    // Load data
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    std::vector<float>().swap(base_data);

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

    // Build HNSW baseline
    std::cout << "\n[1] Building HNSW baseline..." << std::endl;
    std::vector<std::vector<SearchResult>> hnsw_baseline;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        for (size_t q = 0; q < num_query; q++)
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
    }

    IOConfig odirect_config;
    odirect_config.use_odirect = true;
    odirect_config.drop_page_cache = true;

    std::vector<BenchResult> all_results;

    // ---- F0: Full-memory ----
    std::cout << "\n[F0] Full-memory..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        hnsw.resetCacheStats();

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
        double total_s = std::chrono::duration<double>(t1 - t0).count();

        BenchResult r;
        r.name = "F0: Full-mem";
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

        printResult(r);
        all_results.push_back(r);
    }
    malloc_trim(0);

    // ---- F2-single: c1024+GP, blocking search (original) ----
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
        auto& pf = hnsw.getGraphPrefetchStats();
        r.pf_submitted = pf.prefetch_submitted;
        r.pf_skipped = pf.prefetch_skipped;
        r.pf_failed = pf.prefetch_failed;

        printResult(r);
        all_results.push_back(r);
    }
    malloc_trim(0);


    // ---- F2-batch: c1024+GP, batch non-blocking search ----
    for (size_t bs : {4}) {
        if (bs > num_query) continue;
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
            auto& pf = hnsw.getGraphPrefetchStats();
            r.pf_submitted = pf.prefetch_submitted;
            r.pf_skipped = pf.prefetch_skipped;
        r.pf_failed = pf.prefetch_failed;

            printResult(r);
            all_results.push_back(r);
        }
        malloc_trim(0);
    }

    // ---- F2-single-256MB: 同内存 (256MB) 对比, slots 按 block_size 自动计算 ----
    size_t mem256_slots = (size_t)256 * 1024 * 1024 / block_size;  // 64KB->4096, 32KB->8192
    std::cout << "\n[F2-single-256MB] c" << mem256_slots << "+GP, blocking (同内存 256MB)..." << std::endl;
    {
        auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
        auto policy = std::make_unique<LRUPolicy>();
        auto cache = std::make_unique<BlockCache>(
            blocks_path, std::move(layout), std::move(policy),
            mem256_slots, dim, odirect_config);
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
        r.name = "F2-single: c" + std::to_string(mem256_slots) + "+GP (256MB)";
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
        auto& pf = hnsw.getGraphPrefetchStats();
        r.pf_submitted = pf.prefetch_submitted;
        r.pf_skipped = pf.prefetch_skipped;
        r.pf_failed = pf.prefetch_failed;
        printResult(r);
        all_results.push_back(r);
    }
    malloc_trim(0);

    // ---- F2-beam: Cache-Aware Beam Search ----
    // 测试多个 beam width: B=2, 4, 8
    for (int bw : {2, 4, 8}) {
        std::cout << "\n[F2-beam-" << bw << "] c" << mem256_slots << "+GP, beam search (B=" << bw << ")..." << std::endl;
        {
            auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
            auto policy = std::make_unique<LRUPolicy>();
            auto cache = std::make_unique<BlockCache>(
                blocks_path, std::move(layout), std::move(policy),
                mem256_slots, dim, odirect_config);
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
            r.name = "F2-beam-" + std::to_string(bw) + ": c" + std::to_string(mem256_slots) + "+GP (256MB)";
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
            auto& pf = hnsw.getGraphPrefetchStats();
            r.pf_submitted = pf.prefetch_submitted;
            r.pf_skipped = pf.prefetch_skipped;
            r.pf_failed = pf.prefetch_failed;
            printResult(r);
            all_results.push_back(r);
        }
        malloc_trim(0);
    }

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
