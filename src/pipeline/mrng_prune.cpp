// mrng_prune.cpp - Keep nearest K neighbors by vector distance (simple MRNG variant)
//
// For HNSW: instead of HNSW's graph construction, keep each node's top-K nearest vector neighbors
// to reduce edge count while maintaining search quality.
//
// Usage: ./mrng_prune <graph.bin> <data.fvecs> <output_graph.bin> [K]
#include "common.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>
#include <fstream>
#include <iostream>
#include <chrono>

// Read fvecs file
std::vector<float> read_fvecs(const std::string& path, int& n, int& dim) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open: " + path);
    f.seekg(0, std::ios::end);
    size_t size = f.tellg();
    f.seekg(0);
    // Read first dim
    int d;
    f.read(reinterpret_cast<char*>(&d), 4);
    dim = d;
    size_t per_vec = 4 + d * 4;
    n = size / per_vec;
    f.seekg(0);
    std::vector<float> data((size_t)n * dim);
    for (int i = 0; i < n; i++) {
        int dd;
        f.read(reinterpret_cast<char*>(&dd), 4);
        f.read(reinterpret_cast<char*>(&data[(size_t)i * dim]), dim * 4);
    }
    std::cout << "Loaded " << n << " vectors, dim=" << dim << std::endl;
    return data;
}

float l2_dist(const float* a, const float* b, int dim) {
    float s = 0;
    for (int i = 0; i < dim; i++) {
        float d = a[i] - b[i];
        s += d * d;
    }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <graph.bin> <data.fvecs> <output.bin> [K]\n";
        return 1;
    }
    int K = argc > 4 ? atoi(argv[4]) : 16;  // default: top-16 nearest neighbors
    
    std::cout << "Keeping top K=" << K << " nearest neighbors per node (by vector distance)\n";

    // Load graph
    std::ifstream gf(argv[1], std::ios::binary);
    GraphHeader hdr;
    gf.read(reinterpret_cast<char*>(&hdr), sizeof(GraphHeader));
    uint32_t pad;
    gf.read(reinterpret_cast<char*>(&pad), sizeof(uint32_t));
    
    int n = hdr.num_nodes;
    int dim = hdr.dim;
    std::cout << "Graph: n=" << n << ", dim=" << dim << ", maxM0=" << hdr.maxM0 << std::endl;

    // Load vectors
    int vn, vdim;
    auto vectors = read_fvecs(argv[2], vn, vdim);
    if (vdim != dim || vn < n) {
        std::cerr << "Vector mismatch: " << vdim << " vs " << dim << std::endl;
        return 1;
    }

    // Skip levels
    gf.seekg(40 + (size_t)n * 4);

    // Skip upper vectors and labels
    gf.seekg(40 + (size_t)n * 4 + (size_t)n * hdr.data_size);
    gf.seekg(40 + (size_t)n * 4 + (size_t)n * hdr.data_size + (size_t)n * 8);

    // Read L0 adjacency (we won't use it, just read to skip)
    std::vector<std::vector<uint32_t>> old_adjacency(n);
    for (int i = 0; i < n; i++) {
        uint16_t cnt;
        gf.read(reinterpret_cast<char*>(&cnt), 2);
        old_adjacency[i].resize(cnt);
        if (cnt > 0) {
            gf.read(reinterpret_cast<char*>(old_adjacency[i].data()), cnt * 4);
        }
    }
    gf.close();

    // Compute new adjacency: keep top-K nearest vector neighbors for each node
    auto t0 = std::chrono::high_resolution_clock::now();
    std::vector<std::vector<uint32_t>> new_adjacency(n);
    size_t total_edges = 0;
    int max_degree = 0;

    for (int i = 0; i < n; i++) {
        const float* vi = &vectors[(size_t)i * dim];
        std::vector<std::pair<float, uint32_t>> dists;
        dists.reserve(200); // up to maxM0=32 * 6, but we'll compute for all old neighbors
        
        // For this first version: just use the old adjacency as candidates, rank by vector distance
        for (uint32_t nb : old_adjacency[i]) {
            float d = l2_dist(vi, &vectors[(size_t)nb * dim], dim);
            dists.emplace_back(d, nb);
        }
        // Also ensure self is not in candidates (but old adjacency probably doesn't have self)
        // sort ascending, keep top K
        std::sort(dists.begin(), dists.end());
        int keep = std::min((int)dists.size(), K);
        new_adjacency[i].reserve(keep);
        for (int j = 0; j < keep; j++) {
            new_adjacency[i].push_back(dists[j].second);
        }
        total_edges += new_adjacency[i].size();
        max_degree = std::max(max_degree, (int)new_adjacency[i].size());
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "New graph: " << total_edges << " edges (" << (double)total_edges / n << " avg, max=" << max_degree << " )" << std::endl;
    std::cout << "Reduction: " << (1.0 - (double)total_edges / old_adjacency.size() / 21.2026) * 100 << "% (approx)" << std::endl;
    std::cout << "Time: " << sec << "s" << std::endl;

    // Write output graph (copy original, replace adjacency)
    std::ifstream orig(argv[1], std::ios::binary);
    orig.seekg(0, std::ios::end);
    size_t file_size = orig.tellg();
    orig.seekg(0);
    std::vector<char> buf(file_size);
    orig.read(buf.data(), file_size);
    orig.close();

    size_t adj_offset = 40 + (size_t)n * 4 + (size_t)n * hdr.data_size + (size_t)n * 8;

    std::ofstream out(argv[3], std::ios::binary);
    out.write(buf.data(), adj_offset);

    for (int i = 0; i < n; i++) {
        uint16_t cnt = new_adjacency[i].size();
        out.write(reinterpret_cast<const char*>(&cnt), 2);
        if (cnt > 0) {
            out.write(reinterpret_cast<const char*>(new_adjacency[i].data()), cnt * 4);
        }
    }
    out.close();

    size_t out_size = adj_offset;
    for (int i = 0; i < n; i++) {
        out_size += 2 + new_adjacency[i].size() * 4;
    }
    std::cout << "Output: " << argv[3] << " (" << out_size / 1e6 << " MB)" << std::endl;
    std::cout << "Original: " << file_size / 1e6 << " MB" << std::endl;

    return 0;
}
