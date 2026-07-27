// benchmark_hnswlib_native.cpp - Standalone hnswlib native benchmark with GT recall
// Usage: ./benchmark_hnswlib_native <index> <query.fvecs> <gt.bin> <k> <ef> <num_queries>
#include "hnswlib/hnswlib.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <vector>
#include <sys/resource.h>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstdint>

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

int main(int argc, char** argv) {
    if (argc < 7) {
        std::cerr << "Usage: " << argv[0] << " <index> <query.fvecs> <gt.bin> <k> <ef> <num_queries>" << std::endl;
        return 1;
    }
    std::string index_path = argv[1];
    std::string query_path = argv[2];
    std::string gt_path = argv[3];
    int k = atoi(argv[4]);
    int ef = atoi(argv[5]);
    int num_queries = atoi(argv[6]);

    // Load query data
    std::ifstream qf(query_path, std::ios::binary);
    if (!qf) { std::cerr << "Cannot open query file" << std::endl; return 1; }
    int dim;
    qf.read(reinterpret_cast<char*>(&dim), sizeof(int));
    qf.seekg(0, std::ios::end);
    size_t file_size = qf.tellg();
    size_t row_size = sizeof(int) + dim * sizeof(float);
    size_t total_queries = file_size / row_size;
    if ((size_t)num_queries > total_queries) num_queries = total_queries;

    std::vector<float> query_data((size_t)num_queries * dim);
    qf.seekg(0, std::ios::beg);
    for (int i = 0; i < num_queries; i++) {
        int d;
        qf.read(reinterpret_cast<char*>(&d), sizeof(int));
        qf.read(reinterpret_cast<char*>(&query_data[i * dim]), dim * sizeof(float));
    }
    qf.close();

    // Load GT: header(8B) + n * kk * uint64
    std::vector<std::vector<uint64_t>> gt(num_queries);
    {
        std::ifstream gf(gt_path, std::ios::binary);
        if (!gf) { std::cerr << "Cannot open GT file" << std::endl; return 1; }
        uint32_t n_gt, kk;
        gf.read(reinterpret_cast<char*>(&n_gt), sizeof(uint32_t));
        gf.read(reinterpret_cast<char*>(&kk), sizeof(uint32_t));
        for (int i = 0; i < num_queries; i++) {
            gt[i].resize(k);
            gf.read(reinterpret_cast<char*>(gt[i].data()), k * sizeof(uint64_t));
        }
    }

    std::cout << "=== hnswlib Native Benchmark ===" << std::endl;
    std::cout << "Queries: " << num_queries << ", k=" << k << ", ef=" << ef << ", dim=" << dim << std::endl;

    // Load index
    auto t0 = std::chrono::high_resolution_clock::now();
    hnswlib::L2Space space(dim);
    hnswlib::HierarchicalNSW<float> alg(&space, index_path);
    alg.setEf(ef);
    auto t1 = std::chrono::high_resolution_clock::now();
    std::cout << "Index loaded in " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;
    std::cout << "RSS after load: " << getRSS_MB() << " MB" << std::endl;

    // CPU warmup: spin to ramp up frequency (governor=performance)
    volatile double warm_sink = 0;
    for (int i = 0; i < 1000000; i++) warm_sink += i * 1.1;
    if (warm_sink < 0) std::cerr << "(warmup sink)";

    // Search warmup: run all queries once to warm cache + CPU
    for (int i = 0; i < num_queries; i++)
        alg.searchKnn(&query_data[i * dim], k);

    // Timed search
    std::vector<double> latencies(num_queries);
    std::vector<std::vector<std::pair<float, uint32_t>>> all_results(num_queries);
    auto tt0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_queries; i++) {
        auto q0 = std::chrono::high_resolution_clock::now();
        auto result = alg.searchKnn(&query_data[i * dim], k);
        auto q1 = std::chrono::high_resolution_clock::now();
        latencies[i] = std::chrono::duration<double, std::micro>(q1 - q0).count();
        while (!result.empty()) {
            auto& [dist, id] = result.top();
            all_results[i].push_back({(float)dist, (uint32_t)id});
            result.pop();
        }
    }
    auto tt1 = std::chrono::high_resolution_clock::now();
    double total_s = std::chrono::duration<double>(tt1 - tt0).count();

    // Compute recall vs GT
    size_t correct = 0;
    size_t total = 0;
    for (int i = 0; i < num_queries; i++) {
        std::set<uint64_t> gt_set(gt[i].begin(), gt[i].end());
        for (auto& [dist, id] : all_results[i]) {
            if (gt_set.count(id)) correct++;
        }
        total += k;
    }
    double recall = (double)correct / total * 100;

    // Compute stats
    std::sort(latencies.begin(), latencies.end());
    double mean_us = 0;
    for (auto l : latencies) mean_us += l;
    mean_us /= num_queries;
    double p50 = latencies[num_queries / 2];
    double p95 = latencies[(size_t)(num_queries * 0.95)];
    double p99 = latencies[(size_t)(num_queries * 0.99)];
    double qps = num_queries / total_s;

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Recall: " << recall << "%" << std::endl;
    std::cout << "Mean:   " << mean_us / 1000.0 << " ms" << std::endl;
    std::cout << "P50:    " << p50 / 1000.0 << " ms" << std::endl;
    std::cout << "P95:    " << p95 / 1000.0 << " ms" << std::endl;
    std::cout << "P99:    " << p99 / 1000.0 << " ms" << std::endl;
    std::cout << std::setprecision(1);
    std::cout << "QPS:    " << qps << std::endl;
    std::cout << "RSS:    " << getRSS_MB() << " MB" << std::endl;

    return 0;
}
