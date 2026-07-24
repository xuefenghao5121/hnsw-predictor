// 精确对比 blocking vs non-blocking，使用 F0 baseline (full-memory) 作为参考
#include "disk_hnsw.h"
#include "block_cache.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <set>

std::vector<float> read_fvecs(const std::string& path, int& dim, int& num) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "Cannot open " << path << "\n"; exit(1); }
    f.read(reinterpret_cast<char*>(&dim), 4);
    f.seekg(0, std::ios::end);
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    num = size / (4 + 4 * dim);
    std::vector<float> data((size_t)num * dim);
    std::vector<int> d(1);
    for (int i = 0; i < num; i++) {
        f.read(reinterpret_cast<char*>(d.data()), 4);
        f.read(reinterpret_cast<char*>(&data[(size_t)i * dim]), 4LL * dim);
    }
    return data;
}

int main(int argc, char** argv) {
    if (argc < 8) {
        std::cerr << "Usage: " << argv[0] << " <graph> <bfs> <blocks> <route> <base.fvecs> <query.fvecs> <gt.bin> [k=10] [ef=50] [n_queries=200]\n";
        return 1;
    }
    int k = argc > 8 ? atoi(argv[8]) : 10;
    int ef = argc > 9 ? atoi(argv[9]) : 50;
    int n_queries = argc > 10 ? atoi(argv[10]) : 200;

    int dim, num_base, num_query, gt_dim, gt_num;
    auto queries = read_fvecs(argv[6], dim, num_query);
    auto gt_raw = [&]() {
        std::ifstream f(argv[7], std::ios::binary);
        if (!f) return std::vector<uint32_t>();
        f.read(reinterpret_cast<char*>(&gt_dim), 4);
        f.seekg(0, std::ios::end);
        auto size = f.tellg();
        f.seekg(0, std::ios::beg);
        gt_num = size / (4 + 4 * gt_dim);
        std::vector<uint32_t> data((size_t)gt_num * gt_dim);
        std::vector<int> d(1);
        for (int i = 0; i < gt_num; i++) {
            f.read(reinterpret_cast<char*>(d.data()), 4);
            f.read(reinterpret_cast<char*>(&data[(size_t)i * gt_dim]), 4LL * gt_dim);
        }
        return data;
    }();

    n_queries = std::min(n_queries, num_query);

    // ======== F0 Baseline: Full-memory ========
    DiskHNSW hnsw_f0(argv[1], argv[2], argv[3], argv[4], 2225, dim);  // 全部 block 都缓存
    hnsw_f0.setEf(ef);
    // 不启用 graph prefetch, 全内存

    std::cout << "F0 baseline (full-memory)...\n";
    // warmup
    for (int i = 0; i < 50 && i < n_queries; i++)
        hnsw_f0.searchKnn(&queries[(size_t)i * dim], k);

    std::vector<std::set<uint64_t>> f0_sets(n_queries);
    for (int i = 0; i < n_queries; i++) {
        auto res = hnsw_f0.searchKnn(&queries[(size_t)i * dim], k);
        for (auto& r : res) f0_sets[i].insert(r.second);
    }

    // ======== Blocking (F2-single) ========
    DiskHNSW hnsw_b(argv[1], argv[2], argv[3], argv[4], 1024, dim);
    hnsw_b.setEf(ef);
    hnsw_b.enableGraphPrefetch(true);

    std::cout << "Blocking search (cache=1024)...\n";
    for (int i = 0; i < 50 && i < n_queries; i++)
        hnsw_b.searchKnn(&queries[(size_t)i * dim], k);

    std::vector<std::set<uint64_t>> block_sets(n_queries);
    for (int i = 0; i < n_queries; i++) {
        auto res = hnsw_b.searchKnn(&queries[(size_t)i * dim], k);
        for (auto& r : res) block_sets[i].insert(r.second);
    }

    // ======== Non-blocking (batchSearch bs=1) ========
    DiskHNSW hnsw_nb(argv[1], argv[2], argv[3], argv[4], 1024, dim);
    hnsw_nb.setEf(ef);
    hnsw_nb.enableGraphPrefetch(true);

    std::cout << "Non-blocking search (cache=1024, bs=1)...\n";
    for (int i = 0; i < 50 && i < n_queries; i++) {
        std::vector<float> q(&queries[(size_t)i * dim], &queries[(size_t)i * dim] + dim);
        hnsw_nb.batchSearch(q, k, 1);
    }

    std::vector<std::set<uint64_t>> nb_sets(n_queries);
    for (int i = 0; i < n_queries; i++) {
        std::vector<float> q(&queries[(size_t)i * dim], &queries[(size_t)i * dim] + dim);
        auto res = hnsw_nb.batchSearch(q, k, 1);
        for (auto& r : res[0]) nb_sets[i].insert(r.second);
    }

    // ======== 对比 ========
    int block_correct = 0, nb_correct = 0;
    int block_mismatch = 0, nb_mismatch = 0;
    int total = n_queries * k;

    for (int i = 0; i < n_queries; i++) {
        for (auto l : block_sets[i]) if (f0_sets[i].count(l)) block_correct++;
        for (auto l : nb_sets[i]) if (f0_sets[i].count(l)) nb_correct++;

        if (block_sets[i] != f0_sets[i]) block_mismatch++;
        if (nb_sets[i] != f0_sets[i]) {
            nb_mismatch++;
            if (nb_mismatch <= 10) {
                std::vector<uint64_t> only_f0, only_nb;
                for (auto l : f0_sets[i]) if (!nb_sets[i].count(l)) only_f0.push_back(l);
                for (auto l : nb_sets[i]) if (!f0_sets[i].count(l)) only_nb.push_back(l);
                std::cout << "  Q" << i << ": ";
                std::cout << "F0-only={" ;
                for (auto l : only_f0) std::cout << l << " ";
                std::cout << "} NB-extra={";
                for (auto l : only_nb) std::cout << l << " ";
                std::cout << "}\n";
            }
        }
    }

    std::cout << "\n=== Summary (vs F0 baseline) ===\n";
    std::cout << "Blocking recall@HNSW:     " << (100.0 * block_correct / total) << "% (mismatch=" << block_mismatch << "/" << n_queries << ")\n";
    std::cout << "Non-blocking recall@HNSW: " << (100.0 * nb_correct / total) << "% (mismatch=" << nb_mismatch << "/" << n_queries << ")\n";

    return 0;
}
