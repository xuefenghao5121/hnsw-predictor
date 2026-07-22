// benchmark_1m.cpp - SIFT1M Comprehensive Benchmark
//
// Test matrix:
//   A: cache slots sweep (64/128/256/512)
//   B: replacement policy comparison (LRU/LFU/LRU-K)
//   C: layout x IO mode matrix
//   D: full-memory HNSW vs DiskHNSW baseline comparison
//
// Uses precomputed GT to avoid brute-force overhead.
// Outputs JSON results.

#include "hnswlib/hnswlib.h"
#include "common.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include "disk_hnsw.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

using hnswlib::tableint;

// ============================================================
// Utility functions
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

static std::vector<std::vector<uint64_t>> loadPrecomputedGT(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) throw std::runtime_error("Cannot open GT file: " + path);
    uint32_t n_queries, k;
    in.read(reinterpret_cast<char*>(&n_queries), sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(&k), sizeof(uint32_t));
    std::vector<std::vector<uint64_t>> gt(n_queries);
    for (uint32_t i = 0; i < n_queries; i++) {
        gt[i].resize(k);
        in.read(reinterpret_cast<char*>(gt[i].data()), k * sizeof(uint64_t));
    }
    in.close();
    std::cout << "Loaded GT: " << n_queries << " queries, k=" << k << std::endl;
    return gt;
}

static double computeRecall(const std::vector<std::vector<uint64_t>>& gt,
                            const std::vector<std::vector<uint64_t>>& pred, size_t k) {
    size_t hits = 0, total = 0;
    for (size_t i = 0; i < gt.size(); i++) {
        std::set<uint64_t> s(gt[i].begin(), std::min(gt[i].begin() + k, gt[i].end()));
        for (size_t j = 0; j < std::min(k, pred[i].size()); j++)
            if (s.count(pred[i][j])) hits++;
        total += k;
    }
    return total > 0 ? (double)hits / total : 0.0;
}

struct LatencyStats { double mean, p50, p95, p99, total_ms; };

static LatencyStats computeLatencyStats(std::vector<double> times) {
    std::sort(times.begin(), times.end());
    double sum = 0;
    for (double t : times) sum += t;
    size_t n = times.size();
    return { sum/n, times[n/2], times[(size_t)(n*0.95)], times[(size_t)(n*0.99)], sum/1000.0 };
}

static IOConfig buildIOConfig(const std::string& mode, double latency) {
    IOConfig c;
    if (mode == "direct") { c.use_odirect = true; c.drop_page_cache = true; }
    else if (mode == "simulated") { c.drop_page_cache = true; c.simulated_latency_us = latency; }
    return c;
}

static std::string escapeJson(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

// ============================================================
// Config & Result
// ============================================================

struct TestConfig {
    std::string group, id, description;
    std::string layout = "bfs", policy = "lru", io_mode = "cached";
    double latency = 0.0;
    size_t cache_slots = 64, ef = 50, k = 10;
    bool is_baseline = false;
};

struct TestResult {
    TestConfig config;
    double recall_vs_gt = 0, recall_vs_hnsw = 0;
    LatencyStats latency{};
    size_t total_accesses = 0, cache_hits = 0, cache_misses = 0;
    double hit_rate = 0;
    size_t evictions = 0, disk_reads = 0;
    size_t rss_before_mb = 0, rss_after_mb = 0;
    double theoretical_mem_mb = 0;
    double io_wait_us_per_query = 0, compute_us_per_query = 0;
};

// ============================================================
// JSON serialization
// ============================================================

static std::string resultToJsonStr(const TestResult& r) {
    std::ostringstream j;
    j << std::fixed << std::setprecision(4);
    j << "{";
    j << "\"group\":\"" << r.config.group << "\"";
    j << ",\"id\":\"" << escapeJson(r.config.id) << "\"";
    j << ",\"description\":\"" << escapeJson(r.config.description) << "\"";
    j << ",\"layout\":\"" << r.config.layout << "\"";
    j << ",\"policy\":\"" << r.config.policy << "\"";
    j << ",\"io_mode\":\"" << r.config.io_mode << "\"";
    j << ",\"latency_us\":" << r.config.latency;
    j << ",\"cache_slots\":" << r.config.cache_slots;
    j << ",\"ef\":" << r.config.ef;
    j << ",\"k\":" << r.config.k;
    j << ",\"is_baseline\":" << (r.config.is_baseline ? "true" : "false");
    j << ",\"recall_vs_gt\":" << r.recall_vs_gt;
    j << ",\"recall_vs_hnsw\":" << r.recall_vs_hnsw;
    j << ",\"mean_latency_us\":" << r.latency.mean;
    j << ",\"p50_latency_us\":" << r.latency.p50;
    j << ",\"p95_latency_us\":" << r.latency.p95;
    j << ",\"p99_latency_us\":" << r.latency.p99;
    j << ",\"total_time_ms\":" << r.latency.total_ms;
    j << ",\"total_accesses\":" << r.total_accesses;
    j << ",\"cache_hits\":" << r.cache_hits;
    j << ",\"cache_misses\":" << r.cache_misses;
    j << ",\"hit_rate\":" << r.hit_rate;
    j << ",\"evictions\":" << r.evictions;
    j << ",\"disk_reads\":" << r.disk_reads;
    j << ",\"rss_before_mb\":" << r.rss_before_mb;
    j << ",\"rss_after_mb\":" << r.rss_after_mb;
    j << ",\"theoretical_mem_mb\":" << r.theoretical_mem_mb;
    j << ",\"io_wait_us_per_query\":" << r.io_wait_us_per_query;
    j << ",\"compute_us_per_query\":" << r.compute_us_per_query;
    j << "}";
    return j.str();
}

// ============================================================
// Run DiskHNSW test
// ============================================================

static TestResult runDiskHNSWTest(
        const TestConfig& cfg,
        const std::string& graph_path, const std::string& bfs_path,
        const std::string& blocks_path, const std::string& route_path,
        const std::vector<float>& query_data, size_t num_query, int dim,
        const std::vector<std::vector<uint64_t>>& ground_truth,
        const std::vector<std::vector<uint64_t>>& hnsw_results, size_t k) {

    TestResult result;
    result.config = cfg;
    std::cout << "\n>>> Running " << cfg.id << ": " << cfg.description << std::endl;
    result.rss_before_mb = getRSS_MB();

    IOConfig io_config = buildIOConfig(cfg.io_mode, cfg.latency);

    // Read blocks header
    std::ifstream bfin(blocks_path, std::ios::binary);
    BlocksFileHeader bfhdr;
    bfin.read(reinterpret_cast<char*>(&bfhdr), sizeof(BlocksFileHeader));
    bfin.close();
    uint32_t num_blocks = bfhdr.num_blocks;
    uint32_t block_size = bfhdr.block_size;

    // Layout
    std::unique_ptr<LayoutProvider> layout;
    if (cfg.layout == "random") {
        std::ifstream gfin(graph_path, std::ios::binary);
        GraphHeader ghdr;
        gfin.read(reinterpret_cast<char*>(&ghdr), sizeof(GraphHeader));
        gfin.close();
        layout = std::make_unique<RandomLayoutProvider>(ghdr.num_nodes, num_blocks, 42);
    } else {
        layout = std::make_unique<BfsLayoutProvider>(route_path, num_blocks);
    }

    // Policy
    std::unique_ptr<ReplacementPolicy> policy;
    if (cfg.policy == "lfu") policy = std::make_unique<LFUPolicy>();
    else if (cfg.policy == "lru-k") policy = std::make_unique<LRUKPolicy>();
    else policy = std::make_unique<LRUPolicy>();

    // BlockCache
    auto cache = std::make_unique<BlockCache>(
        blocks_path, std::move(layout), std::move(policy),
        cfg.cache_slots, dim, io_config);

    // DiskHNSW
    DiskHNSW diskHnsw(graph_path, bfs_path, std::move(cache));
    diskHnsw.setEf(cfg.ef);

    // Theoretical memory
    {
        std::ifstream gfin(graph_path, std::ios::binary);
        GraphHeader ghdr;
        gfin.read(reinterpret_cast<char*>(&ghdr), sizeof(GraphHeader));
        gfin.close();
        size_t graph_mem = (size_t)ghdr.num_nodes * ghdr.dim * 4
                         + (size_t)ghdr.num_nodes * 4
                         + (size_t)ghdr.num_nodes * 8
                         + (size_t)ghdr.num_nodes * ghdr.maxM0 * 4 * 2;
        size_t bfs_mem = 2ULL * ghdr.num_nodes * 4;
        size_t cache_mem = (size_t)cfg.cache_slots * block_size;
        size_t route_mem = (size_t)ghdr.num_nodes * 4;
        result.theoretical_mem_mb = (double)(graph_mem + bfs_mem + cache_mem + route_mem) / (1024*1024);
    }

    result.rss_after_mb = getRSS_MB();

    // Search
    std::vector<std::vector<uint64_t>> disk_results(num_query);
    std::vector<double> disk_times(num_query);
    diskHnsw.resetCacheStats();

    for (size_t q = 0; q < num_query; q++) {
        auto qt0 = std::chrono::high_resolution_clock::now();
        auto res = diskHnsw.searchKnn(&query_data[q * dim], k);
        auto qt1 = std::chrono::high_resolution_clock::now();
        disk_times[q] = std::chrono::duration<double, std::micro>(qt1 - qt0).count();
        for (const auto& [dist, label] : res)
            disk_results[q].push_back(label);
    }

    result.latency = computeLatencyStats(disk_times);
    result.recall_vs_gt = computeRecall(ground_truth, disk_results, k);
    result.recall_vs_hnsw = computeRecall(hnsw_results, disk_results, k);

    const auto& stats = diskHnsw.getCacheStats();
    result.total_accesses = stats.total_accesses.load();
    result.cache_hits = stats.cache_hits.load();
    result.cache_misses = stats.cache_misses.load();
    result.hit_rate = result.total_accesses > 0 ? (double)result.cache_hits / result.total_accesses : 0;
    result.evictions = stats.evictions.load();
    result.disk_reads = stats.disk_reads.load();

    if (cfg.io_mode == "simulated" && cfg.latency > 0) {
        double total_io = result.cache_misses * cfg.latency;
        result.io_wait_us_per_query = total_io / num_query;
        result.compute_us_per_query = result.latency.mean - result.io_wait_us_per_query;
    }

    std::cout << "  Recall@GT: " << (result.recall_vs_gt*100) << "%"
              << " | Recall@HNSW: " << (result.recall_vs_hnsw*100) << "%"
              << " | Mean: " << result.latency.mean << "us"
              << " | Hit: " << (result.hit_rate*100) << "%"
              << " | RSS: " << result.rss_after_mb << "MB" << std::endl;

    return result;
}

// ============================================================
// Run HNSW baseline (D1)
// ============================================================

static TestResult runHNSWBaseline(
        const std::string& index_path,
        const std::vector<float>& query_data, size_t num_query, int dim,
        const std::vector<std::vector<uint64_t>>& ground_truth,
        size_t k, size_t ef,
        std::vector<std::vector<uint64_t>>& hnsw_results) {

    TestResult result;
    result.config.group = "D";
    result.config.id = "D1";
    result.config.description = "Full-memory HNSW (baseline)";
    result.config.is_baseline = true;
    result.config.ef = ef;
    result.config.k = k;

    std::cout << "\n>>> Running D1: Full-memory HNSW baseline" << std::endl;
    result.rss_before_mb = getRSS_MB();

    hnswlib::L2Space space(dim);
    auto* hnsw = new hnswlib::HierarchicalNSW<float>(&space, index_path);
    hnsw->setEf(ef);
    result.rss_after_mb = getRSS_MB();

    {
        std::ifstream f(index_path, std::ios::ate | std::ios::binary);
        result.theoretical_mem_mb = (double)f.tellg() / (1024*1024);
        f.close();
    }

    std::vector<double> hnsw_times(num_query);
    hnsw_results.resize(num_query);

    for (size_t q = 0; q < num_query; q++) {
        auto qt0 = std::chrono::high_resolution_clock::now();
        auto res = hnsw->searchKnn(&query_data[q * dim], k);
        auto qt1 = std::chrono::high_resolution_clock::now();
        hnsw_times[q] = std::chrono::duration<double, std::micro>(qt1 - qt0).count();
        while (!res.empty()) {
            hnsw_results[q].push_back(res.top().second);
            res.pop();
        }
        std::reverse(hnsw_results[q].begin(), hnsw_results[q].end());
    }

    result.latency = computeLatencyStats(hnsw_times);
    result.recall_vs_gt = computeRecall(ground_truth, hnsw_results, k);
    result.recall_vs_hnsw = 1.0;

    std::cout << "  Recall@GT: " << (result.recall_vs_gt*100) << "%"
              << " | Mean: " << result.latency.mean << "us"
              << " | RSS: " << result.rss_after_mb << "MB" << std::endl;

    delete hnsw;
    return result;
}

// ============================================================
// Main
// ============================================================

int main(int argc, char** argv) {
    if (argc < 9) {
        std::cerr << "Usage: " << argv[0]
                  << " <index.bin> <graph.bin> <bfs.bin> <blocks.bin> <route.bin>"
                  << " <data.fvecs> <query.fvecs> <gt.bin> [k=10] [ef=50] [output_dir=logs]"
                  << std::endl;
        return 1;
    }

    std::string index_path = argv[1];
    std::string graph_path = argv[2];
    std::string bfs_path = argv[3];
    std::string blocks_path = argv[4];
    std::string route_path = argv[5];
    std::string data_path = argv[6];
    std::string query_path = argv[7];
    std::string gt_path = argv[8];
    size_t k = argc > 9 ? std::stoul(argv[9]) : 10;
    size_t ef = argc > 10 ? std::stoul(argv[10]) : 50;
    std::string output_dir = argc > 11 ? argv[11] : "logs";

    std::cout << "=== SIFT1M Comprehensive Benchmark ===" << std::endl;
    std::cout << "k=" << k << ", ef=" << ef << std::endl;

    // Load data
    std::cout << "\n[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<float> base_data = read_fvecs(data_path, dim, num_base);
    std::vector<float> query_data = read_fvecs(query_path, dim, num_query);
    auto t1 = std::chrono::high_resolution_clock::now();
    std::cout << "  Base: " << num_base << ", Query: " << num_query << ", dim=" << dim << std::endl;
    std::cout << "  Load: " << std::chrono::duration<double>(t1-t0).count() << "s" << std::endl;

    // Load GT
    std::cout << "\n[2] Loading ground truth..." << std::endl;
    auto ground_truth = loadPrecomputedGT(gt_path);

    // Define configs
    std::vector<TestConfig> configs;

    // D: baseline comparison (prototype phase - only D group needed)
    { TestConfig c; c.group="D"; c.id="D2"; c.description="DiskHNSW BFS+LRU, cache=64, cached"; c.cache_slots=64; configs.push_back(c); }
    { TestConfig c; c.group="D"; c.id="D3"; c.description="DiskHNSW BFS+LRU, cache=256, cached"; c.cache_slots=256; configs.push_back(c); }
    { TestConfig c; c.group="D"; c.id="D4"; c.description="DiskHNSW BFS+LRU, cache=64, simulated 100us"; c.io_mode="simulated"; c.latency=100; c.cache_slots=64; configs.push_back(c); }
    { TestConfig c; c.group="D"; c.id="D5"; c.description="DiskHNSW BFS+LRU, cache=256, simulated 100us"; c.io_mode="simulated"; c.latency=100; c.cache_slots=256; configs.push_back(c); }

    for (auto& c : configs) { c.ef = ef; c.k = k; }

    std::cout << "\n[3] Configs: 1 baseline + " << configs.size() << " DiskHNSW" << std::endl;

    // Run D1 baseline
    std::cout << "\n[4] D1 baseline..." << std::endl;
    std::vector<std::vector<uint64_t>> hnsw_results;
    std::vector<TestResult> all_results;
    all_results.push_back(runHNSWBaseline(index_path, query_data, num_query, dim, ground_truth, k, ef, hnsw_results));

    // Run DiskHNSW tests (save incrementally)
    std::cout << "\n[5] DiskHNSW tests..." << std::endl;
    for (const auto& cfg : configs) {
        // Skip C2 (direct I/O fails on this system)
        if (cfg.id == "C2") {
            std::cout << "\n>>> Skipping " << cfg.id << " (direct I/O not supported)" << std::endl;
            TestResult er; er.config = cfg; er.config.description = "SKIPPED: O_DIRECT not supported";
            all_results.push_back(er);
            continue;
        }
        try {
            all_results.push_back(runDiskHNSWTest(cfg, graph_path, bfs_path, blocks_path, route_path,
                query_data, num_query, dim, ground_truth, hnsw_results, k));
        } catch (const std::exception& e) {
            std::cerr << "  ERROR in " << cfg.id << ": " << e.what() << std::endl;
            TestResult er; er.config = cfg; er.config.description = "ERROR: " + std::string(e.what());
            all_results.push_back(er);
        }
        // Incremental save after each test
        {
            std::ostringstream jtmp;
            jtmp << std::fixed << std::setprecision(4);
            jtmp << "{\"dataset\":\"SIFT1M\",\"num_base\":" << num_base
                << ",\"num_query\":" << num_query << ",\"dim\":" << dim
                << ",\"k\":" << k << ",\"ef\":" << ef << ",\"groups\":{";
            bool fg = true;
            for (char g : {'A','B','C','D'}) {
                bool has = false;
                for (const auto& r : all_results) if (r.config.group[0]==g) { has=true; break; }
                if (!has) continue;
                if (!fg) jtmp << ","; fg = false;
                jtmp << "\"" << g << "\":[";
                bool first = true;
                for (const auto& r : all_results) {
                    if (r.config.group[0] != g) continue;
                    if (!first) jtmp << ","; first = false;
                    jtmp << resultToJsonStr(r);
                }
                jtmp << "]";
            }
            jtmp << "}}";
            std::ofstream tmpout(output_dir + "/benchmark_1m_partial.json");
            tmpout << jtmp.str() << std::endl;
            tmpout.close();
        }
    }

    // JSON output
    std::cout << "\n[6] Generating JSON..." << std::endl;
    std::ostringstream j;
    j << std::fixed << std::setprecision(4);
    j << "{\"dataset\":\"SIFT1M\"";
    j << ",\"num_base\":" << num_base;
    j << ",\"num_query\":" << num_query;
    j << ",\"dim\":" << dim;
    j << ",\"k\":" << k;
    j << ",\"ef\":" << ef;
    j << ",\"groups\":{";
    bool first_g = true;
    for (char g : {'A','B','C','D'}) {
        bool has = false;
        for (const auto& r : all_results) if (r.config.group[0]==g) { has=true; break; }
        if (!has) continue;
        if (!first_g) j << ","; first_g = false;
        j << "\"" << g << "\":[";
        bool first = true;
        for (const auto& r : all_results) {
            if (r.config.group[0] != g) continue;
            if (!first) j << ","; first = false;
            j << resultToJsonStr(r);
        }
        j << "]";
    }
    j << "}";

    // D group analysis
    auto findR = [&](const std::string& id) -> const TestResult* {
        for (const auto& r : all_results) if (r.config.id==id) return &r;
        return nullptr;
    };
    j << ",\"d_group_analysis\":{";
    if (auto* d1=findR("D1")) if (auto* d2=findR("D2")) if (auto* d3=findR("D3")) if (auto* d4=findR("D4")) if (auto* d5=findR("D5")) {
        j << "\"d1_mean_us\":" << d1->latency.mean;
        j << ",\"d2_mean_us\":" << d2->latency.mean;
        j << ",\"d3_mean_us\":" << d3->latency.mean;
        j << ",\"d4_mean_us\":" << d4->latency.mean;
        j << ",\"d5_mean_us\":" << d5->latency.mean;
        j << ",\"d2_latency_ratio\":" << d2->latency.mean/d1->latency.mean;
        j << ",\"d3_latency_ratio\":" << d3->latency.mean/d1->latency.mean;
        j << ",\"d4_latency_ratio\":" << d4->latency.mean/d1->latency.mean;
        j << ",\"d5_latency_ratio\":" << d5->latency.mean/d1->latency.mean;
        j << ",\"d1_rss_mb\":" << d1->rss_after_mb;
        j << ",\"d2_rss_mb\":" << d2->rss_after_mb;
        j << ",\"d3_rss_mb\":" << d3->rss_after_mb;
        j << ",\"d4_rss_mb\":" << d4->rss_after_mb;
        j << ",\"d5_rss_mb\":" << d5->rss_after_mb;
        j << ",\"d1_theoretical_mem_mb\":" << d1->theoretical_mem_mb;
        j << ",\"d2_theoretical_mem_mb\":" << d2->theoretical_mem_mb;
        j << ",\"d3_theoretical_mem_mb\":" << d3->theoretical_mem_mb;
        j << ",\"d4_theoretical_mem_mb\":" << d4->theoretical_mem_mb;
        j << ",\"d5_theoretical_mem_mb\":" << d5->theoretical_mem_mb;
        if (d2->rss_after_mb>0) j << ",\"d2_mem_ratio\":" << (double)d1->rss_after_mb/d2->rss_after_mb;
        if (d3->rss_after_mb>0) j << ",\"d3_mem_ratio\":" << (double)d1->rss_after_mb/d3->rss_after_mb;
        j << ",\"cached_speedup_64to256\":" << d2->latency.mean/d3->latency.mean;
        j << ",\"simulated_speedup_64to256\":" << d4->latency.mean/d5->latency.mean;
        j << ",\"cached_hitrate_64\":" << d2->hit_rate;
        j << ",\"cached_hitrate_256\":" << d3->hit_rate;
        j << ",\"simulated_hitrate_64\":" << d4->hit_rate;
        j << ",\"simulated_hitrate_256\":" << d5->hit_rate;
        j << ",\"d4_io_wait_us\":" << d4->io_wait_us_per_query;
        j << ",\"d4_compute_us\":" << d4->compute_us_per_query;
        j << ",\"d5_io_wait_us\":" << d5->io_wait_us_per_query;
        j << ",\"d5_compute_us\":" << d5->compute_us_per_query;
        j << ",\"d1_p99_us\":" << d1->latency.p99;
        j << ",\"d4_p99_us\":" << d4->latency.p99;
        j << ",\"d5_p99_us\":" << d5->latency.p99;
        j << ",\"d4_p99_ratio\":" << d4->latency.p99/d1->latency.p99;
        j << ",\"d5_p99_ratio\":" << d5->latency.p99/d1->latency.p99;
        j << ",\"d1_recall_gt\":" << d1->recall_vs_gt;
        j << ",\"d2_recall_gt\":" << d2->recall_vs_gt;
        j << ",\"d3_recall_gt\":" << d3->recall_vs_gt;
        j << ",\"d4_recall_gt\":" << d4->recall_vs_gt;
        j << ",\"d5_recall_gt\":" << d5->recall_vs_gt;
    }
    j << "}}";

    std::string json_path = output_dir + "/benchmark_1m_results.json";
    std::ofstream jout(json_path);
    jout << j.str() << std::endl;
    jout.close();
    std::cout << "JSON saved to " << json_path << std::endl;

    // Summary table
    std::cout << "\n=== Summary ===" << std::endl;
    printf("| %-4s | %-45s | %7s | %7s | %9s | %9s | %6s | %8s |\n",
           "ID","Description","R@GT","R@HNSW","Mean(us)","P99(us)","Hit%","RSS(MB)");
    printf("|------|-----------------------------------------------|---------|---------|-----------|-----------|--------|----------|\n");
    for (const auto& r : all_results) {
        printf("| %-4s | %-45s | %6.2f%% | %6.2f%% | %9.1f | %9.1f | %5.1f%% | %6zuMB |\n",
               r.config.id.c_str(), r.config.description.c_str(),
               r.recall_vs_gt*100, r.recall_vs_hnsw*100,
               r.latency.mean, r.latency.p99, r.hit_rate*100, r.rss_after_mb);
    }

    // D group analysis
    std::cout << "\n=== D Group Analysis ===" << std::endl;
    if (auto* d1=findR("D1")) {
        std::cout << "\n1. Latency ratio (disk/hnsw):" << std::endl;
        std::cout << "   D1 mean = " << d1->latency.mean << " us (baseline)" << std::endl;
        if (auto* d2=findR("D2")) std::cout << "   D2/D1 = " << d2->latency.mean/d1->latency.mean << "x (cached, cache=64)" << std::endl;
        if (auto* d3=findR("D3")) std::cout << "   D3/D1 = " << d3->latency.mean/d1->latency.mean << "x (cached, cache=256)" << std::endl;
        if (auto* d4=findR("D4")) std::cout << "   D4/D1 = " << d4->latency.mean/d1->latency.mean << "x (simulated, cache=64)" << std::endl;
        if (auto* d5=findR("D5")) std::cout << "   D5/D1 = " << d5->latency.mean/d1->latency.mean << "x (simulated, cache=256)" << std::endl;

        std::cout << "\n2. Memory usage:" << std::endl;
        std::cout << "   D1 RSS = " << d1->rss_after_mb << " MB (full-memory HNSW)" << std::endl;
        if (auto* d2=findR("D2")) std::cout << "   D2 RSS = " << d2->rss_after_mb << " MB, ratio = " << (double)d1->rss_after_mb/d2->rss_after_mb << "x" << std::endl;
        if (auto* d3=findR("D3")) std::cout << "   D3 RSS = " << d3->rss_after_mb << " MB, ratio = " << (double)d1->rss_after_mb/d3->rss_after_mb << "x" << std::endl;

        std::cout << "\n3. Cache scaling (64 -> 256):" << std::endl;
        if (auto* d2=findR("D2")) if (auto* d3=findR("D3")) {
            std::cout << "   Cached: " << d2->latency.mean << " -> " << d3->latency.mean << " us (speedup " << d2->latency.mean/d3->latency.mean << "x)" << std::endl;
            std::cout << "   Hit rate: " << (d2->hit_rate*100) << "% -> " << (d3->hit_rate*100) << "%" << std::endl;
        }

        std::cout << "\n4. Simulated NVMe (P99):" << std::endl;
        std::cout << "   D1 P99 = " << d1->latency.p99 << " us" << std::endl;
        if (auto* d4=findR("D4")) {
            std::cout << "   D4 P99 = " << d4->latency.p99 << " us (ratio " << d4->latency.p99/d1->latency.p99 << "x)" << std::endl;
            std::cout << "   D4 I/O wait/q = " << d4->io_wait_us_per_query << " us, compute = " << d4->compute_us_per_query << " us" << std::endl;
        }
        if (auto* d5=findR("D5")) {
            std::cout << "   D5 P99 = " << d5->latency.p99 << " us (ratio " << d5->latency.p99/d1->latency.p99 << "x)" << std::endl;
            std::cout << "   D5 I/O wait/q = " << d5->io_wait_us_per_query << " us, compute = " << d5->compute_us_per_query << " us" << std::endl;
        }
    }

    std::cout << "\nDone!" << std::endl;
    return 0;
}
