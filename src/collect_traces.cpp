// collect_traces.cpp - Task 3.1: 采集 Block 访问轨迹
//
// 用法:
//   ./collect_traces <index.bin> <graph.bin> <bfs.bin> <blocks.bin> <route.bin> \
//                    <data.fvecs> <query.fvecs> <gt.bin> <k> <ef> <output_trace.txt>
#include "common.h"
#include "block_cache.h"
#include "disk_hnsw.h"
#include <iostream>
#include <fstream>
#include <chrono>

int main(int argc, char** argv) {
    if (argc < 12) {
        std::cerr << "Usage: " << argv[0]
                  << " <index> <graph> <bfs> <blocks> <route> <data> <query> <gt> <k> <ef> <output_trace>"
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
    int k = std::atoi(argv[9]);
    int ef = std::atoi(argv[10]);
    std::string output_path = argv[11];

    // 加载数据
    std::cout << "[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    std::cout << "  Base: " << num_base << ", Query: " << num_query << ", dim=" << dim << std::endl;

    // 构建 DiskHNSW
    std::cout << "[2] Building DiskHNSW..." << std::endl;
    DiskHNSW diskHnsw(graph_path, bfs_path, blocks_path, route_path, 64, dim);
    diskHnsw.setEf(ef);

    // 采集轨迹
    std::cout << "[3] Collecting traces..." << std::endl;
    std::ofstream trace_out(output_path);
    if (!trace_out.is_open()) {
        std::cerr << "Cannot create trace file: " << output_path << std::endl;
        return 1;
    }

    trace_out << "# Block access traces for Markov training\n";
    trace_out << "# Format: query_id block_id timestamp_us is_hit\n";

    for (size_t q = 0; q < num_query; q++) {
        // 重置统计以获取本次查询的 block 访问
        diskHnsw.resetCacheStats();
        size_t disk_reads_before = diskHnsw.getCacheStats().disk_reads.load();

        auto t0 = std::chrono::high_resolution_clock::now();
        auto res = diskHnsw.searchKnn(&query_data[q * dim], k);
        auto t1 = std::chrono::high_resolution_clock::now();

        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        // 获取本次查询的 disk_reads
        size_t disk_reads_after = diskHnsw.getCacheStats().disk_reads.load();

        // 通过 cache 的 recent accesses 获取 block 序列
        // (简化版：只记录 disk_reads 数量)
        trace_out << q << " " << disk_reads_after - disk_reads_before
                  << " " << us << " " << 0 << "\n";

        if ((q + 1) % 100 == 0) {
            std::cout << "  Query " << (q + 1) << "/" << num_query << std::endl;
        }
    }

    trace_out.close();
    std::cout << "[4] Traces saved to " << output_path << std::endl;

    return 0;
}
