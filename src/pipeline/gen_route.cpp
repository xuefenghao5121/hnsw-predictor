// gen_route.cpp - Task 1.4: 生成路由表
// 读取 blocks.bin，为每个节点生成 NodeID -> BlockID 的映射
//
// 用法: ./gen_route <blocks.bin> <route_table.bin>

#include "common.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <blocks.bin> <route_table.bin>" << std::endl;
        return 1;
    }
    
    std::string blocks_path = argv[1];
    std::string output_path = argv[2];
    
    std::cout << "=== Task 1.4: Generate Route Table ===" << std::endl;
    
    // 读取 blocks.bin
    std::cout << "Reading blocks from " << blocks_path << "..." << std::endl;
    std::ifstream in(blocks_path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "Error: Cannot open " << blocks_path << std::endl;
        return 1;
    }
    
    BlocksFileHeader fhdr;
    in.read(reinterpret_cast<char*>(&fhdr), sizeof(BlocksFileHeader));
    if (fhdr.magic != MAGIC_BLOCKS) {
        std::cerr << "Error: Invalid blocks file" << std::endl;
        return 1;
    }
    
    uint32_t block_size = fhdr.block_size;
    uint32_t num_blocks = fhdr.num_blocks;
    std::cout << "  Block size: " << block_size << " bytes, Num blocks: " << num_blocks << std::endl;
    
    // 跳过头部 padding 到 BLOCKS_FILE_HEADER_SIZE
    in.seekg(BLOCKS_FILE_HEADER_SIZE, std::ios::beg);
    
    // 找出最大节点 ID 以确定路由表大小
    uint32_t max_node_id = 0;
    
    // 先读一遍所有 Block 收集节点信息
    std::vector<char> block_buf(block_size);
    uint32_t total_nodes = 0;
    
    // 先扫描确定 max_node_id 和 total_nodes
    for (uint32_t b = 0; b < num_blocks; b++) {
        in.read(block_buf.data(), block_size);
        
        BlockHeader bh;
        memcpy(&bh, block_buf.data(), sizeof(BlockHeader));
        
        for (uint32_t i = 0; i < bh.node_count; i++) {
            uint32_t node_id;
            memcpy(&node_id, block_buf.data() + sizeof(BlockHeader) + i * sizeof(uint32_t), sizeof(uint32_t));
            if (node_id > max_node_id) max_node_id = node_id;
        }
        total_nodes += bh.node_count;
    }
    
    std::cout << "  Total nodes in blocks: " << total_nodes << std::endl;
    std::cout << "  Max node ID: " << max_node_id << std::endl;
    
    uint32_t num_entries = max_node_id + 1;
    
    // 回到文件开头重新读取，构建路由表
    in.clear();
    in.seekg(BLOCKS_FILE_HEADER_SIZE, std::ios::beg);
    
    std::vector<uint32_t> route_table(num_entries, 0xFFFFFFFF);  // 0xFFFFFFFF = not found
    
    for (uint32_t b = 0; b < num_blocks; b++) {
        in.read(block_buf.data(), block_size);
        
        BlockHeader bh;
        memcpy(&bh, block_buf.data(), sizeof(BlockHeader));
        
        for (uint32_t i = 0; i < bh.node_count; i++) {
            uint32_t node_id;
            memcpy(&node_id, block_buf.data() + sizeof(BlockHeader) + i * sizeof(uint32_t), sizeof(uint32_t));
            route_table[node_id] = b;
        }
    }
    in.close();
    
    // 验证: 所有节点都有路由
    uint32_t unmapped = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        if (route_table[i] == 0xFFFFFFFF) {
            unmapped++;
        }
    }
    
    if (unmapped > 0) {
        std::cerr << "  WARNING: " << unmapped << " nodes have no block mapping!" << std::endl;
    } else {
        std::cout << "  All " << num_entries << " nodes mapped to blocks." << std::endl;
    }
    
    // 统计每个 Block 的节点数
    std::vector<uint32_t> block_node_count(num_blocks, 0);
    for (uint32_t i = 0; i < num_entries; i++) {
        if (route_table[i] != 0xFFFFFFFF) {
            block_node_count[route_table[i]]++;
        }
    }
    
    uint32_t min_count = UINT32_MAX, max_count = 0;
    for (uint32_t b = 0; b < num_blocks; b++) {
        min_count = std::min(min_count, block_node_count[b]);
        max_count = std::max(max_count, block_node_count[b]);
    }
    
    std::cout << "\n=== Route Table Statistics ===" << std::endl;
    std::cout << "  Entries: " << num_entries << std::endl;
    std::cout << "  Blocks: " << num_blocks << std::endl;
    std::cout << "  Nodes per block: min=" << min_count << ", max=" << max_count << std::endl;
    
    // 保存路由表
    std::cout << "\nSaving route table to " << output_path << "..." << std::endl;
    std::ofstream out(output_path, std::ios::binary);
    
    RouteHeader rhdr;
    rhdr.magic = MAGIC_ROUTE;
    rhdr.num_entries = num_entries;
    rhdr.block_size = block_size;
    rhdr.reserved = 0;
    out.write(reinterpret_cast<const char*>(&rhdr), sizeof(RouteHeader));
    
    out.write(reinterpret_cast<const char*>(route_table.data()), num_entries * sizeof(uint32_t));
    out.close();
    
    size_t file_size = sizeof(RouteHeader) + (size_t)num_entries * sizeof(uint32_t);
    std::cout << "  File size: " << file_size << " bytes (" << (file_size / 1024.0) << " KB)" << std::endl;
    std::cout << "Task 1.4 complete." << std::endl;
    return 0;
}
