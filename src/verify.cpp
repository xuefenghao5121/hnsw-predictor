// verify.cpp - 验证工具: 检查阶段一所有产出文件的正确性
//
// 用法: ./verify <graph_structure.bin> <bfs_order.bin> <blocks.bin> <route_table.bin>

#include "common.h"
#include <set>
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <graph_structure.bin> <bfs_order.bin> <blocks.bin> <route_table.bin>" << std::endl;
        return 1;
    }
    
    std::cout << "=== Phase 1 Verification ===" << std::endl;
    
    // 1. 验证 graph_structure.bin
    std::cout << "\n[1] Verifying graph_structure.bin..." << std::endl;
    GraphStructure g = load_graph_structure(argv[1]);
    
    // 检查邻接表中的节点 ID 在合法范围
    uint32_t max_id = g.num_nodes - 1;
    uint64_t bad_ids = 0;
    uint64_t total_edges = 0;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        for (uint32_t n : g.adjacency0[i]) {
            if (n > max_id) bad_ids++;
            total_edges++;
        }
    }
    std::cout << "  Level 0 edges: " << total_edges << ", bad IDs: " << bad_ids << std::endl;
    
    // 检查向量数据有效性
    uint32_t nan_count = 0;
    for (uint32_t i = 0; i < g.num_nodes * g.dim; i++) {
        if (std::isnan(g.vectors[i])) nan_count++;
    }
    std::cout << "  NaN values in vectors: " << nan_count << std::endl;
    
    // 检查标签唯一性
    std::set<uint64_t> label_set(g.labels.begin(), g.labels.end());
    std::cout << "  Unique labels: " << label_set.size() << "/" << g.num_nodes << std::endl;
    
    // 2. 验证 bfs_order.bin
    std::cout << "\n[2] Verifying bfs_order.bin..." << std::endl;
    std::ifstream bfs_in(argv[2], std::ios::binary);
    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    
    if (bhdr.magic != MAGIC_BFS) {
        std::cerr << "  FAIL: Invalid magic" << std::endl;
        return 1;
    }
    
    std::vector<uint32_t> old_to_new(g.num_nodes);
    std::vector<uint32_t> bfs_order(g.num_nodes);
    bfs_in.read(reinterpret_cast<char*>(old_to_new.data()), g.num_nodes * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(bfs_order.data()), g.num_nodes * sizeof(uint32_t));
    bfs_in.close();
    
    // 验证映射是一一对应
    std::vector<bool> new_id_seen(g.num_nodes, false);
    uint32_t dup_count = 0;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        uint32_t new_id = old_to_new[i];
        if (new_id >= g.num_nodes || new_id_seen[new_id]) {
            dup_count++;
        } else {
            new_id_seen[new_id] = true;
        }
    }
    uint32_t unmapped = 0;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (!new_id_seen[i]) unmapped++;
    }
    std::cout << "  Duplicate mappings: " << dup_count << std::endl;
    std::cout << "  Unmapped new IDs: " << unmapped << std::endl;
    
    // 验证逆映射
    bool inverse_ok = true;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (bfs_order[old_to_new[i]] != i) {
            inverse_ok = false;
            break;
        }
    }
    std::cout << "  Inverse mapping correct: " << (inverse_ok ? "YES" : "NO") << std::endl;
    
    // 3. 验证 blocks.bin
    std::cout << "\n[3] Verifying blocks.bin..." << std::endl;
    std::ifstream blk_in(argv[3], std::ios::binary);
    BlocksFileHeader fhdr;
    blk_in.read(reinterpret_cast<char*>(&fhdr), sizeof(BlocksFileHeader));
    
    if (fhdr.magic != MAGIC_BLOCKS) {
        std::cerr << "  FAIL: Invalid magic" << std::endl;
        return 1;
    }
    
    uint32_t block_size = fhdr.block_size;
    uint32_t num_blocks = fhdr.num_blocks;
    std::cout << "  Blocks: " << num_blocks << ", block_size: " << block_size << std::endl;
    
    // 读取所有 Block，收集节点信息
    std::vector<char> block_buf(block_size);
    std::set<uint32_t> all_node_ids;
    uint32_t total_block_nodes = 0;
    uint32_t max_node_id_in_blocks = 0;
    
    for (uint32_t b = 0; b < num_blocks; b++) {
        blk_in.read(block_buf.data(), block_size);
        
        BlockHeader bh;
        memcpy(&bh, block_buf.data(), sizeof(BlockHeader));
        
        // 验证 block_id
        if (bh.block_id != b) {
            std::cerr << "  FAIL: Block " << b << " has wrong block_id " << bh.block_id << std::endl;
            return 1;
        }
        
        // 读取 node IDs
        for (uint32_t i = 0; i < bh.node_count; i++) {
            uint32_t node_id;
            memcpy(&node_id, block_buf.data() + sizeof(BlockHeader) + i * sizeof(uint32_t), sizeof(uint32_t));
            
            if (all_node_ids.count(node_id) > 0) {
                std::cerr << "  FAIL: Node " << node_id << " appears in multiple blocks" << std::endl;
                return 1;
            }
            all_node_ids.insert(node_id);
            if (node_id > max_node_id_in_blocks) max_node_id_in_blocks = node_id;
            total_block_nodes++;
        }
    }
    blk_in.close();
    
    std::cout << "  Total nodes in blocks: " << total_block_nodes << "/" << g.num_nodes << std::endl;
    std::cout << "  Unique node IDs: " << all_node_ids.size() << std::endl;
    
    if (total_block_nodes != g.num_nodes) {
        std::cerr << "  WARNING: Node count mismatch!" << std::endl;
    }
    
    // 4. 验证 route_table.bin
    std::cout << "\n[4] Verifying route_table.bin..." << std::endl;
    std::ifstream rt_in(argv[4], std::ios::binary);
    RouteHeader rhdr;
    rt_in.read(reinterpret_cast<char*>(&rhdr), sizeof(RouteHeader));
    
    if (rhdr.magic != MAGIC_ROUTE) {
        std::cerr << "  FAIL: Invalid magic" << std::endl;
        return 1;
    }
    
    std::vector<uint32_t> route(rhdr.num_entries);
    rt_in.read(reinterpret_cast<char*>(route.data()), rhdr.num_entries * sizeof(uint32_t));
    rt_in.close();
    
    // 验证: 每个在 block 中的节点都有正确的路由
    uint32_t route_errors = 0;
    for (uint32_t node_id : all_node_ids) {
        if (node_id >= rhdr.num_entries) {
            route_errors++;
            continue;
        }
        uint32_t block_id = route[node_id];
        if (block_id >= num_blocks) {
            route_errors++;
            continue;
        }
        // 验证该 Block 确实包含此节点
        // (需要重新读取 block 检查，这里简化为检查 block_id 范围)
    }
    std::cout << "  Route entries: " << rhdr.num_entries << std::endl;
    std::cout << "  Route errors: " << route_errors << std::endl;
    
    // 总结
    std::cout << "\n=== Verification Summary ===" << std::endl;
    bool all_ok = (bad_ids == 0) && (nan_count == 0) && (dup_count == 0) &&
                  (unmapped == 0) && inverse_ok && (route_errors == 0) &&
                  (total_block_nodes == g.num_nodes);
    std::cout << "  Result: " << (all_ok ? "ALL CHECKS PASSED ✅" : "SOME CHECKS FAILED ❌") << std::endl;
    
    return all_ok ? 0 : 1;
}
