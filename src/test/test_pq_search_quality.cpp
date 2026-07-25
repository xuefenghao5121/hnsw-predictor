// test_pq_search_quality.cpp - Check PQ search result quality
#include "common.h"
#include "disk_hnsw.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include <iostream>
#include <fstream>
#include <set>

int main() {
    std::string graph_path = "output/deep10m_graph.bin";
    std::string bfs_path = "output/deep10m_bfs.bin";
    std::string blocks_path = "output/deep10m_pq_blocks_4k.bin";  // PQ blocks (no vectors)
    std::string route_path = "output/deep10m_pq_route_4k.bin";
    std::string pq_path = "output/deep10m_pq_codes.bin";
    std::string query_path = "data/deep10m_test.fvecs";
    std::string gt_path = "data/deep10m_gt.bin";

    int dim;
    size_t num_query;
    auto query_data = read_fvecs(query_path, dim, num_query);

    // Load GT
    std::ifstream gt_in(gt_path, std::ios::binary);
    uint32_t n_gt, k_gt;
    gt_in.read(reinterpret_cast<char*>(&n_gt), sizeof(uint32_t));
    gt_in.read(reinterpret_cast<char*>(&k_gt), sizeof(uint32_t));

    // Load full graph for true vectors
    std::cout << "Loading full graph..." << std::endl;
    GraphStructure g = load_graph_structure(graph_path);

    // Load BFS mapping
    std::ifstream bfs_in(bfs_path, std::ios::binary);
    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    std::vector<uint32_t> old_to_new(g.num_nodes), new_to_old(g.num_nodes);
    bfs_in.read(reinterpret_cast<char*>(old_to_new.data()), g.num_nodes * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(new_to_old.data()), g.num_nodes * sizeof(uint32_t));
    bfs_in.close();

    // Setup DiskHNSW with PQ - with O_DIRECT and graph prefetch like benchmark
    IOConfig io_config;
    io_config.use_odirect = true;
    io_config.drop_page_cache = true;
    auto layout = std::make_unique<BfsLayoutProvider>(route_path);
    auto policy = std::make_unique<LRUPolicy>();
    auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy), 65536, dim, io_config);
    DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
    hnsw.setEf(50);
    hnsw.enableGraphPrefetch(true);
    hnsw.loadPQCodes(pq_path);

    // Search first 5 queries with PQ
    for (int q = 0; q < 5; q++) {
        float* query = &query_data[q * dim];

        // Get GT for this query
        std::vector<uint64_t> gt(k_gt);
        gt_in.seekg(sizeof(uint32_t) * 2 + q * k_gt * sizeof(uint64_t));
        gt_in.read(reinterpret_cast<char*>(gt.data()), k_gt * sizeof(uint64_t));

        // PQ search
        auto results = hnsw.searchKnn(query, 10);

        std::cout << "\nQuery " << q << ":" << std::endl;
        std::cout << "  PQ Search Results:" << std::endl;
        std::set<uint64_t> gt_set(gt.begin(), gt.begin() + 10);
        int recall = 0;
        for (const auto& [pq_dist, label] : results) {
            uint32_t old_id = (uint32_t)label;
            // Compute true L2 distance
            const float* vec = &g.vectors[old_id * g.dim];
            float true_dist = 0.0f;
            for (int j = 0; j < dim; j++) {
                float d = query[j] - vec[j];
                true_dist += d * d;
            }
            bool in_gt = gt_set.count(label) > 0;
            if (in_gt) recall++;
            std::cout << "    label=" << label << " pq_dist=" << pq_dist
                      << " true_dist=" << true_dist
                      << (in_gt ? " ✓" : " ✗") << std::endl;
        }
        std::cout << "  Recall@10: " << recall << "/10" << std::endl;

        // Print GT top-10 true distances
        std::cout << "  GT top-10:" << std::endl;
        for (int i = 0; i < 10; i++) {
            uint32_t old_id = (uint32_t)gt[i];
            const float* vec = &g.vectors[old_id * g.dim];
            float true_dist = 0.0f;
            for (int j = 0; j < dim; j++) {
                float d = query[j] - vec[j];
                true_dist += d * d;
            }
            // Also compute PQ distance
            uint32_t new_id = old_to_new[old_id];
            float pq_dist = hnsw.pqDistance(query, new_id);
            std::cout << "    label=" << gt[i] << " true_dist=" << true_dist
                      << " pq_dist=" << pq_dist << std::endl;
        }
    }

    return 0;
}
