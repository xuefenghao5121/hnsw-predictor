// bfs_reorder.cpp - Task 1.2: BFS 全局重排
// 从 entry_point 开始 BFS 遍历 Level 0 图，生成旧ID到新ID的映射
//
// 用法: ./bfs_reorder <graph_structure.bin> <bfs_order.bin>

#include "common.h"
#include <queue>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <graph_structure.bin> <bfs_order.bin>" << std::endl;
        return 1;
    }
    
    std::string graph_path = argv[1];
    std::string output_path = argv[2];
    
    std::cout << "=== Task 1.2: BFS Global Reorder ===" << std::endl;
    
    // 加载图结构
    GraphStructure g = load_graph_structure(graph_path);
    uint32_t N = g.num_nodes;
    
    std::cout << "Running BFS from entry point " << g.entry_point << "..." << std::endl;
    
    // BFS
    std::vector<uint32_t> bfs_order;  // bfs_order[new_id] = old_id
    bfs_order.reserve(N);
    std::vector<bool> visited(N, false);
    std::queue<uint32_t> q;
    
    // 从 entry point 开始
    q.push(g.entry_point);
    visited[g.entry_point] = true;
    
    while (!q.empty()) {
        uint32_t node = q.front();
        q.pop();
        bfs_order.push_back(node);
        
        // 遍历 level 0 邻居
        for (uint32_t neighbor : g.adjacency0[node]) {
            if (neighbor < N && !visited[neighbor]) {
                visited[neighbor] = true;
                q.push(neighbor);
            }
        }
    }
    
    // 处理未被 BFS 访问到的孤立节点
    uint32_t isolated = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (!visited[i]) {
            isolated++;
            bfs_order.push_back(i);
        }
    }
    
    std::cout << "  BFS visited: " << (N - isolated) << "/" << N << " nodes" << std::endl;
    if (isolated > 0) {
        std::cout << "  Isolated nodes (appended at end): " << isolated << std::endl;
    }
    
    // 构建映射: old_id -> new_id
    std::vector<uint32_t> old_to_new(N);
    for (uint32_t new_id = 0; new_id < N; new_id++) {
        old_to_new[bfs_order[new_id]] = new_id;
    }
    
    // 统计 BFS 重排效果: 计算邻居在新序列中的平均距离
    double total_neighbor_distance = 0;
    uint64_t neighbor_count = 0;
    for (uint32_t new_id = 0; new_id < N; new_id++) {
        uint32_t old_id = bfs_order[new_id];
        for (uint32_t neighbor_old : g.adjacency0[old_id]) {
            uint32_t neighbor_new = old_to_new[neighbor_old];
            int64_t dist = (int64_t)neighbor_new - (int64_t)new_id;
            total_neighbor_distance += std::abs(dist);
            neighbor_count++;
        }
    }
    double avg_dist = neighbor_count > 0 ? total_neighbor_distance / neighbor_count : 0;
    
    // 随机排列的期望平均距离 = N/3
    double random_expected = N / 3.0;
    double improvement = random_expected > 0 ? (1.0 - avg_dist / random_expected) * 100 : 0;
    
    std::cout << "\n=== BFS Reorder Statistics ===" << std::endl;
    std::cout << "  Total nodes: " << N << std::endl;
    std::cout << "  Average neighbor distance (BFS): " << avg_dist << std::endl;
    std::cout << "  Expected average distance (random): " << random_expected << std::endl;
    std::cout << "  Distance improvement: " << improvement << "%" << std::endl;
    std::cout << "  Total edges (level 0): " << neighbor_count << std::endl;
    
    // 保存 BFS order
    std::cout << "\nSaving BFS order to " << output_path << "..." << std::endl;
    std::ofstream out(output_path, std::ios::binary);
    
    BfsHeader bhdr;
    bhdr.magic = MAGIC_BFS;
    bhdr.num_nodes = N;
    bhdr.entry_point = g.entry_point;
    bhdr.reserved = 0;
    out.write(reinterpret_cast<const char*>(&bhdr), sizeof(BfsHeader));
    
    // 写入映射数组: old_to_new[i] = new_id for old node i
    out.write(reinterpret_cast<const char*>(old_to_new.data()), N * sizeof(uint32_t));
    
    // 同时写入 bfs_order: bfs_order[new_id] = old_id
    out.write(reinterpret_cast<const char*>(bfs_order.data()), N * sizeof(uint32_t));
    
    out.close();
    
    std::cout << "  File size: " << (sizeof(BfsHeader) + 2 * N * sizeof(uint32_t)) << " bytes" << std::endl;
    std::cout << "Task 1.2 complete." << std::endl;
    return 0;
}
