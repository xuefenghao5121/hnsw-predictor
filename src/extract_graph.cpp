// extract_graph.cpp - Task 1.1: 提取图结构
// 从 hnswlib 构建的 .bin 索引文件中提取邻接表、层级、向量数据
// 
// 用法: ./extract_graph <index.bin> <graph_structure.bin> <dim>

#include "hnswlib/hnswlib.h"
#include "common.h"
#include <chrono>

using hnswlib::tableint;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <index.bin> <graph_structure.bin> <dim>" << std::endl;
        return 1;
    }
    
    std::string index_path = argv[1];
    std::string output_path = argv[2];
    int dim = std::stoi(argv[3]);
    
    std::cout << "=== Task 1.1: Extract Graph Structure ===" << std::endl;
    std::cout << "Loading index from " << index_path << "..." << std::endl;
    
    // 加载 hnswlib 索引
    hnswlib::L2Space space(dim);
    auto* alg_hnsw = new hnswlib::HierarchicalNSW<float>(&space, index_path);
    
    size_t num_nodes = alg_hnsw->getCurrentElementCount();
    std::cout << "  Loaded: " << num_nodes << " nodes" << std::endl;
    
    // 准备输出
    GraphStructure g;
    g.num_nodes = num_nodes;
    g.dim = dim;
    g.maxM = alg_hnsw->maxM_;
    g.maxM0 = alg_hnsw->maxM0_;
    g.entry_point = alg_hnsw->enterpoint_node_;
    g.max_level = alg_hnsw->maxlevel_;
    g.data_size = dim * sizeof(float);
    
    g.levels.resize(num_nodes);
    g.vectors.resize((size_t)num_nodes * dim);
    g.labels.resize(num_nodes);
    g.adjacency0.resize(num_nodes);
    g.upper_adjacency.resize(num_nodes);
    
    // 提取数据
    std::cout << "Extracting node data and adjacency lists..." << std::endl;
    
    size_t total_edges_l0 = 0;
    size_t total_edges_upper = 0;
    size_t nodes_with_upper = 0;
    
    for (size_t i = 0; i < num_nodes; i++) {
        // 层级
        g.levels[i] = alg_hnsw->element_levels_[i];
        
        // 向量数据
        float* vec_data = reinterpret_cast<float*>(alg_hnsw->getDataByInternalId(i));
        memcpy(&g.vectors[i * dim], vec_data, dim * sizeof(float));
        
        // 标签
        g.labels[i] = alg_hnsw->getExternalLabel(i);
        
        // Level 0 邻接表
        auto* ll0 = alg_hnsw->get_linklist0(i);
        size_t cnt0 = alg_hnsw->getListCount(ll0);
        tableint* neighbors0 = reinterpret_cast<tableint*>(ll0 + 1);
        g.adjacency0[i].assign(neighbors0, neighbors0 + cnt0);
        total_edges_l0 += cnt0;
        
        // 上层邻接表
        if (g.levels[i] > 0) {
            nodes_with_upper++;
            g.upper_adjacency[i].resize(g.levels[i] + 1); // index 0 unused
            for (int lv = 1; lv <= g.levels[i]; lv++) {
                auto* ll = alg_hnsw->get_linklist(i, lv);
                size_t cnt = alg_hnsw->getListCount(ll);
                tableint* neighbors = reinterpret_cast<tableint*>(ll + 1);
                g.upper_adjacency[i][lv].assign(neighbors, neighbors + cnt);
                total_edges_upper += cnt;
            }
        }
        
        if ((i + 1) % 100000 == 0) {
            std::cout << "  Processed " << (i + 1) << "/" << num_nodes << "..." << std::endl;
        }
    }
    
    // 统计信息
    std::cout << "\n=== Extraction Statistics ===" << std::endl;
    std::cout << "  Total nodes: " << num_nodes << std::endl;
    std::cout << "  Entry point: " << g.entry_point << std::endl;
    std::cout << "  Max level: " << g.max_level << std::endl;
    std::cout << "  Nodes with upper levels: " << nodes_with_upper 
              << " (" << (100.0 * nodes_with_upper / num_nodes) << "%)" << std::endl;
    std::cout << "  Level 0 edges: " << total_edges_l0 
              << " (avg: " << (double)total_edges_l0 / num_nodes << " per node)" << std::endl;
    std::cout << "  Upper level edges: " << total_edges_upper << std::endl;
    
    // 层级分布
    std::vector<size_t> level_dist(g.max_level + 1, 0);
    for (size_t i = 0; i < num_nodes; i++) {
        if (g.levels[i] >= 0 && g.levels[i] <= g.max_level) {
            level_dist[g.levels[i]]++;
        }
    }
    std::cout << "  Level distribution:" << std::endl;
    for (size_t lv = 0; lv < level_dist.size(); lv++) {
        std::cout << "    Level " << lv << ": " << level_dist[lv] << " nodes" << std::endl;
    }
    
    // 保存
    save_graph_structure(output_path, g);
    
    delete alg_hnsw;
    std::cout << "Task 1.1 complete." << std::endl;
    return 0;
}
