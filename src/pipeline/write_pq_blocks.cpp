// write_pq_blocks.cpp - Task: Write PQ blocks (neighbor-only, no vectors)
//
// PQ + Disk Offload 方案:
//   - PQ codes (8B/vector) 常驻内存, 用于 ADC 距离计算
//   - Block 只含邻居列表 (图结构), 不含向量数据
//   - Block 大小可以很小 (4KB), 因为每节点只有 ~88B 邻居数据
//
// Block 格式 (与原格式兼容, 但 data_offset=0 表示无向量):
//   [BlockHeader(24B)] [node_ids(4*cnt)] [adj_lists(delta+varint compressed)]
//
// 用法: ./write_pq_blocks <graph_structure.bin> <bfs_order.bin> <pq_codes.bin> <output_blocks.bin> [block_size]
// 默认 block_size = 4096 (4KB)

#include "common.h"
#include <cstring>
#include <algorithm>

// PQ 文件头
#pragma pack(push, 1)
struct PQFileHeader {
    char magic[4];         // "PQCO"
    uint64_t n;            // 节点数
    uint32_t M;            // 子量化器数
    uint32_t nbits;        // 每个子量化器的位数
    uint32_t dim;          // 原始向量维度
    uint32_t codebook_M;   // codebook M 维
    uint32_t codebook_K;   // codebook K (centroids)
    uint32_t codebook_dsub;// codebook dsub
};
#pragma pack(pop)
static_assert(sizeof(PQFileHeader) == 36, "PQFileHeader size mismatch");

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph_structure.bin> <bfs_order.bin> <pq_codes.bin>"
                  << " <output_blocks.bin> [block_size]" << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string pq_path = argv[3];
    std::string output_path = argv[4];
    uint32_t block_size = (argc > 5) ? std::stoul(argv[5]) : 4096;

    std::cout << "=== Write PQ Blocks (neighbor-only, no vectors) ===" << std::endl;

    // 加载图结构 (slim 模式, 只需要邻接表)
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

    // 加载 PQ codes (只读头部验证, 不需要加载全部到内存)
    std::cout << "Loading PQ codes header from " << pq_path << "..." << std::endl;
    std::ifstream pq_in(pq_path, std::ios::binary);
    if (!pq_in.is_open()) {
        std::cerr << "Error: Cannot open PQ codes file: " << pq_path << std::endl;
        return 1;
    }

    PQFileHeader pq_hdr;
    pq_in.read(reinterpret_cast<char*>(&pq_hdr), sizeof(PQFileHeader));
    if (std::memcmp(pq_hdr.magic, "PQCO", 4) != 0) {
        std::cerr << "Error: Invalid PQ codes file magic" << std::endl;
        return 1;
    }
    std::cout << "  PQ: n=" << pq_hdr.n << ", M=" << pq_hdr.M
              << ", nbits=" << pq_hdr.nbits << ", dim=" << pq_hdr.dim
              << ", codebook: " << pq_hdr.codebook_M << "x" << pq_hdr.codebook_K
              << "x" << pq_hdr.codebook_dsub << std::endl;

    if (pq_hdr.n != N) {
        std::cerr << "Warning: PQ n (" << pq_hdr.n << ") != graph nodes (" << N << ")" << std::endl;
    }
    pq_in.close();

    std::cout << "  Block size: " << block_size << " bytes (" << (block_size / 1024) << "KB)" << std::endl;

    // 准备每个节点的新邻居列表 (new_id 空间)
    std::vector<std::vector<uint32_t>> new_adjacency0(N);
    for (uint32_t new_id = 0; new_id < N; new_id++) {
        uint32_t old_id = bfs_order[new_id];
        new_adjacency0[new_id].reserve(g.adjacency0[old_id].size());
        for (uint32_t old_neighbor : g.adjacency0[old_id]) {
            new_adjacency0[new_id].push_back(old_to_new[old_neighbor]);
        }
    }

    // 预计算每个节点的压缩邻接表大小
    std::vector<size_t> compressed_adj_sizes(N);
    std::vector<uint8_t> tmp_buf(N * 32 + 4096);
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

    // Block 切分 (无向量数据, 只有邻居列表)
    // Block 布局: [BlockHeader(24B)] [node_ids(4*cnt)] [adj_lists(变长)]
    struct BlockPlan {
        uint32_t start_node;
        uint32_t end_node;
        uint32_t node_count;
        size_t   block_bytes;
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
            // node_id(4B) + neighbor_count(2B) + compressed_adj
            size_t node_bytes = sizeof(uint32_t) + sizeof(uint16_t) + compressed_adj_sizes[nid];

            if (used + node_bytes > block_size && count > 0) {
                break;
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

    // 统计
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
    std::cout << "\nWriting PQ blocks to " << output_path << "..." << std::endl;
    std::ofstream out(output_path, std::ios::binary);

    // File Header - 使用新的 flag 标记 PQ 模式 (无向量)
    // 使用 FORMAT_VERSION_COMPRESSED 版本号, 并在 BlockHeader 中设置 data_offset=0
    BlocksFileHeader fhdr;
    fhdr.magic = MAGIC_BLOCKS;
    fhdr.version = FORMAT_VERSION_COMPRESSED;
    fhdr.block_size = block_size;
    fhdr.num_blocks = num_blocks;
    out.write(reinterpret_cast<const char*>(&fhdr), sizeof(BlocksFileHeader));

    // Padding 到 BLOCKS_FILE_HEADER_SIZE
    size_t padding = BLOCKS_FILE_HEADER_SIZE - sizeof(BlocksFileHeader);
    std::vector<char> pad_buf(padding, 0);
    out.write(pad_buf.data(), padding);

    // 为每个 Block 分配固定大小的 buffer
    std::vector<char> block_buf(block_size, 0);
    std::vector<uint8_t> encode_buf(4096);

    for (uint32_t b = 0; b < num_blocks; b++) {
        const auto& plan = block_plans[b];
        memset(block_buf.data(), 0, block_size);

        uint32_t cnt = plan.node_count;

        // 计算偏移 (无向量数据)
        uint32_t node_ids_offset = sizeof(BlockHeader);
        uint32_t adj_offset = node_ids_offset + cnt * sizeof(uint32_t);
        // data_offset = 0 表示 PQ 模式 (无向量)

        // 写 Block Header
        BlockHeader bh;
        bh.block_id = b;
        bh.node_count = cnt;
        bh.data_offset = 0;  // PQ 模式: 无向量数据
        bh.adj_offset = adj_offset;
        bh.flags = FLAG_NEIGHBOR_DELTA_VARINT;
        memset(bh.reserved_pad, 0, sizeof(bh.reserved_pad));
        memcpy(block_buf.data(), &bh, sizeof(BlockHeader));

        // 写 Node IDs (new_id)
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t new_id = plan.start_node + i;
            memcpy(block_buf.data() + node_ids_offset + i * sizeof(uint32_t),
                   &new_id, sizeof(uint32_t));
        }

        // 写 Adjacency Lists (delta+varint 压缩)
        size_t adj_pos = adj_offset;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t new_id = plan.start_node + i;
            uint16_t ncnt = static_cast<uint16_t>(new_adjacency0[new_id].size());
            memcpy(block_buf.data() + adj_pos, &ncnt, sizeof(uint16_t));
            adj_pos += sizeof(uint16_t);
            if (ncnt > 0) {
                std::vector<uint32_t> sorted_adj = new_adjacency0[new_id];
                std::sort(sorted_adj.begin(), sorted_adj.end());
                size_t encoded = delta_varint_encode(sorted_adj.data(), sorted_adj.size(),
                                                     encode_buf.data());
                memcpy(block_buf.data() + adj_pos, encode_buf.data(), encoded);
                adj_pos += encoded;
            }
        }

        out.write(block_buf.data(), block_size);

        if ((b + 1) % 1000 == 0 || b == num_blocks - 1) {
            std::cout << "  Written block " << (b + 1) << "/" << num_blocks << std::endl;
        }
    }

    out.close();

    // 输出元数据
    std::string meta_path = output_path + ".meta";
    std::ofstream meta_out(meta_path);
    meta_out << "num_blocks=" << num_blocks << "\n";
    meta_out << "block_size=" << block_size << "\n";
    meta_out << "num_nodes=" << N << "\n";
    meta_out << "dim=" << dim << "\n";
    meta_out << "format=pq_neighbor_only\n";
    meta_out << "pq_codes=" << pq_path << "\n";
    for (uint32_t b = 0; b < num_blocks; b++) {
        const auto& plan = block_plans[b];
        meta_out << "block " << b << " "
                 << plan.start_node << " " << plan.end_node << " " << plan.node_count << " "
                 << plan.block_bytes << "\n";
    }
    meta_out.close();

    // 统计
    std::cout << "\n=== PQ Block Writing Statistics ===" << std::endl;
    std::cout << "  Total blocks: " << num_blocks << std::endl;
    std::cout << "  Block size: " << block_size << " bytes (" << (block_size / 1024) << "KB)" << std::endl;
    std::cout << "  Neighbor data: " << total_compressed_adj << " bytes ("
              << (total_compressed_adj / 1024 / 1024) << " MB)" << std::endl;
    std::cout << "  Total file size: " << (BLOCKS_FILE_HEADER_SIZE + (size_t)num_blocks * block_size)
              << " bytes (" << ((BLOCKS_FILE_HEADER_SIZE + (size_t)num_blocks * block_size) / 1024 / 1024)
              << " MB)" << std::endl;
    std::cout << "  PQ codes (in-memory): " << (pq_hdr.n * pq_hdr.M / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Metadata saved to " << meta_path << std::endl;
    std::cout << "Task complete." << std::endl;
    return 0;
}
