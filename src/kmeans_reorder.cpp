// kmeans_reorder.cpp - d-HNSW 风格分区重排
// 按 K-means 分区聚集节点顺序，同分区节点在磁盘上连续存储
// 图结构完全保留（跨分区边不丢失），只改变节点物理布局
// 输出格式与 bfs_order.bin 完全一致，可复用 write_blocks/gen_route/disk_hnsw
//
// 用法: ./kmeans_reorder <graph_structure.bin> <partition_mapping.bin> <kmeans_order.bin>
//   partition_mapping.bin: uint32[num_nodes], partition_mapping[old_id] = partition_id
//   分区内节点顺序: 保持 old_id 升序 (也可改为分区内 BFS)

#include "common.h"
#include <queue>
#include <algorithm>
#include <numeric>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph_structure.bin> <partition_mapping.bin> <kmeans_order.bin> [--bfs-within]"
                  << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string mapping_path = argv[2];
    std::string output_path = argv[3];
    bool bfs_within = (argc > 4 && std::string(argv[4]) == "--bfs-within");

    std::cout << "=== d-HNSW Partition Reorder ===" << std::endl;

    // 加载图结构（需要邻接表做分区内 BFS，用完整加载）
    GraphStructure g = load_graph_structure(graph_path);
    uint32_t N = g.num_nodes;

    // 加载分区映射
    std::cout << "Loading partition mapping from " << mapping_path << "..." << std::endl;
    std::ifstream map_in(mapping_path, std::ios::binary);
    if (!map_in.is_open()) {
        std::cerr << "Cannot open mapping file" << std::endl;
        return 1;
    }
    std::vector<uint32_t> partition_of(N);
    map_in.read(reinterpret_cast<char*>(partition_of.data()), N * sizeof(uint32_t));
    map_in.close();

    // 统计分区数
    uint32_t num_partitions = 0;
    for (uint32_t i = 0; i < N; i++) {
        num_partitions = std::max(num_partitions, partition_of[i] + 1);
    }
    std::cout << "  Num partitions: " << num_partitions << std::endl;

    // 按分区收集节点
    std::vector<std::vector<uint32_t>> partition_nodes(num_partitions);
    for (uint32_t old_id = 0; old_id < N; old_id++) {
        partition_nodes[partition_of[old_id]].push_back(old_id);
    }

    // 生成新顺序: 分区0的节点, 分区1的节点, ...
    // new_to_old[new_id] = old_id
    std::vector<uint32_t> new_to_old;
    new_to_old.reserve(N);

    for (uint32_t pid = 0; pid < num_partitions; pid++) {
        auto& nodes = partition_nodes[pid];
        if (nodes.empty()) continue;

        if (bfs_within && nodes.size() > 1) {
            // 分区内 BFS 排序（改善分区内局部性）
            std::vector<bool> in_partition(N, false);
            for (uint32_t n : nodes) in_partition[n] = true;
            std::vector<bool> visited(N, false);
            std::vector<uint32_t> ordered;
            ordered.reserve(nodes.size());

            // 从分区内第一个节点开始 BFS
            std::queue<uint32_t> q;
            q.push(nodes[0]);
            visited[nodes[0]] = true;
            while (!q.empty()) {
                uint32_t node = q.front(); q.pop();
                ordered.push_back(node);
                for (uint32_t nb : g.adjacency0[node]) {
                    if (nb < N && in_partition[nb] && !visited[nb]) {
                        visited[nb] = true;
                        q.push(nb);
                    }
                }
            }
            // 分区内未被 BFS 访问的节点
            for (uint32_t n : nodes) {
                if (!visited[n]) ordered.push_back(n);
            }
            for (uint32_t n : ordered) new_to_old.push_back(n);
        } else {
            // 保持 old_id 升序
            for (uint32_t n : nodes) new_to_old.push_back(n);
        }
    }

    if (new_to_old.size() != N) {
        std::cerr << "Error: node count mismatch " << new_to_old.size() << " vs " << N << std::endl;
        return 1;
    }

    // 构建 old_to_new
    std::vector<uint32_t> old_to_new(N);
    for (uint32_t new_id = 0; new_id < N; new_id++) {
        old_to_new[new_to_old[new_id]] = new_id;
    }

    // 统计重排效果: 邻居在新序列中的平均距离
    double total_dist = 0;
    uint64_t edge_count = 0;
    for (uint32_t new_id = 0; new_id < N; new_id++) {
        uint32_t old_id = new_to_old[new_id];
        for (uint32_t nb_old : g.adjacency0[old_id]) {
            uint32_t nb_new = old_to_new[nb_old];
            total_dist += std::abs((int64_t)nb_new - (int64_t)new_id);
            edge_count++;
        }
    }
    double avg_dist = edge_count > 0 ? total_dist / edge_count : 0;
    double random_expected = N / 3.0;
    double improvement = (1.0 - avg_dist / random_expected) * 100;

    // 跨分区边统计
    uint64_t cross_edges = 0;
    for (uint32_t old_id = 0; old_id < N; old_id++) {
        uint32_t pid = partition_of[old_id];
        for (uint32_t nb : g.adjacency0[old_id]) {
            if (nb < N && partition_of[nb] != pid) cross_edges++;
        }
    }

    std::cout << "\n=== Reorder Statistics ===" << std::endl;
    std::cout << "  Total nodes: " << N << std::endl;
    std::cout << "  Within-partition BFS: " << (bfs_within ? "YES" : "NO") << std::endl;
    std::cout << "  Avg neighbor distance: " << avg_dist << std::endl;
    std::cout << "  Random expected: " << random_expected << std::endl;
    std::cout << "  Distance improvement: " << improvement << "%" << std::endl;
    std::cout << "  Total edges (L0): " << edge_count << std::endl;
    std::cout << "  Cross-partition edges: " << cross_edges
              << " (" << (100.0 * cross_edges / edge_count) << "%)" << std::endl;

    // 保存 (格式与 bfs_order.bin 一致)
    std::cout << "\nSaving reorder to " << output_path << "..." << std::endl;
    std::ofstream out(output_path, std::ios::binary);
    BfsHeader bhdr;
    bhdr.magic = MAGIC_BFS;
    bhdr.num_nodes = N;
    bhdr.entry_point = g.entry_point;
    bhdr.reserved = 0;
    out.write(reinterpret_cast<const char*>(&bhdr), sizeof(BfsHeader));
    out.write(reinterpret_cast<const char*>(old_to_new.data()), N * sizeof(uint32_t));
    out.write(reinterpret_cast<const char*>(new_to_old.data()), N * sizeof(uint32_t));
    out.close();

    std::cout << "  File size: " << (sizeof(BfsHeader) + 2 * (size_t)N * sizeof(uint32_t)) << " bytes" << std::endl;
    std::cout << "Done." << std::endl;
    return 0;
}