// common.h - 公共定义和工具函数
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <cmath>
#include <unordered_map>

// ============================================================
// 常量定义
// ============================================================

// 文件 Magic Numbers
static constexpr uint32_t MAGIC_GRAPH   = 0x47524148; // "GRPH"
static constexpr uint32_t MAGIC_BLOCKS  = 0x424C4B48; // "BHKH" 
static constexpr uint32_t MAGIC_ROUTE   = 0x524F5554; // "ROUT"
static constexpr uint32_t MAGIC_BFS     = 0x42465300; // "BFS\0"

static constexpr uint32_t FORMAT_VERSION = 1;
static constexpr uint32_t FORMAT_VERSION_COMPRESSED = 2;  // delta+varint neighbor encoding

// 默认 Block 大小: 256KB
static constexpr uint32_t DEFAULT_BLOCK_SIZE = 256 * 1024;

// Blocks 文件头部保留大小（4096 字节，O_DIRECT 对齐）
// 实际 BlocksFileHeader 只有 16 字节，但文件中保留 4096 字节
static constexpr size_t BLOCKS_FILE_HEADER_SIZE = 4096;

// ============================================================
// graph_structure.bin 格式
// ============================================================

#pragma pack(push, 1)
struct GraphHeader {
    uint32_t magic;          // MAGIC_GRAPH
    uint32_t version;        // FORMAT_VERSION
    uint32_t num_nodes;      // 节点总数
    uint32_t dim;            // 向量维度
    uint32_t maxM;           // 上层最大邻居数
    uint32_t maxM0;          // level0 最大邻居数
    uint32_t entry_point;    // 入口节点 (internal ID)
    int32_t  max_level;      // 最大层级
    uint32_t data_size;      // 单向量字节数
    uint32_t reserved;       // 保留字段
};
// sizeof = 44 bytes, 4 bytes padding to 48
static_assert(sizeof(GraphHeader) == 40, "GraphHeader size mismatch");
#pragma pack(pop)

// ============================================================
// blocks.bin 格式
// ============================================================

#pragma pack(push, 1)
struct BlocksFileHeader {
    uint32_t magic;          // MAGIC_BLOCKS
    uint32_t version;        // FORMAT_VERSION
    uint32_t block_size;     // Block 固定大小（字节）
    uint32_t num_blocks;     // Block 总数
};

struct BlockHeader {
    uint32_t block_id;       // Block 编号
    uint32_t node_count;     // Block 内节点数
    uint32_t data_offset;    // 向量数据起始偏移（相对 Block 起始）
    uint32_t adj_offset;     // 邻接表起始偏移（相对 Block 起始）
    uint8_t  flags;          // bit0: neighbor delta+varint compression
    uint8_t  reserved_pad[7];// 填充对齐
};
static_assert(sizeof(BlockHeader) == 24, "BlockHeader size mismatch");
#pragma pack(pop)

// BlockHeader flags
static constexpr uint8_t FLAG_NEIGHBOR_DELTA_VARINT = 0x01;
static constexpr uint32_t FLAG_VEC_ONLY = 0x02;  // vec-only block (no adjacency)

// ============================================================
// route_table.bin 格式
// ============================================================

#pragma pack(push, 1)
struct RouteHeader {
    uint32_t magic;          // MAGIC_ROUTE
    uint32_t num_entries;    // 节点总数（= num_nodes）
    uint32_t block_size;     // Block 大小（字节）
    uint32_t reserved;       // 保留
};
static_assert(sizeof(RouteHeader) == 16, "RouteHeader size mismatch");
#pragma pack(pop)

// ============================================================
// bfs_order.bin 格式
// ============================================================

#pragma pack(push, 1)
struct BfsHeader {
    uint32_t magic;          // MAGIC_BFS
    uint32_t num_nodes;      // 节点总数
    uint32_t entry_point;    // BFS 起始节点
    uint32_t reserved;       // 保留
};
static_assert(sizeof(BfsHeader) == 16, "BfsHeader size mismatch");
#pragma pack(pop)

// ============================================================
// 图结构内存表示
// ============================================================

struct GraphStructure {
    uint32_t num_nodes = 0;
    uint32_t dim = 0;
    uint32_t maxM = 0;
    uint32_t maxM0 = 0;
    uint32_t entry_point = 0;
    int32_t  max_level = 0;
    uint32_t data_size = 0;
    
    std::vector<int32_t> levels;           // [num_nodes]
    std::vector<float>   vectors;          // [num_nodes * dim] (全量, slim模式不加载)
    std::vector<uint64_t> labels;          // [num_nodes]
    
    // Level 0 邻接表: adjacency0[i] = vector of neighbor IDs
    std::vector<std::vector<uint32_t>> adjacency0;
    
    // 上层邻接表: upper_adjacency[i][level] = vector of neighbor IDs (level >= 1)
    std::vector<std::vector<std::vector<uint32_t>>> upper_adjacency;

    // ---- slim 模式: 只加载上层节点的向量 ----
    // 当 slim=true 时, vectors 和 adjacency0 为空, 使用 upper_vectors
    bool slim = false;
    std::unordered_map<uint32_t, std::vector<float>> upper_vectors;  // old_id -> vector
};

// ============================================================
// Varint encoding/decoding (LEB128 unsigned)
// ============================================================

// Encode a uint32 value as varint into buffer, returns bytes written
inline size_t varint_encode(uint32_t value, uint8_t* buf) {
    size_t n = 0;
    while (value >= 0x80) {
        buf[n++] = (value & 0x7F) | 0x80;
        value >>= 7;
    }
    buf[n++] = value;
    return n;
}

// Decode a varint from buffer, returns bytes consumed; out gets decoded value
inline size_t varint_decode(const uint8_t* buf, size_t available, uint32_t& out) {
    out = 0;
    size_t n = 0;
    int shift = 0;
    while (n < available && n < 5) {
        uint8_t b = buf[n++];
        out |= (uint32_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return n;
        shift += 7;
    }
    // malformed varint
    out = 0;
    return 0;
}

// Encode a sorted vector of uint32 IDs as delta+varint into buffer
// Returns total bytes written
inline size_t delta_varint_encode(const uint32_t* ids, size_t count, uint8_t* buf) {
    if (count == 0) return 0;
    size_t pos = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t delta = ids[i] - prev;
        pos += varint_encode(delta, buf + pos);
        prev = ids[i];
    }
    return pos;
}

// Decode delta+varint encoded neighbor list into output vector
// Returns bytes consumed, or 0 on error
inline size_t delta_varint_decode(const uint8_t* buf, size_t available, size_t count,
                                   std::vector<uint32_t>& out) {
    out.resize(count);
    size_t pos = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t delta;
        size_t n = varint_decode(buf + pos, available - pos, delta);
        if (n == 0) return 0;
        pos += n;
        prev += delta;
        out[i] = prev;
    }
    return pos;
}

// ============================================================
// 工具函数
// ============================================================

// 读取 fvecs 格式文件
// fvecs 格式: [dim(int32) | dim * float] 重复
inline std::vector<float> read_fvecs(const std::string& path, int& dim, size_t& count) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    
    // 获取文件大小
    in.seekg(0, std::ios::end);
    size_t file_size = in.tellg();
    in.seekg(0, std::ios::beg);
    
    // 读取第一个 int32 获取维度
    int32_t d;
    in.read(reinterpret_cast<char*>(&d), sizeof(int32_t));
    dim = d;
    in.seekg(0, std::ios::beg);
    
    size_t record_size = sizeof(int32_t) + dim * sizeof(float);
    count = file_size / record_size;
    
    std::vector<float> data(count * dim);
    std::vector<int32_t> dim_buf(1);
    
    for (size_t i = 0; i < count; i++) {
        in.read(reinterpret_cast<char*>(dim_buf.data()), sizeof(int32_t));
        // 验证维度一致
        if (dim_buf[0] != dim) {
            throw std::runtime_error("Inconsistent dimension in fvecs file at record " + std::to_string(i));
        }
        in.read(reinterpret_cast<char*>(&data[i * dim]), dim * sizeof(float));
    }
    
    in.close();
    return data;
}

// 写入二进制 POD
template<typename T>
inline void write_pod(std::ofstream& out, const T& val) {
    out.write(reinterpret_cast<const char*>(&val), sizeof(T));
}

// 读取二进制 POD
template<typename T>
inline void read_pod(std::ifstream& in, T& val) {
    in.read(reinterpret_cast<char*>(&val), sizeof(T));
}

// 将 graph_structure 保存到文件
inline void save_graph_structure(const std::string& path, const GraphStructure& g) {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot create file: " + path);
    }
    
    // Header (填充到 48 字节，加 4 字节 padding)
    GraphHeader hdr;
    hdr.magic = MAGIC_GRAPH;
    hdr.version = FORMAT_VERSION;
    hdr.num_nodes = g.num_nodes;
    hdr.dim = g.dim;
    hdr.maxM = g.maxM;
    hdr.maxM0 = g.maxM0;
    hdr.entry_point = g.entry_point;
    hdr.max_level = g.max_level;
    hdr.data_size = g.data_size;
    hdr.reserved = 0;
    
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(GraphHeader));
    // 写 4 字节 padding 使 header 对齐到 44 字节
    uint32_t pad = 0;
    write_pod(out, pad);
    
    // Element Levels
    out.write(reinterpret_cast<const char*>(g.levels.data()), g.num_nodes * sizeof(int32_t));
    
    // Vector Data
    out.write(reinterpret_cast<const char*>(g.vectors.data()), (size_t)g.num_nodes * g.dim * sizeof(float));
    
    // Labels
    out.write(reinterpret_cast<const char*>(g.labels.data()), g.num_nodes * sizeof(uint64_t));
    
    // Level 0 Adjacency Lists
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        uint16_t cnt = static_cast<uint16_t>(g.adjacency0[i].size());
        write_pod(out, cnt);
        if (cnt > 0) {
            out.write(reinterpret_cast<const char*>(g.adjacency0[i].data()), cnt * sizeof(uint32_t));
        }
    }
    
    // Upper Level Adjacency Lists
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) {
            for (int32_t lv = 1; lv <= g.levels[i]; lv++) {
                const auto& neighbors = g.upper_adjacency[i][lv];
                uint16_t cnt = static_cast<uint16_t>(neighbors.size());
                write_pod(out, cnt);
                if (cnt > 0) {
                    out.write(reinterpret_cast<const char*>(neighbors.data()), cnt * sizeof(uint32_t));
                }
            }
        }
    }
    
    out.close();
    std::cout << "  Saved graph structure to " << path << " (" << g.num_nodes << " nodes)" << std::endl;
}

// 从文件加载 graph_structure
inline GraphStructure load_graph_structure(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }
    
    GraphStructure g;
    
    GraphHeader hdr;
    in.read(reinterpret_cast<char*>(&hdr), sizeof(GraphHeader));
    uint32_t pad;
    read_pod(in, pad); // padding
    
    if (hdr.magic != MAGIC_GRAPH) {
        throw std::runtime_error("Invalid graph structure file: bad magic");
    }
    
    g.num_nodes = hdr.num_nodes;
    g.dim = hdr.dim;
    g.maxM = hdr.maxM;
    g.maxM0 = hdr.maxM0;
    g.entry_point = hdr.entry_point;
    g.max_level = hdr.max_level;
    g.data_size = hdr.data_size;
    
    // Element Levels
    g.levels.resize(g.num_nodes);
    in.read(reinterpret_cast<char*>(g.levels.data()), g.num_nodes * sizeof(int32_t));
    
    // Vector Data
    g.vectors.resize((size_t)g.num_nodes * g.dim);
    in.read(reinterpret_cast<char*>(g.vectors.data()), (size_t)g.num_nodes * g.dim * sizeof(float));
    
    // Labels
    g.labels.resize(g.num_nodes);
    in.read(reinterpret_cast<char*>(g.labels.data()), g.num_nodes * sizeof(uint64_t));
    
    // Level 0 Adjacency Lists
    g.adjacency0.resize(g.num_nodes);
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        uint16_t cnt;
        read_pod(in, cnt);
        g.adjacency0[i].resize(cnt);
        if (cnt > 0) {
            in.read(reinterpret_cast<char*>(g.adjacency0[i].data()), cnt * sizeof(uint32_t));
        }
    }
    
    // Upper Level Adjacency Lists
    g.upper_adjacency.resize(g.num_nodes);
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) {
            g.upper_adjacency[i].resize(g.levels[i] + 1); // index 0 unused
            for (int32_t lv = 1; lv <= g.levels[i]; lv++) {
                uint16_t cnt;
                read_pod(in, cnt);
                g.upper_adjacency[i][lv].resize(cnt);
                if (cnt > 0) {
                    in.read(reinterpret_cast<char*>(g.upper_adjacency[i][lv].data()), cnt * sizeof(uint32_t));
                }
            }
        }
    }
    
    in.close();
    std::cout << "  Loaded graph structure from " << path << " (" << g.num_nodes << " nodes)" << std::endl;
    return g;
}

// ============================================================
// load_graph_structure_slim: 只加载上层图结构，跳过L0数据和全量向量
//
// 内存占用对比 (SIFT1M, dim=128, M=16):
//   全量加载: vectors(512MB) + adjacency0(~60MB) + labels(8MB) + levels(4MB) ≈ 584MB
//   slim加载: upper_vectors(~32MB) + labels(8MB) + levels(4MB) + upper_adj(~8MB) ≈ 52MB
// ============================================================
inline GraphStructure load_graph_structure_slim(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    GraphStructure g;
    g.slim = true;

    // ---- 1. Header ----
    GraphHeader hdr;
    in.read(reinterpret_cast<char*>(&hdr), sizeof(GraphHeader));
    uint32_t pad;
    read_pod(in, pad);

    if (hdr.magic != MAGIC_GRAPH) {
        throw std::runtime_error("Invalid graph structure file: bad magic");
    }

    g.num_nodes = hdr.num_nodes;
    g.dim = hdr.dim;
    g.maxM = hdr.maxM;
    g.maxM0 = hdr.maxM0;
    g.entry_point = hdr.entry_point;
    g.max_level = hdr.max_level;
    g.data_size = hdr.data_size;

    // ---- 2. Element Levels (全部加载，需要判断哪些节点有上层) ----
    g.levels.resize(g.num_nodes);
    in.read(reinterpret_cast<char*>(g.levels.data()), g.num_nodes * sizeof(int32_t));

    // 统计上层节点数
    size_t upper_count = 0;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) upper_count++;
    }

    // ---- 3. 向量数据: 只读取上层节点的向量 ----
    size_t vec_base = (size_t)in.tellg();  // 当前位置就是向量数据起始
    size_t data_size_bytes = (size_t)g.num_nodes * g.data_size;

    std::cout << "  [slim] Loading vectors for " << upper_count << " upper-layer nodes ("
              << (upper_count * g.data_size / 1024 / 1024) << "MB, skipping "
              << (data_size_bytes / 1024 / 1024) << "MB)" << std::endl;

    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) {
            // seek 到节点 i 的向量位置
            in.seekg(vec_base + (size_t)i * g.data_size);
            std::vector<float> vec(g.dim);
            in.read(reinterpret_cast<char*>(vec.data()), g.data_size);
            g.upper_vectors[i] = std::move(vec);
        }
    }

    // ---- 4. Labels (全部加载，搜索结果需要) ----
    size_t label_base = vec_base + data_size_bytes;
    in.seekg(label_base);
    g.labels.resize(g.num_nodes);
    in.read(reinterpret_cast<char*>(g.labels.data()), g.num_nodes * sizeof(uint64_t));

    // ---- 5. 跳过 Level 0 邻接表 (在 blocks.bin 中有) ----
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        uint16_t cnt;
        read_pod(in, cnt);
        if (cnt > 0) {
            in.seekg((size_t)cnt * sizeof(uint32_t), std::ios::cur);
        }
    }

    // ---- 6. 上层邻接表 (全部加载，贪心下降需要) ----
    g.upper_adjacency.resize(g.num_nodes);
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) {
            g.upper_adjacency[i].resize(g.levels[i] + 1);
            for (int32_t lv = 1; lv <= g.levels[i]; lv++) {
                uint16_t cnt;
                read_pod(in, cnt);
                g.upper_adjacency[i][lv].resize(cnt);
                if (cnt > 0) {
                    in.read(reinterpret_cast<char*>(g.upper_adjacency[i][lv].data()),
                            cnt * sizeof(uint32_t));
                }
            }
        }
    }

    in.close();
    std::cout << "  [slim] Loaded graph structure from " << path
              << " (" << g.num_nodes << " nodes, " << upper_count << " upper)" << std::endl;
    return g;
}

// ============================================================
// slim+adj 加载器: 加载标签、层级、上层节点+向量, 同时加载 L0 邻接表
// 用于 multi-hop 预取: 邻接表常驻内存 (old_id 空间)
// RSS: upper_vectors(~32MB) + labels(8MB) + levels(4MB) + upper_adj(~8MB) + L0_adj(~81MB) ≈ 133MB
// ============================================================
inline GraphStructure load_graph_structure_slim_adj(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    GraphStructure g;
    g.slim = true;

    GraphHeader hdr;
    in.read(reinterpret_cast<char*>(&hdr), sizeof(GraphHeader));
    uint32_t pad;
    read_pod(in, pad);

    if (hdr.magic != MAGIC_GRAPH) {
        throw std::runtime_error("Invalid graph structure file: bad magic");
    }

    g.num_nodes = hdr.num_nodes;
    g.dim = hdr.dim;
    g.maxM = hdr.maxM;
    g.maxM0 = hdr.maxM0;
    g.entry_point = hdr.entry_point;
    g.max_level = hdr.max_level;
    g.data_size = hdr.data_size;

    g.levels.resize(g.num_nodes);
    in.read(reinterpret_cast<char*>(g.levels.data()), g.num_nodes * sizeof(int32_t));

    size_t upper_count = 0;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) upper_count++;
    }

    size_t vec_base = (size_t)in.tellg();
    size_t data_size_bytes = (size_t)g.num_nodes * g.data_size;

    std::cout << "  [slim+adj] Loading vectors for " << upper_count << " upper-layer nodes ("
              << (upper_count * g.data_size / 1024 / 1024) << "MB, skipping "
              << (data_size_bytes / 1024 / 1024) << "MB)" << std::endl;

    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) {
            in.seekg(vec_base + (size_t)i * g.data_size);
            std::vector<float> vec(g.dim);
            in.read(reinterpret_cast<char*>(vec.data()), g.data_size);
            g.upper_vectors[i] = std::move(vec);
        }
    }

    size_t label_base = vec_base + data_size_bytes;
    in.seekg(label_base);
    g.labels.resize(g.num_nodes);
    in.read(reinterpret_cast<char*>(g.labels.data()), g.num_nodes * sizeof(uint64_t));

    // ---- L0 邻接表 (全部加载, 用于 multi-hop 预取) ----
    size_t adj0_base = (size_t)in.tellg();
    g.adjacency0.resize(g.num_nodes);
    size_t total_edges = 0;
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        uint16_t cnt;
        read_pod(in, cnt);
        g.adjacency0[i].resize(cnt);
        if (cnt > 0) {
            in.read(reinterpret_cast<char*>(g.adjacency0[i].data()), cnt * sizeof(uint32_t));
            total_edges += cnt;
        }
    }
    std::cout << "  [slim+adj] Loaded L0 adjacency: " << total_edges << " edges ("
              << (total_edges * 4 / 1024 / 1024) << "MB)" << std::endl;

    // ---- 上层邻接表 ----
    g.upper_adjacency.resize(g.num_nodes);
    for (uint32_t i = 0; i < g.num_nodes; i++) {
        if (g.levels[i] > 0) {
            g.upper_adjacency[i].resize(g.levels[i] + 1);
            for (int32_t lv = 1; lv <= g.levels[i]; lv++) {
                uint16_t cnt;
                read_pod(in, cnt);
                g.upper_adjacency[i][lv].resize(cnt);
                if (cnt > 0) {
                    in.read(reinterpret_cast<char*>(g.upper_adjacency[i][lv].data()),
                            cnt * sizeof(uint32_t));
                }
            }
        }
    }

    in.close();
    std::cout << "  [slim+adj] Loaded graph structure from " << path
              << " (" << g.num_nodes << " nodes, " << upper_count << " upper, "
              << total_edges << " edges)" << std::endl;
    return g;
}
