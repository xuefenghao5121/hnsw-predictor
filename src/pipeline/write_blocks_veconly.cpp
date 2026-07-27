// write_blocks_veconly.cpp - Vec-only block generator
// 生成只含向量数据的 blocks (不含邻接表)
// 邻接表由 DiskHNSW 的 CSR 内存表提供
//
// Block 格式 (veconly):
//   [VecOnlyHeader(16B)] [node_ids(4*cnt)] [vector_0(dim*4B)] ... [vector_N]
//   VecOnlyHeader: block_id(4B) + node_count(4B) + data_offset(4B) + flags(4B)
//   flags bit1 = VEC_ONLY (0x02)
//   data_offset = sizeof(VecOnlyHeader) + node_count * sizeof(uint32)
//
// 用法: ./write_blocks_veconly <graph.bin> <bfs.bin> <output.bin> [block_size]
// 默认 block_size = 65536 (64KB)

#include "common.h"
#include <cstring>
#include <algorithm>

// Vec-only block header (16 bytes, 4 fields × 4 bytes)
#pragma pack(push, 1)
struct VecOnlyHeader {
    uint32_t block_id;
    uint32_t node_count;
    uint32_t data_offset;   // offset to first vector (= sizeof(VecOnlyHeader))
    uint32_t flags;         // bit1: VEC_ONLY
};
#pragma pack(pop)
static_assert(sizeof(VecOnlyHeader) == 16, "VecOnlyHeader size");

// FLAG_VEC_ONLY defined in common.h

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <graph.bin> <bfs.bin> <output.bin> [block_size]" << std::endl;
        return 1;
    }

    std::string graph_path = argv[1];
    std::string bfs_path = argv[2];
    std::string output_path = argv[3];
    uint32_t block_size = (argc > 4) ? std::stoul(argv[4]) : 65536;

    std::cout << "=== Vec-Only Block Generator ===" << std::endl;
    std::cout << "  Block size: " << block_size << " (" << (block_size / 1024) << "KB)" << std::endl;

    // Load graph (full - need vectors)
    GraphStructure g = load_graph_structure(graph_path);
    uint32_t N = g.num_nodes;
    uint32_t dim = g.dim;

    // Load BFS order
    std::ifstream bfs_in(bfs_path, std::ios::binary);
    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    if (bhdr.magic != MAGIC_BFS) {
        std::cerr << "Error: Invalid BFS file" << std::endl;
        return 1;
    }

    std::vector<uint32_t> old_to_new(N);
    std::vector<uint32_t> bfs_order(N);
    bfs_in.read(reinterpret_cast<char*>(old_to_new.data()), N * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(bfs_order.data()), N * sizeof(uint32_t));
    bfs_in.close();

    // Plan blocks: only vectors, no adjacency
    // Each node: dim * 4B = vector only
    size_t vec_bytes = dim * sizeof(float);
    size_t header_bytes = sizeof(VecOnlyHeader);
    // capacity = block_size - header - node_ids
    // Each node: node_id(4B) + vector(dim*4B)
    size_t per_node = sizeof(uint32_t) + vec_bytes;
    size_t capacity = block_size - header_bytes;
    uint32_t max_nodes_per_block = capacity / per_node;

    std::cout << "  Nodes per block: max " << max_nodes_per_block << std::endl;
    std::cout << "  Per-node: " << per_node << " bytes (id+vec)" << std::endl;

    // Plan
    struct BlockPlan {
        uint32_t start_node;
        uint32_t node_count;
        size_t   block_bytes;
    };

    std::vector<BlockPlan> plans;
    uint32_t idx = 0;
    while (idx < N) {
        BlockPlan p;
        p.start_node = idx;
        size_t used = header_bytes;
        uint32_t cnt = 0;
        while (idx + cnt < N && used + per_node <= block_size) {
            used += per_node;
            cnt++;
        }
        p.node_count = cnt;
        p.block_bytes = used;
        plans.push_back(p);
        idx += cnt;
    }

    uint32_t num_blocks = plans.size();
    std::cout << "  Total blocks: " << num_blocks << std::endl;
    std::cout << "  Avg nodes/block: " << ((double)N / num_blocks) << std::endl;

    // Stats
    double total_fill = 0;
    for (const auto& p : plans) total_fill += (double)p.block_bytes / block_size;
    std::cout << "  Avg fill rate: " << (total_fill / num_blocks * 100) << "%" << std::endl;
    std::cout << "  Total data: " << ((size_t)num_blocks * block_size / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  vs old format (vec+adj): same block count but adjacency not stored" << std::endl;

    // Write blocks
    std::cout << "\nWriting to " << output_path << "..." << std::endl;
    std::ofstream out(output_path, std::ios::binary);

    // File header (pad to 4096)
    BlocksFileHeader fhdr;
    fhdr.magic = MAGIC_BLOCKS;
    fhdr.version = 3;  // vec-only format
    fhdr.block_size = block_size;
    fhdr.num_blocks = num_blocks;
    out.write(reinterpret_cast<const char*>(&fhdr), sizeof(BlocksFileHeader));
    size_t pad = BLOCKS_FILE_HEADER_SIZE - sizeof(BlocksFileHeader);
    std::vector<char> pad_buf(pad, 0);
    out.write(pad_buf.data(), pad);

    // Write each block
    std::vector<char> block_buf(block_size, 0);
    for (uint32_t b = 0; b < num_blocks; b++) {
        const auto& p = plans[b];
        memset(block_buf.data(), 0, block_size);

        VecOnlyHeader vh;
        vh.block_id = b;
        vh.node_count = p.node_count;
        vh.data_offset = sizeof(VecOnlyHeader) + p.node_count * sizeof(uint32_t);
        vh.flags = FLAG_VEC_ONLY;
        memcpy(block_buf.data(), &vh, sizeof(VecOnlyHeader));

        // Write node_ids
        for (uint32_t i = 0; i < p.node_count; i++) {
            uint32_t new_id = p.start_node + i;
            memcpy(block_buf.data() + sizeof(VecOnlyHeader) + i * sizeof(uint32_t),
                   &new_id, sizeof(uint32_t));
        }

        // Write vectors (in BFS order)
        size_t vec_start = sizeof(VecOnlyHeader) + p.node_count * sizeof(uint32_t);
        for (uint32_t i = 0; i < p.node_count; i++) {
            uint32_t new_id = p.start_node + i;
            uint32_t old_id = bfs_order[new_id];
            memcpy(block_buf.data() + vec_start + i * vec_bytes,
                   &g.vectors[old_id * dim], vec_bytes);
        }

        out.write(block_buf.data(), block_size);
        if ((b + 1) % 1000 == 0 || b == num_blocks - 1)
            std::cout << "  Block " << (b + 1) << "/" << num_blocks << std::endl;
    }
    out.close();

    // Write route table (same as before: node_id → block_id mapping)
    std::string route_path = output_path;
    size_t dot = route_path.rfind('.');
    if (dot != std::string::npos) route_path = route_path.substr(0, dot);
    route_path += "_route.bin";

    std::cout << "\nWriting route table to " << route_path << "..." << std::endl;
    std::vector<uint32_t> route(N);
    for (uint32_t b = 0; b < num_blocks; b++) {
        for (uint32_t i = 0; i < plans[b].node_count; i++) {
            uint32_t new_id = plans[b].start_node + i;
            route[new_id] = b;
        }
    }

    std::ofstream rout(route_path, std::ios::binary);
    RouteHeader rhdr;
    rhdr.magic = MAGIC_ROUTE;
    rhdr.num_entries = N;
    rhdr.block_size = block_size;
    rhdr.reserved = num_blocks;
    rout.write(reinterpret_cast<const char*>(&rhdr), sizeof(RouteHeader));
    rout.write(reinterpret_cast<const char*>(route.data()), N * sizeof(uint32_t));
    rout.close();

    std::cout << "\n=== Vec-Only Blocks Complete ===" << std::endl;
    std::cout << "  Blocks: " << num_blocks << " × " << block_size << "B = "
              << ((size_t)num_blocks * block_size / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Route: " << (N * 4 / 1024 / 1024) << " MB" << std::endl;
    std::cout << "  Adjacency: in CSR memory (not in blocks!)" << std::endl;
    return 0;
}
