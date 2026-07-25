// test_pq_distance.cpp - Quick test to verify PQ ADC distance correctness
#include "common.h"
#include "disk_hnsw.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include <iostream>
#include <fstream>

int main() {
    std::string graph_path = "output/deep10m_graph.bin";
    std::string bfs_path = "output/deep10m_bfs.bin";
    std::string blocks_path = "output/deep10m_blocks_4k.bin";
    std::string route_path = "output/deep10m_route_4k.bin";
    std::string pq_path = "output/deep10m_pq_codes.bin";
    std::string query_path = "data/deep10m_test.fvecs";

    // Load query
    int dim;
    size_t num_query;
    auto query_data = read_fvecs(query_path, dim, num_query);
    std::cout << "Query: " << num_query << " x " << dim << std::endl;

    // Setup DiskHNSW with original blocks (non-PQ)
    IOConfig io_config;
    auto layout = std::make_unique<BfsLayoutProvider>(route_path);
    auto policy = std::make_unique<LRUPolicy>();
    auto cache = std::make_unique<BlockCache>(blocks_path, std::move(layout), std::move(policy), 65536, dim, io_config);
    DiskHNSW hnsw(graph_path, bfs_path, std::move(cache));
    hnsw.setEf(50);

    // Load PQ codes
    hnsw.loadPQCodes(pq_path);

    // Compare PQ ADC distance vs true L2 distance for a few nodes
    // Use the first query
    float* query = &query_data[0];

    // Get a few nodes and compare distances
    // Node 0, 1, 100, 1000 in new_id space
    for (uint32_t new_id : {0u, 1u, 100u, 1000u, 5000u}) {
        // True L2 distance: need to get the vector
        uint32_t old_id = hnsw.newToOld(new_id);
        
        // PQ ADC distance
        float pq_dist = hnsw.pqDistance(query, new_id);
        
        std::cout << "Node " << new_id << " (old_id=" << old_id << "): PQ_dist=" << pq_dist << std::endl;
    }

    // Also test: search with PQ and check results
    std::cout << "\nSearching with PQ..." << std::endl;
    auto results = hnsw.searchKnn(query, 10);
    std::cout << "Results: " << results.size() << std::endl;
    for (const auto& [dist, label] : results) {
        std::cout << "  dist=" << dist << " label=" << label << std::endl;
    }

    // Also search without PQ for comparison
    // We need to create a new DiskHNSW without PQ
    auto layout2 = std::make_unique<BfsLayoutProvider>(route_path);
    auto policy2 = std::make_unique<LRUPolicy>();
    auto cache2 = std::make_unique<BlockCache>(blocks_path, std::move(layout2), std::move(policy2), 65536, dim, io_config);
    DiskHNSW hnsw_no_pq(graph_path, bfs_path, std::move(cache2));
    hnsw_no_pq.setEf(50);

    std::cout << "\nSearching without PQ (true L2)..." << std::endl;
    auto results_no_pq = hnsw_no_pq.searchKnn(query, 10);
    std::cout << "Results: " << results_no_pq.size() << std::endl;
    for (const auto& [dist, label] : results_no_pq) {
        std::cout << "  dist=" << dist << " label=" << label << std::endl;
    }

    return 0;
}
