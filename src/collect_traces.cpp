// collect_traces.cpp - Task 3.1: 采集 Block 访问轨迹
//
// 用法:
//   ./collect_traces <graph.bin> <bfs.bin> <blocks.bin> <route.bin> \
//                    <data.fvecs> <query.fvecs> <k> <ef> <cache_slots> <output_trace.txt>
#include "common.h"
#include "block_cache.h"
#include "disk_hnsw.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 11) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph> <bfs> <blocks> <route> <data> <query> <k> <ef> <cache_slots> <output_trace>"
                  << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string blocks_path = argv[3];
    std::string route_path = argv[4];
    std::string data_path = argv[5];
    std::string query_path = argv[6];
    int k = std::atoi(argv[7]);
    int ef = std::atoi(argv[8]);
    size_t cache_slots = std::atoll(argv[9]);
    std::string output_path = argv[10];

    // 加载数据
    std::cout << "[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    std::cout << "  Base: " << num_base << ", Query: " << num_query << ", dim=" << dim << std::endl;

    // 构建 DiskHNSW
    std::cout << "[2] Building DiskHNSW (cache_slots=" << cache_slots << ")..." << std::endl;
    DiskHNSW diskHnsw(graph_path, bfs_path, blocks_path, route_path, cache_slots, dim);
    diskHnsw.setEf(ef);

    // 打开轨迹文件
    std::ofstream trace_out(output_path);
    if (!trace_out.is_open()) {
        std::cerr << "Cannot create trace file: " << output_path << std::endl;
        return 1;
    }
    trace_out << "# Block access traces for Markov training\n";
    trace_out << "# Format: query_id block_id timestamp_us is_hit\n";

    // 采集轨迹
    std::cout << "[3] Collecting traces..." << std::endl;
    size_t total_accesses = 0;
    size_t total_hits = 0;

    for (size_t q = 0; q < num_query; q++) {
        std::vector<std::pair<uint32_t, bool>> query_trace;  // (block_id, is_hit)

        // 设置 trace 回调
        diskHnsw.setTraceCallback([&query_trace](uint32_t block_id, bool is_hit) {
            query_trace.push_back({block_id, is_hit});
        });

        auto t0 = std::chrono::high_resolution_clock::now();
        auto res = diskHnsw.searchKnn(&query_data[q * dim], k);
        auto t1 = std::chrono::high_resolution_clock::now();

        diskHnsw.clearTraceCallback();

        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        // 写入轨迹
        for (const auto& [block_id, is_hit] : query_trace) {
            trace_out << q << " " << block_id << " " << us << " " << (is_hit ? 1 : 0) << "\n";
            total_accesses++;
            if (is_hit) total_hits++;
        }

        if ((q + 1) % 100 == 0) {
            double hit_rate = query_trace.empty() ? 0 :
                (double)std::count_if(query_trace.begin(), query_trace.end(),
                    [](const auto& p) { return p.second; }) / query_trace.size();
            std::cout << "  Query " << (q + 1) << "/" << num_query
                      << " (blocks=" << query_trace.size()
                      << ", hit_rate=" << (hit_rate * 100) << "%)" << std::endl;
        }
    }

    trace_out.close();

    double overall_hit = total_accesses > 0 ? (double)total_hits / total_accesses * 100 : 0;
    std::cout << "[4] Traces saved to " << output_path << std::endl;
    std::cout << "    Total accesses: " << total_accesses << std::endl;
    std::cout << "    Overall hit rate: " << overall_hit << "%" << std::endl;

    return 0;
}
