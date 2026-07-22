// benchmark_prefetch.cpp - 阶段三 benchmark: 全内存 vs DiskHNSW(无预取) vs DiskHNSW(Markov预取)
//
// 用法:
//   ./benchmark_prefetch <graph> <bfs> <blocks> <route> <data> <query> <gt> <k> <ef> <model_path>
//
// 配置:
//   E0: Full-memory (cache = num_blocks, 全部在 DRAM)
//   E1: DiskHNSW no prefetch (cache=64)
//   E2: DiskHNSW + Markov prefetch (cache=64)
//   E3: DiskHNSW no prefetch (cache=256)
//   E4: DiskHNSW + Markov prefetch (cache=256)
#include "common.h"
#include "block_cache.h"
#include "disk_hnsw.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <set>
#include <numeric>
#include <sys/resource.h>

static size_t getRSS_MB() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / 1024;
}

using SearchResult = std::pair<float, uint64_t>;

std::vector<std::vector<uint64_t>> read_gt(const std::string& path, size_t n, int k) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::vector<uint64_t>> gt(n);
    for (size_t i = 0; i < n; i++) {
        gt[i].resize(k);
        in.read(reinterpret_cast<char*>(gt[i].data()), k * sizeof(uint64_t));
    }
    return gt;
}

struct BenchmarkResult {
    std::string config_name;
    double recall_gt;
    double recall_hnsw;
    double mean_us;
    double p50_us;
    double p95_us;
    double p99_us;
    double qps;
    double cache_hit_rate;
    size_t rss_mb;
    size_t cache_slots;
    // prefetch stats
    bool prefetch_enabled = false;
    size_t prefetch_requested = 0;
    size_t prefetch_skipped = 0;
    size_t prefetch_loaded = 0;
    size_t prefetch_failed = 0;
};

BenchmarkResult run_benchmark(
    DiskHNSW& hnsw,
    const std::vector<float>& queries,
    const std::vector<std::vector<uint64_t>>& gt_data,
    const std::vector<std::vector<SearchResult>>& hnsw_baseline,
    int dim, int k,
    const std::string& config_name,
    bool prefetch_enabled)
{
    BenchmarkResult r{};
    r.config_name = config_name;
    r.prefetch_enabled = prefetch_enabled;

    size_t num_queries = queries.size() / dim;
    std::vector<double> latencies(num_queries);
    std::vector<std::vector<SearchResult>> results(num_queries);

    hnsw.resetCacheStats();
    if (prefetch_enabled) hnsw.resetPrefetchStats();

    // warm up (10 queries)
    for (size_t q = 0; q < std::min(10UL, num_queries); q++) {
        hnsw.searchKnn(&queries[q * dim], k);
    }

    auto t_start = std::chrono::high_resolution_clock::now();
    for (size_t q = 0; q < num_queries; q++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        results[q] = hnsw.searchKnn(&queries[q * dim], k);
        auto t1 = std::chrono::high_resolution_clock::now();
        latencies[q] = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    auto t_end = std::chrono::high_resolution_clock::now();
    double total_s = std::chrono::duration<double>(t_end - t_start).count();

    std::vector<double> sorted_lat = latencies;
    std::sort(sorted_lat.begin(), sorted_lat.end());

    r.mean_us = std::accumulate(latencies.begin(), latencies.end(), 0.0) / num_queries;
    r.p50_us = sorted_lat[num_queries / 2];
    r.p95_us = sorted_lat[(size_t)(num_queries * 0.95)];
    r.p99_us = sorted_lat[(size_t)(num_queries * 0.99)];
    r.qps = num_queries / total_s;

    // Recall
    size_t correct_gt = 0, correct_hnsw = 0;
    for (size_t q = 0; q < num_queries; q++) {
        std::set<uint64_t> gt_set(gt_data[q].begin(), gt_data[q].end());
        std::set<uint64_t> hnsw_set;
        for (const auto& [dist, id] : hnsw_baseline[q]) hnsw_set.insert(id);
        for (const auto& [dist, id] : results[q]) {
            if (gt_set.count(id)) correct_gt++;
            if (hnsw_set.count(id)) correct_hnsw++;
        }
    }
    r.recall_gt = (double)correct_gt / (num_queries * k) * 100;
    r.recall_hnsw = (double)correct_hnsw / (num_queries * k) * 100;

    auto& stats = hnsw.getCacheStats();
    size_t total = stats.total_accesses.load();
    r.cache_hit_rate = total > 0 ? (double)stats.cache_hits.load() / total * 100 : 0;
    r.rss_mb = getRSS_MB();

    if (prefetch_enabled) {
        auto& pstats = hnsw.getPrefetchStats();
        r.prefetch_requested = pstats.prefetch_requested.load();
        r.prefetch_skipped = pstats.prefetch_skipped.load();
        r.prefetch_loaded = pstats.prefetch_loaded.load();
        r.prefetch_failed = pstats.prefetch_failed.load();
    }

    return r;
}

void print_result(const BenchmarkResult& r) {
    std::cout << "\n=== " << r.config_name << " ===" << std::endl;
    std::cout << "  Recall@GT:    " << r.recall_gt << "%" << std::endl;
    std::cout << "  Recall@HNSW:  " << r.recall_hnsw << "%" << std::endl;
    std::cout << "  Mean:         " << r.mean_us << " us" << std::endl;
    std::cout << "  P50:          " << r.p50_us << " us" << std::endl;
    std::cout << "  P95:          " << r.p95_us << " us" << std::endl;
    std::cout << "  P99:          " << r.p99_us << " us" << std::endl;
    std::cout << "  QPS:          " << r.qps << std::endl;
    std::cout << "  Cache hit:    " << r.cache_hit_rate << "%" << std::endl;
    std::cout << "  RSS:          " << r.rss_mb << " MB" << std::endl;
    if (r.prefetch_enabled) {
        std::cout << "  Prefetch req:   " << r.prefetch_requested << std::endl;
        std::cout << "  Prefetch skip:  " << r.prefetch_skipped << std::endl;
        std::cout << "  Prefetch load:  " << r.prefetch_loaded << std::endl;
        std::cout << "  Prefetch fail:  " << r.prefetch_failed << std::endl;
    }
}

void print_comparison(const std::vector<BenchmarkResult>& results) {
    std::cout << "\n\n========== 竞品对比总结 ==========" << std::endl;
    std::cout << "\n| 配置 | Mean(ms) | P50(ms) | P95(ms) | P99(ms) | QPS | Cache% | RSS(MB) | Recall@HNSW |" << std::endl;
    std::cout << "|------|----------|---------|---------|---------|-----|--------|---------|-------------|" << std::endl;
    for (const auto& r : results) {
        std::cout << "| " << r.config_name
                  << " | " << r.mean_us / 1000
                  << " | " << r.p50_us / 1000
                  << " | " << r.p95_us / 1000
                  << " | " << r.p99_us / 1000
                  << " | " << r.qps
                  << " | " << r.cache_hit_rate
                  << " | " << r.rss_mb
                  << " | " << r.recall_hnsw << "% |" << std::endl;
    }

    // 相对于全内存的比值
    if (!results.empty()) {
        auto& base = results[0];  // E0 全内存
        std::cout << "\n相对全内存(E0)对比:" << std::endl;
        std::cout << "| 配置 | 延迟倍数 | QPS比 | 内存比 | 内存节省 |" << std::endl;
        std::cout << "|------|---------|-------|--------|---------|" << std::endl;
        for (const auto& r : results) {
            double lat_ratio = r.mean_us / base.mean_us;
            double qps_ratio = r.qps / base.qps;
            double mem_ratio = (double)r.rss_mb / base.rss_mb;
            double mem_saved = 1.0 - mem_ratio;
            std::cout << "| " << r.config_name
                      << " | " << lat_ratio << "x"
                      << " | " << qps_ratio << "x"
                      << " | " << mem_ratio << "x"
                      << " | " << (mem_saved * 100) << "% |" << std::endl;
        }
    }
    std::cout << "\n==================================\n" << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 11) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph> <bfs> <blocks> <route> <data> <query> <gt> <k> <ef> <model_path>"
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
    int k = std::atoi(argv[8]);
    int ef = std::atoi(argv[9]);
    std::string model_path = argv[10];

    std::cout << "[1] Loading data..." << std::endl;
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

    // 先获取 num_blocks 信息
    // 从 BlockCache 初始化信息中读取
    // 简单方法: 文件大小 / block_size
    std::ifstream blocks_file(blocks_path, std::ios::binary | std::ios::ate);
    size_t blocks_file_size = blocks_file.tellg();
    blocks_file.close();
    size_t block_size = 262144;  // 固定值
    size_t num_blocks = blocks_file_size / block_size;
    std::cout << "  Blocks: " << num_blocks << " (file=" << blocks_file_size << " bytes)" << std::endl;

    std::vector<BenchmarkResult> all_results;
    std::vector<std::vector<SearchResult>> hnsw_baseline;

    // ---- E0: Full-memory (cache = num_blocks) ----
    std::cout << "\n[2] E0: Full-memory (cache=" << num_blocks << ")..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, num_blocks, dim);
        hnsw.setEf(ef);
        // 收集 baseline 结果
        for (size_t q = 0; q < num_query; q++) {
            hnsw_baseline.push_back(hnsw.searchKnn(&query_data[q * dim], k));
        }
        auto r = run_benchmark(hnsw, query_data, gt_data, hnsw_baseline,
                               dim, k, "E0: Full-mem", false);
        r.cache_slots = num_blocks;
        print_result(r);
        all_results.push_back(r);
    }

    // ---- E1: DiskHNSW no prefetch (cache=64) ----
    std::cout << "\n[3] E1: No prefetch (cache=64)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        auto r = run_benchmark(hnsw, query_data, gt_data, hnsw_baseline,
                               dim, k, "E1: Disk c64", false);
        r.cache_slots = 64;
        print_result(r);
        all_results.push_back(r);
    }

    // ---- E2: DiskHNSW + Markov prefetch (cache=64) ----
    std::cout << "\n[4] E2: Markov prefetch (cache=64)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
        hnsw.setEf(ef);
        hnsw.enablePrefetch(model_path);
        auto r = run_benchmark(hnsw, query_data, gt_data, hnsw_baseline,
                               dim, k, "E2: Disk c64+pf", true);
        r.cache_slots = 64;
        print_result(r);
        all_results.push_back(r);
    }

    // ---- E3: DiskHNSW no prefetch (cache=256) ----
    std::cout << "\n[5] E3: No prefetch (cache=256)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 256, dim);
        hnsw.setEf(ef);
        auto r = run_benchmark(hnsw, query_data, gt_data, hnsw_baseline,
                               dim, k, "E3: Disk c256", false);
        r.cache_slots = 256;
        print_result(r);
        all_results.push_back(r);
    }

    // ---- E4: DiskHNSW + Markov prefetch (cache=256) ----
    std::cout << "\n[6] E4: Markov prefetch (cache=256)..." << std::endl;
    {
        DiskHNSW hnsw(graph_path, bfs_path, blocks_path, route_path, 256, dim);
        hnsw.setEf(ef);
        hnsw.enablePrefetch(model_path);
        auto r = run_benchmark(hnsw, query_data, gt_data, hnsw_baseline,
                               dim, k, "E4: Disk c256+pf", true);
        r.cache_slots = 256;
        print_result(r);
        all_results.push_back(r);
    }

    print_comparison(all_results);
    return 0;
}
