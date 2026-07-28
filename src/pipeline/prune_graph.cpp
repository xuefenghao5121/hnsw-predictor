// prune_graph.cpp - 图裁剪工具
//
// 支持两种裁剪模式:
//   mrng: MRNG (Monotonic Relative Neighborhood Graph) 裁剪
//         保留 v 当且仅当不存在已保留的 w 使得 dist(v,w) < dist(u,v)
//   cap:  度数上限裁剪, 保留最近的 R_max 个邻居
//
// 用法: ./build/prune_graph <graph.bin> <base.fvecs> <output_graph.bin> [R_min=5] [R_max=32] [mode=mrng]
//
// 产出: 裁剪后的 graph.bin (格式与原始相同, 可直接用于后续 pipeline)

#include "common.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <chrono>

float l2Dist(const float* a, const float* b, int dim) {
    float s = 0.0f;
    for (int i = 0; i < dim; i++) {
        float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <graph.bin> <base.fvecs> <output_graph.bin> [R_min=5] [R_max=32] [mode=mrng]" << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string vec_path = argv[2];
    std::string out_path = argv[3];
    int R_min = argc > 4 ? std::atoi(argv[4]) : 5;
    int R_max = argc > 5 ? std::atoi(argv[5]) : 32;
    std::string mode = argc > 6 ? argv[6] : "mrng";

    std::cout << "=== Graph Pruning (mode=" << mode << ") ===" << std::endl;
    std::cout << "Input: " << graph_path << std::endl;
    std::cout << "Vectors: " << vec_path << std::endl;
    std::cout << "Output: " << out_path << std::endl;
    std::cout << "R_min=" << R_min << ", R_max=" << R_max << std::endl;

    auto t0 = std::chrono::high_resolution_clock::now();
    std::cout << "\nLoading graph..." << std::endl;
    GraphStructure g = load_graph_structure(graph_path);
    auto t1 = std::chrono::high_resolution_clock::now();
    std::cout << "Loaded in " << std::chrono::duration<double>(t1 - t0).count() << "s" << std::endl;

    if (g.vectors.empty()) {
        std::cout << "Graph has no vectors, loading from " << vec_path << "..." << std::endl;
        int dim;
        size_t count;
        g.vectors = read_fvecs(vec_path, dim, count);
        g.dim = dim;
        if (count != g.num_nodes) {
            std::cerr << "ERROR: vector count mismatch" << std::endl;
            return 1;
        }
    }

    size_t total_edges_before = 0;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        total_edges_before += g.adjacency0[i].size();
    }
    std::cout << "\nOriginal: " << total_edges_before << " edges, avg degree "
              << (double)total_edges_before / g.num_nodes << std::endl;

    auto t2 = std::chrono::high_resolution_clock::now();
    if (mode == "cap") {
        std::cout << "\nApplying degree-cap pruning (keep closest " << R_max << ")..." << std::endl;
    } else {
        std::cout << "\nApplying MRNG pruning..." << std::endl;
    }

    size_t total_edges_after = 0;
    int max_degree_after = 0;
    int min_degree_after = 999;
    std::vector<int> degree_hist(33, 0);

    for (uint32_t u = 0; u < g.num_nodes; u++) {
        auto& nbrs = g.adjacency0[u];
        if ((int)nbrs.size() <= R_max) {
            total_edges_after += nbrs.size();
            max_degree_after = std::max(max_degree_after, (int)nbrs.size());
            min_degree_after = std::min(min_degree_after, (int)nbrs.size());
            if (nbrs.size() < 33) degree_hist[nbrs.size()]++;
            continue;
        }

        const float* u_vec = &g.vectors[u * g.dim];
        std::vector<std::pair<float, uint32_t>> nbr_dist;
        nbr_dist.reserve(nbrs.size());
        for (uint32_t v : nbrs) {
            float d = l2Dist(u_vec, &g.vectors[v * g.dim], g.dim);
            nbr_dist.emplace_back(d, v);
        }
        std::sort(nbr_dist.begin(), nbr_dist.end());

        std::vector<uint32_t> kept;

        if (mode == "cap") {
            for (int i = 0; i < R_max && i < (int)nbr_dist.size(); i++) {
                kept.push_back(nbr_dist[i].second);
            }
        } else {
            std::vector<std::vector<float>> kept_vecs;
            for (auto& [dist_uv, v] : nbr_dist) {
                if ((int)kept.size() >= R_max) break;
                bool keep = true;
                const float* v_vec = &g.vectors[v * g.dim];
                for (auto& w_vec : kept_vecs) {
                    float dist_vw = l2Dist(v_vec, w_vec.data(), g.dim);
                    if (dist_vw < dist_uv) {
                        keep = false;
                        break;
                    }
                }
                if (keep) {
                    kept.push_back(v);
                    kept_vecs.emplace_back(v_vec, v_vec + g.dim);
                }
            }
            if ((int)kept.size() < R_min) {
                for (auto& [dist_uv, v] : nbr_dist) {
                    if ((int)kept.size() >= R_min) break;
                    if (std::find(kept.begin(), kept.end(), v) == kept.end()) {
                        kept.push_back(v);
                    }
                }
            }
        }

        nbrs = std::move(kept);
        total_edges_after += nbrs.size();
        int deg = (int)nbrs.size();
        max_degree_after = std::max(max_degree_after, deg);
        min_degree_after = std::min(min_degree_after, deg);
        if (deg < 33) degree_hist[deg]++;

        if (u % 100000 == 0 && u > 0) {
            std::cout << "  " << u / 10000 << "0% done..." << std::endl;
        }
    }

    auto t3 = std::chrono::high_resolution_clock::now();
    std::cout << "\nPruning complete in " << std::chrono::duration<double>(t3 - t2).count() << "s" << std::endl;
    std::cout << "After pruning: " << total_edges_after << " edges, avg degree "
              << (double)total_edges_after / g.num_nodes << std::endl;
    std::cout << "Edge reduction: " << (1.0 - (double)total_edges_after / total_edges_before) * 100 << "%" << std::endl;
    std::cout << "Degree range: [" << min_degree_after << ", " << max_degree_after << "]" << std::endl;

    for (int d = 0; d <= 32; d++) {
        if (degree_hist[d] > 0) {
            std::cout << "  deg=" << d << ": " << degree_hist[d] << " ("
                      << (double)degree_hist[d] / g.num_nodes * 100 << "%)" << std::endl;
        }
    }

    std::cout << "\nSaving pruned graph to " << out_path << "..." << std::endl;
    save_graph_structure(out_path, g);
    std::cout << "Done." << std::endl;

    return 0;
}
