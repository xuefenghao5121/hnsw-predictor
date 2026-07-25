// test_block_compare.cpp - Compare neighbor lists between original and PQ blocks
#include "common.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include <iostream>

int main() {
    uint32_t dim = 96;

    // Load original blocks (4KB, with vectors)
    std::cout << "Loading original blocks..." << std::endl;
    IOConfig io_config;
    auto layout1 = std::make_unique<BfsLayoutProvider>("output/deep10m_route_4k.bin");
    auto policy1 = std::make_unique<LRUPolicy>();
    auto cache1 = std::make_unique<BlockCache>(
        "output/deep10m_blocks_4k.bin", std::move(layout1), std::move(policy1),
        65536, dim, io_config);

    // Load PQ blocks (4KB, no vectors)
    std::cout << "Loading PQ blocks..." << std::endl;
    auto layout2 = std::make_unique<BfsLayoutProvider>("output/deep10m_pq_route_4k.bin");
    auto policy2 = std::make_unique<LRUPolicy>();
    auto cache2 = std::make_unique<BlockCache>(
        "output/deep10m_pq_blocks_4k.bin", std::move(layout2), std::move(policy2),
        65536, dim, io_config);

    // Compare neighbor lists for a few nodes
    for (uint32_t new_id : {0u, 1u, 100u, 1000u, 5000u}) {
        uint32_t nc1 = 0, nc2 = 0;
        const uint32_t* n1 = cache1->getNodeNeighbors(new_id, nc1);
        const uint32_t* n2 = cache2->getNodeNeighbors(new_id, nc2);

        std::cout << "\nNode " << new_id << ":" << std::endl;
        std::cout << "  Original: " << nc1 << " neighbors" << std::endl;
        std::cout << "  PQ:       " << nc2 << " neighbors" << std::endl;

        if (nc1 != nc2) {
            std::cout << "  MISMATCH: different neighbor counts!" << std::endl;
            continue;
        }

        // Compare neighbor lists
        bool match = true;
        for (uint32_t i = 0; i < nc1; i++) {
            if (n1[i] != n2[i]) {
                std::cout << "  MISMATCH at index " << i << ": " << n1[i] << " vs " << n2[i] << std::endl;
                match = false;
                break;
            }
        }
        if (match) {
            std::cout << "  MATCH ✓" << std::endl;
        }
    }

    return 0;
}
