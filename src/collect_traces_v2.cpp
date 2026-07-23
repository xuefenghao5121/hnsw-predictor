// collect_traces_v2.cpp - Phase 3a: 采集 Block 访问轨迹用于 Delta 分析和 LSTM 训练
//
// 采集内容:
//   1. 每次查询的 Block 访问序列 (有序, 含 hit/miss)
//   2. 每次查询的 Node 访问序列 (有序, 用于理解搜索路径)
//   3. 查询向量 (用于跨查询相似性分析)
//
// 用法:
//   ./collect_traces_v2 <graph> <bfs> <blocks> <route> <data> <query> <k> <ef> <cache_slots> <output_dir>
//
// 产出文件:
//   <output_dir>/block_traces.txt   - 每行: query_id block_id is_hit
//   <output_dir>/query_meta.txt     - 每行: query_id num_blocks num_nodes latency_us
//   <output_dir>/block_sequences.txt - 每行: query_id block_id1 block_id2 ... (空格分隔)

#include "common.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include "disk_hnsw.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>
#include <set>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc < 11) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph> <bfs> <blocks> <route> <data> <query> <k> <ef> <cache_slots> <output_dir>"
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
    std::string output_dir = argv[10];

    // 创建输出目录
    mkdir(output_dir.c_str(), 0755);

    // 加载数据
    std::cout << "[1] Loading data..." << std::endl;
    int dim;
    size_t num_base, num_query;
    auto base_data = read_fvecs(data_path, dim, num_base);
    auto query_data = read_fvecs(query_path, dim, num_query);
    if (num_query > 1000) {
        query_data.resize(1000 * dim);
        num_query = 1000;
    }
    std::cout << "  Base: " << num_base << ", Query: " << num_query << ", dim=" << dim << std::endl;

    // 释放 base_data
    std::vector<float>().swap(base_data);

    // 构建 DiskHNSW
    std::cout << "[2] Building DiskHNSW (cache_slots=" << cache_slots << ")..." << std::endl;
    DiskHNSW diskHnsw(graph_path, bfs_path, blocks_path, route_path, cache_slots, dim);
    diskHnsw.setEf(ef);

    // 打开输出文件
    std::string block_traces_path = output_dir + "/block_traces.txt";
    std::string query_meta_path = output_dir + "/query_meta.txt";
    std::string block_seq_path = output_dir + "/block_sequences.txt";

    std::ofstream fout_blocks(block_traces_path);
    std::ofstream fout_meta(query_meta_path);
    std::ofstream fout_seq(block_seq_path);

    if (!fout_blocks.is_open() || !fout_meta.is_open() || !fout_seq.is_open()) {
        std::cerr << "Cannot create output files in " << output_dir << std::endl;
        return 1;
    }

    fout_blocks << "# query_id block_id is_hit (1=hit, 0=miss)\n";
    fout_meta << "# query_id num_block_accesses num_unique_blocks latency_us\n";
    fout_seq << "# query_id block_id1 block_id2 ... (ordered access sequence, may have repeats)\n";

    // 采集轨迹
    std::cout << "[3] Collecting traces for " << num_query << " queries..." << std::endl;

    size_t total_accesses = 0;
    size_t total_hits = 0;
    size_t total_unique_blocks = 0;

    for (size_t q = 0; q < num_query; q++) {
        std::vector<std::pair<uint32_t, bool>> query_trace;

        // 设置 trace 回调
        diskHnsw.setTraceCallback([&query_trace](uint32_t block_id, bool is_hit) {
            query_trace.push_back({block_id, is_hit});
        });

        auto t0 = std::chrono::high_resolution_clock::now();
        auto res = diskHnsw.searchKnn(&query_data[q * dim], k);
        auto t1 = std::chrono::high_resolution_clock::now();
        diskHnsw.clearTraceCallback();

        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        // 统计 unique blocks
        std::set<uint32_t> unique_blocks;
        for (const auto& [bid, hit] : query_trace) {
            unique_blocks.insert(bid);
        }

        // 写入 block traces
        for (const auto& [block_id, is_hit] : query_trace) {
            fout_blocks << q << " " << block_id << " " << (is_hit ? 1 : 0) << "\n";
            total_accesses++;
            if (is_hit) total_hits++;
        }

        // 写入 query meta
        fout_meta << q << " " << query_trace.size() << " "
                  << unique_blocks.size() << " "
                  << (size_t)us << "\n";
        total_unique_blocks += unique_blocks.size();

        // 写入 block sequence (一行一个查询)
        fout_seq << q;
        for (const auto& [block_id, is_hit] : query_trace) {
            fout_seq << " " << block_id;
        }
        fout_seq << "\n";

        if ((q + 1) % 100 == 0) {
            std::cout << "  Query " << (q + 1) << "/" << num_query
                      << " (blocks=" << query_trace.size()
                      << ", unique=" << unique_blocks.size()
                      << ", latency=" << (us / 1000) << "ms)" << std::endl;
        }
    }

    fout_blocks.close();
    fout_meta.close();
    fout_seq.close();

    double overall_hit = total_accesses > 0 ? (double)total_hits / total_accesses * 100 : 0;
    double avg_unique = num_query > 0 ? (double)total_unique_blocks / num_query : 0;

    std::cout << "\n[4] Trace collection complete!" << std::endl;
    std::cout << "    Output dir: " << output_dir << std::endl;
    std::cout << "    Total queries: " << num_query << std::endl;
    std::cout << "    Total block accesses: " << total_accesses << std::endl;
    std::cout << "    Overall hit rate: " << overall_hit << "%" << std::endl;
    std::cout << "    Avg unique blocks/query: " << avg_unique << std::endl;
    std::cout << "    Avg accesses/query: " << (double)total_accesses / num_query << std::endl;

    return 0;
}
