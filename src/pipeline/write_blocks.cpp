// write_blocks.cpp - Task 1.3: 切分 Block 并落盘
// 按 BFS 顺序将节点切分为固定大小的 Block，写入磁盘文件
// 邻居 ID 映射为 BFS 重排后的新 ID
// 邻居列表使用 delta + varint 压缩编码
//
// 用法: ./write_blocks <graph_structure.bin> <bfs_order.bin> <blocks.bin> [block_size]
// 默认 block_size = 262144 (256KB)

#include "common.h"
#include <cstring>
#include <algorithm>

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <graph_structure.bin> <bfs_order.bin> <blocks.bin> [block_size]" << std::endl;
        return 1;
    }
    
    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string output_path = argv[3];
    uint32_t block_size = (argc > 4) ? std::stoul(argv[4]) : DEFAULT_BLOCK_SIZE;
    
    std::cout << "=== Task 1.3: Write Blocks (delta+varint compressed) ===" << std::endl;
    
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
    
    // ---- 预计算每个节点的压缩邻接表大小 ----
    // 压缩格式: 排序 -> delta -> varint
    std::vector<size_t> compressed_adj_sizes(N);
    std::vector<uint8_t> tmp_buf(N * 32 + 4096);  // 临时缓冲区
    for (uint32_t new_id = 0; new_id < N; new_id++) {
        auto& adj = new_adjacency0[new_id];
        if (adj.empty()) {
            compressed_adj_sizes[new_id] = 0;
        } else {
            std::vector<uint32_t> sorted_adj = adj;
            std::sort(sorted_adj.begin(), sorted_adj.end());
            compressed_adj_sizes[new_id] = delta_varint_encode(
                sorted_adj.data(), sorted_adj.size(), tmp_buf.data());
        }
    }
    
    // 统计压缩率
    size_t total_uncompressed_adj = 0;
    size_t total_compressed_adj = 0;
    for (uint32_t i = 0; i < N; i++) {
        total_uncompressed_adj += new_adjacency0[i].size() * sizeof(uint32_t);
        total_compressed_adj += compressed_adj_sizes[i];
    }
    std::cout << "  Neighbor data: " << total_uncompressed_adj << " -> " << total_compressed_adj
              << " bytes (" << (100.0 * total_compressed_adj / (total_uncompressed_adj + 1))
              << "% of original)" << std::endl;
    
    // 计算每个节点在 Block 中占用的空间，进行 Block 切分
    // 压缩格式 Block 布局: [BlockHeader(24B)] [node_ids(4*cnt)] [vectors(dim*4*cnt)] [adj_lists(变长, delta+varint)]
    
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
            uint16_t ncnt_unused = static_cast<uint16_t>(new_adjacency0[nid].size());
            (void)ncnt_unused;  // 压缩格式用 compressed_adj_sizes[nid] 代替
            // node_id(4B) + vector(dim*4B) + neighbor_count(2B) + compressed_adj
            size_t node_bytes = sizeof(uint32_t) + dim * sizeof(float) + sizeof(uint16_t) + compressed_adj_sizes[nid];
            
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
    fhdr.version = FORMAT_VERSION_COMPRESSED;  // 压缩格式版本
    fhdr.block_size = block_size;
    fhdr.num_blocks = num_blocks;
    out.write(reinterpret_cast<const char*>(&fhdr), sizeof(BlocksFileHeader));
    
    // Padding 到 BLOCKS_FILE_HEADER_SIZE (4096 字节，O_DIRECT 对齐)
    size_t padding = BLOCKS_FILE_HEADER_SIZE - sizeof(BlocksFileHeader);
    std::vector<char> pad_buf(padding, 0);
    out.write(pad_buf.data(), padding);
    
    // 为每个 Block 分配固定大小的 buffer
    std::vector<char> block_buf(block_size, 0);
    std::vector<uint8_t> encode_buf(4096);  // 临时编码缓冲区
    
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
        bh.flags = FLAG_NEIGHBOR_DELTA_VARINT;  // 标记压缩格式
        memset(bh.reserved_pad, 0, sizeof(bh.reserved_pad));
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
        
        // 写 Adjacency Lists (压缩格式: 排序 + delta + varint)
        size_t adj_pos = adj_offset;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t new_id = plan.start_node + i;
            uint16_t ncnt = static_cast<uint16_t>(new_adjacency0[new_id].size());
            memcpy(block_buf.data() + adj_pos, &ncnt, sizeof(uint16_t));
            adj_pos += sizeof(uint16_t);
            if (ncnt > 0) {
                // 排序邻居列表
                std::vector<uint32_t> sorted_adj = new_adjacency0[new_id];
                std::sort(sorted_adj.begin(), sorted_adj.end());
                // Delta + varint 编码
                size_t encoded = delta_varint_encode(sorted_adj.data(), sorted_adj.size(),
                                                     encode_buf.data());
                memcpy(block_buf.data() + adj_pos, encode_buf.data(), encoded);
                adj_pos += encoded;
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
    meta_out << "format=delta_varint_compressed\n";
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
    std::cout << "  Neighbor data: " << total_uncompressed_adj << " -> " << total_compressed_adj
              << " bytes (" << (100.0 * total_compressed_adj / (total_uncompressed_adj + 1))
              << "% of original, " << (100.0 - 100.0 * total_compressed_adj / (total_uncompressed_adj + 1))
              << "% savings)" << std::endl;
    std::cout << "  Total file size: " << (BLOCKS_FILE_HEADER_SIZE + (size_t)num_blocks * block_size) << " bytes" 
              << " (" << (BLOCKS_FILE_HEADER_SIZE + (size_t)num_blocks * block_size) / (1024*1024) << " MB)" << std::endl;
    std::cout << "  Metadata saved to " << meta_path << std::endl;
    std::cout << "Task 1.3 complete." << std::endl;
    return 0;
}
