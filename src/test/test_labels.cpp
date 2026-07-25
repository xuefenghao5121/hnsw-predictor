// test_labels.cpp - Check if graph labels match GT labels
#include "common.h"
#include <iostream>
#include <fstream>

int main() {
    // Load graph
    GraphStructure g = load_graph_structure("output/deep10m_graph.bin");
    std::cout << "Graph: " << g.num_nodes << " nodes" << std::endl;

    // Check first 10 labels
    std::cout << "First 10 labels:" << std::endl;
    for (int i = 0; i < 10; i++) {
        std::cout << "  labels[" << i << "] = " << g.labels[i] << std::endl;
    }

    // Load GT
    std::ifstream in("data/deep10m_gt.bin", std::ios::binary);
    uint32_t n_queries, kk;
    in.read(reinterpret_cast<char*>(&n_queries), sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(&kk), sizeof(uint32_t));
    std::cout << "\nGT: " << n_queries << " queries, k=" << kk << std::endl;

    // Read first query's GT
    std::vector<uint64_t> gt(kk);
    in.read(reinterpret_cast<char*>(gt.data()), kk * sizeof(uint64_t));
    std::cout << "First query GT labels:" << std::endl;
    for (int i = 0; i < 10; i++) {
        std::cout << "  GT[0][" << i << "] = " << gt[i] << std::endl;
    }

    // Check if GT labels exist in graph labels
    std::cout << "\nChecking if GT labels match graph node IDs..." << std::endl;
    for (int i = 0; i < 5; i++) {
        uint64_t label = gt[i];
        if (label < g.num_nodes) {
            std::cout << "  GT label " << label << " -> graph labels[" << label << "] = " << g.labels[label] << std::endl;
        }
    }

    return 0;
}
