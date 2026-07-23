// write_blocks.cpp - Task 1.3: 切分 Block 并落盘
// 按 BFS 顺序将节点切分为固定大小的 Block，写入磁盘文件
// 邻居 ID 映射为 BFS 重排后的新 ID
//
// 用法: ./write_blocks <graph_structure.bin> <bfs_order.bin> <blocks.bin> [block_size]
// 默认 block_size = 262144 (256KB)

#include "common.h"
#include <cstring>

// 计算 Block 中单个节点占用的空间（字节）
size_t node_size_in_block(uint32_t dim, uint16_t neighbor_count) {
    // node_id (4B) + vector (dim*4B) + neighbor_count (2B) + neighbors (neighbor_count * 4B)
    return sizeof(uint32_t) + dim * sizeof(float) + sizeof(uint16_t) + neighbor_count * sizeof(uint32_t);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <graph_structure.bin> <bfs_order.bin> <blocks.bin> [block_size]" << std::endl;
        return 1;
    }
    
    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string output_path = argv[3];
    uint32_t block_size = (argc > 4) ? std::stoul(argv[4]) : DEFAULT_BLOCK_SIZE;
    
    std::cout << "=== Task 1.3: Write Blocks ===" << std::endl;
    
    // 加载图结构
    GraphStructure g = load_graph_structure(graph_path);
    uint32_t N = g.num_nodes;
    uint32_t dim = g.dim;
    
    // 加载 BFS order
    std::cout << "Loading BFS order from " << bfs_path << "..." << std::endl;
    std::ifstream bfs_in(bfs_path, std::ios::binary);
    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    if (bhdr.magic != MAGIC_BFS) {
        std::cerr << "Error: Invalid BFS order file" << std::endl;
        return 1;
    }
    
    std::vector<uint32_t> old_to_new(N);
    std::vector<uint32_t> bfs_order(N);  // bfs_order[new_id] = old_id
    bfs_in.read(reinterpret_cast<char*>(old_to_new.data()), N * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(bfs_order.data()), N * sizeof(uint32_t));
    bfs_in.close();
    
    std::cout << "  Block size: " << block_size << " bytes (" << (block_size / 1024) << "KB)" << std::endl;
    
    // 计算每个节点在新排列下的邻居数（就是原来的邻居数，只是ID变了）
    // 准备每个节点的新邻居列表
    std::vector<std::vector<uint32_t>> new_adjacency0(N);
    for (uint32_t new_id = 0; new_id < N; new_id++) {
        uint32_t old_id = bfs_order[new_id];
        new_adjacency0[new_id].reserve(g.adjacency0[old_id].size());
        for (uint32_t old_neighbor : g.adjacency0[old_id]) {
            new_adjacency0[new_id].push_back(old_to_new[old_neighbor]);
        }
    }
    
    // 计算每个节点在 Block 中占用的空间，进行 Block 切分
    // Block 布局: [BlockHeader(24B)] [node_ids(4*cnt)] [vectors(dim*4*cnt)] [adj_lists(变长)]
    // 先计算每个节点的"开销": node_id(4B) + vector(dim*4B) + adj(2B + neighbors*4B)
    
    struct BlockPlan {
        uint32_t start_node;  // new_id 范围 [start, end)
        uint32_t end_node;
        uint32_t node_count;
        size_t   block_bytes;  // 实际字节
    };
    
    std::vector<BlockPlan> block_plans;
    
    uint32_t node_idx = 0;
    while (node_idx < N) {
        BlockPlan plan;
        plan.start_node = node_idx;
        
        size_t header_size = sizeof(BlockHeader);
        size_t used = header_size;
        uint32_t count = 0;
        
        while (node_idx + count < N) {
            uint32_t nid = node_idx + count;
            uint16_t ncnt = static_cast<uint16_t>(new_adjacency0[nid].size());
            size_t node_bytes = sizeof(uint32_t) + dim * sizeof(float) + sizeof(uint16_t) + ncnt * sizeof(uint32_t);
            
            // 检查加入这个节点后是否超限
            // 需要预留 node_ids 区域、vector 区域、adj 区域
            // 当前 used = header + 已有 node_ids + 已有 vectors + 已有 adj
            // 加入后: used += 4 (node_id) + dim*4 (vector) + 2 + ncnt*4 (adj)
            
            if (used + node_bytes > block_size && count > 0) {
                break;  // 当前 Block 已满
            }
            
            used += node_bytes;
            count++;
        }
        
        plan.end_node = node_idx + count;
        plan.node_count = count;
        plan.block_bytes = used;
        block_plans.push_back(plan);
        node_idx += count;
    }
    
    uint32_t num_blocks = block_plans.size();
    std::cout << "  Total blocks: " << num_blocks << std::endl;
    std::cout << "  Avg nodes per block: " << ((double)N / num_blocks) << std::endl;
    
    // 统计 Block 填充率
    double total_fill = 0;
    uint32_t min_nodes = UINT32_MAX, max_nodes = 0;
    for (const auto& bp : block_plans) {
        total_fill += (double)bp.block_bytes / block_size;
        min_nodes = std::min(min_nodes, bp.node_count);
        max_nodes = std::max(max_nodes, bp.node_count);
    }
    std::cout << "  Avg fill rate: " << (total_fill / num_blocks * 100) << "%" << std::endl;
    std::cout << "  Min nodes/block: " << min_nodes << ", Max nodes/block: " << max_nodes << std::endl;
    
    // 写入 blocks.bin
    std::cout << "\nWriting blocks to " << output_path << "..." << std::endl;
    std::ofstream out(output_path, std::ios::binary);
    
    // File Header
    BlocksFileHeader fhdr;
    fhdr.magic = MAGIC_BLOCKS;
    fhdr.version = FORMAT_VERSION;
    fhdr.block_size = block_size;
    fhdr.num_blocks = num_blocks;
    out.write(reinterpret_cast<const char*>(&fhdr), sizeof(BlocksFileHeader));
    
    // Padding 到 BLOCKS_FILE_HEADER_SIZE (4096 字节，O_DIRECT 对齐)
    size_t padding = BLOCKS_FILE_HEADER_SIZE - sizeof(BlocksFileHeader);
    std::vector<char> pad_buf(padding, 0);
    out.write(pad_buf.data(), padding);
    
    // 为每个 Block 分配固定大小的 buffer
    std::vector<char> block_buf(block_size, 0);
    
    for (uint32_t b = 0; b < num_blocks; b++) {
        const auto& plan = block_plans[b];
        memset(block_buf.data(), 0, block_size);
        
        uint32_t cnt = plan.node_count;
        
        // 计算各区域偏移
        uint32_t node_ids_offset = sizeof(BlockHeader);
        uint32_t vectors_offset = node_ids_offset + cnt * sizeof(uint32_t);
        uint32_t adj_offset = vectors_offset + cnt * dim * sizeof(float);
        
        // 写 Block Header
        BlockHeader bh;
        bh.block_id = b;
        bh.node_count = cnt;
        bh.data_offset = vectors_offset;
        bh.adj_offset = adj_offset;
        bh.reserved = 0;
        memcpy(block_buf.data(), &bh, sizeof(BlockHeader));
        
        // 写 Node IDs (new_id)
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t new_id = plan.start_node + i;
            memcpy(block_buf.data() + node_ids_offset + i * sizeof(uint32_t), &new_id, sizeof(uint32_t));
        }
        
        // 写 Vector Data
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t new_id = plan.start_node + i;
            uint32_t old_id = bfs_order[new_id];
            memcpy(block_buf.data() + vectors_offset + i * dim * sizeof(float),
                   &g.vectors[old_id * dim], dim * sizeof(float));
        }
        
        // 写 Adjacency Lists (新 ID)
        size_t adj_pos = adj_offset;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t new_id = plan.start_node + i;
            uint16_t ncnt = static_cast<uint16_t>(new_adjacency0[new_id].size());
            memcpy(block_buf.data() + adj_pos, &ncnt, sizeof(uint16_t));
            adj_pos += sizeof(uint16_t);
            if (ncnt > 0) {
                memcpy(block_buf.data() + adj_pos, new_adjacency0[new_id].data(), ncnt * sizeof(uint32_t));
                adj_pos += ncnt * sizeof(uint32_t);
            }
        }
        
        // 写入整个 Block (固定大小)
        out.write(block_buf.data(), block_size);
        
        if ((b + 1) % 100 == 0 || b == num_blocks - 1) {
            std::cout << "  Written block " << (b + 1) << "/" << num_blocks << std::endl;
        }
    }
    
    out.close();
    
    // 输出 Block 元数据到文本文件
    std::string meta_path = output_path + ".meta";
    std::ofstream meta_out(meta_path);
    meta_out << "num_blocks=" << num_blocks << "\n";
    meta_out << "block_size=" << block_size << "\n";
    meta_out << "num_nodes=" << N << "\n";
    meta_out << "dim=" << dim << "\n";
    for (uint32_t b = 0; b < num_blocks; b++) {
        const auto& plan = block_plans[b];
        meta_out << "block " << b << " "
                 << plan.start_node << " " << plan.end_node << " " << plan.node_count << " " 
                 << plan.block_bytes << "\n";
    }
    meta_out.close();
    
    // 统计
    std::cout << "\n=== Block Writing Statistics ===" << std::endl;
    std::cout << "  Total blocks: " << num_blocks << std::endl;
    std::cout << "  Total file size: " << (BLOCKS_FILE_HEADER_SIZE + (size_t)num_blocks * block_size) << " bytes" 
              << " (" << (BLOCKS_FILE_HEADER_SIZE + (size_t)num_blocks * block_size) / (1024*1024) << " MB)" << std::endl;
    std::cout << "  Metadata saved to " << meta_path << std::endl;
    std::cout << "Task 1.3 complete." << std::endl;
    return 0;
}
