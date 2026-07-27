// benchmark_diskhnsw.cpp - Standalone DiskHNSW benchmark (single config)
// Usage: ./benchmark_diskhnsw <graph> <bfs> <blocks> <route> <data> <query> <gt> <k> <ef> <num_queries>
// Env: CACHE_MB=<cache_size_mb> (required)
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
#include <string>
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

static std::vector<std::vector<uint64_t>> read_gt(const std::string& path, size_t n, int k) {
    // GT format: header(8B) = n_queries(uint32) + kk(uint32)
    //           then n * kk * uint64 indices (no distances)
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

int main(int argc, char** argv) {
    if (argc < 11) {
        std::cerr << "Usage: " << argv[0] << " <graph> <bfs> <blocks> <route> <data> <query> <gt> <k> <ef> <num_queries>" << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string blocks_path = argv[3];
    std::string route_path = argv[4];
    std::string data_path = argv[5];  // only used to get dim
    std::string query_path = argv[6];
    std::string gt_path = argv[7];
    int k = atoi(argv[8]);
    int ef = atoi(argv[9]);
    int num_query = atoi(argv[10]);

    const char* cache_env = std::getenv("CACHE_MB");
    if (!cache_env) { std::cerr << "ERROR: CACHE_MB required" << std::endl; return 1; }
    size_t cache_mb = std::stoul(cache_env);

    // Only read dim from data file header (don't load full data!)
    int dim;
    {
        std::ifstream df(data_path, std::ios::binary);
        df.read(reinterpret_cast<char*>(&dim), sizeof(int));
    }

    // Read block file header
    std::ifstream bf(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bf.read(reinterpret_cast<char*>(&bfhdr), sizeof(bfhdr));
    bf.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    uint32_t block_size = bfhdr.block_size;
    size_t mem_slots = cache_mb * 1024 * 1024 / block_size;

    // Load queries (small)
    int qdim;
    size_t num_queries_file;
    std::vector<float> query_data = read_fvecs(query_path, qdim, num_queries_file);
    if ((size_t)num_query > num_queries_file) num_query = num_queries_file;

    auto gt_data = read_gt(gt_path, num_query, k);

    std::cout << "=== DiskHNSW Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef << ", queries=" << num_query << std::endl;
    std::cout << "Blocks: " << num_blocks << ", block_size=" << block_size << std::endl;
    std::cout << "Cache: " << cache_mb << "MB (" << mem_slots << " slots, "
              << std::fixed << std::setprecision(1) << (100.0 * mem_slots / num_blocks) << "% coverage)" << std::endl;

    IOConfig odirect_config;
    odirect_config.use_odirect = true;
    odirect_config.drop_page_cache = true;

    auto layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
    auto policy = std::make_unique<LRUPolicy>();
    auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy),
                                               mem_slots, dim, odirect_config);
    auto hnsw = std::make_unique<DiskHNSW>(graph_path, bfs_path, std::move(cache));
    hnsw->setEf(ef);
    hnsw->enableGraphPrefetch(true);

    // PQ 模式: 检查 PQ_CODES_PATH 环境变量
    const char* pq_path_env = std::getenv("PQ_CODES_PATH");
    if (pq_path_env && pq_path_env[0]) {
        hnsw->loadPQCodes(pq_path_env);
    }

    hnsw->resetCacheStats();
    hnsw->resetGraphPrefetchStats();

    std::cout << "RSS after init: " << getRSS_MB() << " MB" << std::endl;

    // CPU warmup: spin to ramp up frequency
    volatile double warm_sink = 0;
    for (int i = 0; i < 1000000; i++) warm_sink += i * 1.1;
    if (warm_sink < 0) std::cerr << "(warmup sink)";

    // Search warmup: run all queries once to warm cache + CPU
    // (also warmup batch mode if batch_size > 0)
    const char* bs_env = std::getenv("BATCH_SIZE");
    int batch_size = bs_env ? atoi(bs_env) : 0;  // 0 = blocking, >0 = non-blocking batchSearch
    
    if (batch_size > 0) {
        std::cout << "Mode: NON-BLOCKING (batch_size=" << batch_size << ")" << std::endl;
        std::vector<float> warmup_q(std::min(num_query, 10) * dim);
        std::memcpy(warmup_q.data(), query_data.data(), warmup_q.size() * sizeof(float));
        hnsw->batchSearch(warmup_q, k, batch_size);
    } else {
        std::cout << "Mode: BLOCKING (single query)" << std::endl;
        for (int i = 0; i < num_query; i++)
            hnsw->searchKnn(&query_data[i * dim], k);
    }

    // Timed search
    std::vector<double> latencies(num_query);
    std::vector<std::vector<SearchResult>> results(num_query);
    
    // NUM_THREADS: >0 = concurrent search
    const char* nt_env = std::getenv("NUM_THREADS");
    int num_threads = nt_env ? atoi(nt_env) : 0;
    
    auto t0 = std::chrono::high_resolution_clock::now();
    if (num_threads > 0) {
        // Multi-threaded concurrent search
        std::cout << "Mode: CONCURRENT (threads=" << num_threads << ")" << std::endl;
        std::vector<float> all_q(num_query * dim);
        std::memcpy(all_q.data(), query_data.data(), all_q.size() * sizeof(float));
        auto batch_results = hnsw->batchSearchConcurrent(all_q, k, num_threads);
        auto t1 = std::chrono::high_resolution_clock::now();
        double total_s = std::chrono::duration<double>(t1 - t0).count();
        for (int i = 0; i < num_query; i++) {
            results[i] = batch_results[i];
            latencies[i] = total_s / num_query * 1e6;
        }
    } else if (batch_size > 0) {
        // Non-blocking batch search
        std::vector<float> all_q(num_query * dim);
        std::memcpy(all_q.data(), query_data.data(), all_q.size() * sizeof(float));
        auto batch_results = hnsw->batchSearch(all_q, k, batch_size);
        auto t1 = std::chrono::high_resolution_clock::now();
        // batchSearch returns results for all queries; per-query latency not available
        // Use total time / num_query as mean
        double total_s = std::chrono::duration<double>(t1 - t0).count();
        for (int i = 0; i < num_query; i++) {
            results[i] = batch_results[i];
            latencies[i] = total_s / num_query * 1e6;  // approximate
        }
    } else {
        for (int i = 0; i < num_query; i++) {
            auto q0 = std::chrono::high_resolution_clock::now();
            results[i] = hnsw->searchKnn(&query_data[i * dim], k);
            auto q1 = std::chrono::high_resolution_clock::now();
            latencies[i] = std::chrono::duration<double, std::micro>(q1 - q0).count();
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double total_s = std::chrono::duration<double>(t1 - t0).count();

    std::sort(latencies.begin(), latencies.end());
    double mean_us = 0;
    for (auto l : latencies) mean_us += l;
    mean_us /= num_query;
    double p50 = latencies[num_query / 2];
    double p95 = latencies[(size_t)(num_query * 0.95)];
    double p99 = latencies[(size_t)(num_query * 0.99)];
    double qps = num_query / total_s;

    // Compute recall: GT excludes self-match (query vector itself in dataset),
    // so we compare against GT as-is. DiskHNSW may return the self-match,
    // which counts as a miss. This is expected and fair for comparison.
    // To be fair with hnswlib baseline, we use the same GT for both.
    size_t correct = 0;
    size_t total = 0;
    for (int i = 0; i < num_query; i++) {
        std::set<uint64_t> gt_set;
        for (size_t j = 0; j < gt_data[i].size() && j < (size_t)k; j++)
            gt_set.insert(gt_data[i][j]);
        size_t found = 0;
        for (const auto& [d, id] : results[i]) {
            if (gt_set.count(id)) found++;
        }
        correct += found;
        total += k;
    }
    double recall = (double)correct / total * 100;

    auto& stats = hnsw->getCacheStats();
    double hit_rate = stats.total_accesses > 0 ?
        (double)stats.cache_hits.load() / stats.total_accesses.load() * 100 : 0;
    auto pf = hnsw->getGraphPrefetchStats();

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Recall: " << std::fixed << std::setprecision(2) << recall << "%" << std::endl;
    std::cout << "Mean:   " << mean_us / 1000.0 << " ms" << std::endl;
    std::cout << "P50:    " << p50 / 1000.0 << " ms" << std::endl;
    std::cout << "P95:    " << p95 / 1000.0 << " ms" << std::endl;
    std::cout << "P99:    " << p99 / 1000.0 << " ms" << std::endl;
    std::cout << "QPS:    " << std::fixed << std::setprecision(1) << qps << std::endl;
    std::cout << "Hit%:   " << hit_rate << std::endl;
    std::cout << "RSS:    " << getRSS_MB() << " MB" << std::endl;
    std::cout << "PF:     submitted=" << pf.prefetch_submitted
              << " skipped=" << pf.prefetch_skipped
              << " failed=" << pf.prefetch_failed << std::endl;

    return 0;
}
