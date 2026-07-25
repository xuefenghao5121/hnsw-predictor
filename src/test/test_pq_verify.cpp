// test_pq_verify.cpp - Verify PQ ADC distance vs true L2 distance
#include "common.h"
#include "disk_hnsw.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include <iostream>
#include <fstream>
#include <cmath>

int main() {
    std::string graph_path = "output/deep10m_graph.bin";
    std::string bfs_path = "output/deep10m_bfs.bin";
    std::string blocks_path = "output/deep10m_blocks_4k.bin";
    std::string route_path = "output/deep10m_route_4k.bin";
    std::string pq_path = "output/deep10m_pq_codes.bin";
    std::string query_path = "data/deep10m_test.fvecs";

    int dim;
    size_t num_query;
    auto query_data = read_fvecs(query_path, dim, num_query);

    // Load graph structure to get original vectors
    std::cout << "Loading graph structure (full)..." << std::endl;
    GraphStructure g = load_graph_structure(graph_path);
    std::cout << "Graph: " << g.num_nodes << " nodes, dim=" << g.dim << std::endl;

    // Setup DiskHNSW with original blocks
    IOConfig io_config;
    auto layout = std::make_unique<BfsLayoutProvider>(route_path);
    auto policy = std::make_unique<LRUPolicy>();
    auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy), 65536, dim, io_config);
    DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
    hnsw.setEf(50);
    hnsw.loadPQCodes(pq_path);

    // Load BFS mapping
    std::ifstream bfs_in(bfs_path, std::ios::binary);
    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    std::vector<uint32_t> old_to_new(g.num_nodes);
    std::vector<uint32_t> new_to_old(g.num_nodes);
    bfs_in.read(reinterpret_cast<char*>(old_to_new.data()), g.num_nodes * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(new_to_old.data()), g.num_nodes * sizeof(uint32_t));
    bfs_in.close();

    float* query = &query_data[0];

    // Pick 10 random nodes, compute true L2 and PQ ADC distance
    std::cout << "\nComparing true L2 vs PQ ADC distance for query[0]:" << std::endl;
    std::cout << "  old_id  |  true_L2  |  PQ_ADC   |  ratio  |  rank_match" << std::endl;

    struct DistPair { float true_dist; float pq_dist; uint32_t old_id; };
    std::vector<DistPair> pairs;

    for (int i = 0; i < 1000; i++) {
        uint32_t old_id = i * 10000;
        if (old_id >= g.num_nodes) break;

        // True L2 distance
        const float* vec = &g.vectors[old_id * g.dim];
        float true_dist = 0.0f;
        for (int j = 0; j < dim; j++) {
            float d = query[j] - vec[j];
            true_dist += d * d;
        }

        // PQ ADC distance
        uint32_t new_id = old_to_new[old_id];
        float pq_dist = hnsw.pqDistance(query, new_id);

        pairs.push_back({true_dist, pq_dist, old_id});
    }

    // Sort by true distance
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        return a.true_dist < b.true_dist;
    });

    // Print top 20
    for (int i = 0; i < 20 && i < (int)pairs.size(); i++) {
        std::cout << "  " << pairs[i].old_id << " | " << pairs[i].true_dist
                  << " | " << pairs[i].pq_dist
                  << " | " << (pairs[i].pq_dist / pairs[i].true_dist)
                  << std::endl;
    }

    // Check ranking correlation
    auto pq_sorted = pairs;
    std::sort(pq_sorted.begin(), pq_sorted.end(), [](const auto& a, const auto& b) {
        return a.pq_dist < b.pq_dist;
    });

    // Check overlap of top-10
    std::set<uint32_t> true_top10, pq_top10;
    for (int i = 0; i < 10 && i < (int)pairs.size(); i++) {
        true_top10.insert(pairs[i].old_id);
        pq_top10.insert(pq_sorted[i].old_id);
    }
    int overlap = 0;
    for (auto id : true_top10) if (pq_top10.count(id)) overlap++;
    std::cout << "\nTop-10 overlap: " << overlap << "/10" << std::endl;

    return 0;
}
