// shuffle_vecblocks.cpp - Page Shuffle for vecblocks (DEC-018)
//
// 用途: 按 4KB 页粒度重排 vecblocks 文件, 使 HNSW 图上相邻节点共享同一页。
//       配合 Page Search (DEC-017) 提升页内向量利用率。
//
// 算法 (greedy page clustering):
//   1. 加载图邻接表 (slim_adj 模式, 不加载全量向量)
//   2. 加载 BFS 映射, 将邻接表转换到 new_id 空间
//   3. 对每个 64KB block 内的节点:
//      a. 构建块内邻接子图 (只保留同块邻居)
//      b. 贪心页分配:
//         - 种子: 选块内邻居最多的未分配节点
//         - 后续: 每次选与当前页已有节点共享邻居最多的未分配节点
//         - 页满 (vpp 个) 后开新页
//      c. 按新页顺序重排 node_ids 和 vectors
//   4. 输出新 vecblocks 文件 (格式不变, 块内顺序变了)
//
// 前置条件: SIFT (128D, 512B/vector, 8 vectors/page) 有效。
//           高维数据 (GIST 960D) 一页只放 1 个向量, Shuffle 无收益。
//
// 用法:
//   ./build/shuffle_vecblocks <graph.bin> <bfs.bin> <vecblocks.bin> <output.bin>
//
// 环境变量:
//   PAGE_SHUFFLE_STRATEGY=greedy|random  (默认 greedy)
//   PAGE_SHUFFLE_SEED=42                  (random 策略用)
//   PAGE_SHUFFLE_STATS=1                  (打印页聚类统计)
//
// refines: DEC-006 (BFS 重排布局)
// precondition: DEC-017 (Page Search) 需同一批生成的 shuffle 文件

#include "common.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <numeric>
#include <unordered_set>
#include <cstdlib>
#include <chrono>

static uint32_t vectors_per_page(uint32_t dim) {
    return 4096 / (dim * sizeof(float));
}

// ---- 贪心页聚类 ----
// 输入: 块内节点列表 nodes[0..cnt-1], 块内邻接子图 adj_in_block
// 输出: 新顺序 new_order[0..cnt-1], 使得相邻 vpp 个节点尽量是图邻居
//
// 算法:
//   1. 对每个节点, 计算块内邻居数 (in_block_degree)
//   2. 贪心:
//      - 每页从 in_block_degree 最高的未分配节点开始
//      - 后续选与当前页已有节点共享最多邻居的未分配节点
//      - 平局用 in_block_degree 打破
//      - 页满 (vpp 个) 后开新页
static std::vector<uint32_t> greedy_page_cluster(
    const std::vector<uint32_t>& nodes,  // 块内节点 (new_id)
    uint32_t cnt,
    const std::vector<std::vector<uint32_t>>& adj_new,  // 全图邻接 (new_id 空间)
    const std::unordered_set<uint32_t>& block_set,      // 块内节点集合
    uint32_t vpp)                                       // vectors per page
{
    // 1. 构建块内邻接子图: in_block_adj[i] = 块内邻居的 local index
    std::vector<std::vector<uint32_t>> in_block_adj(cnt);
    // node -> local index 映射
    std::unordered_map<uint32_t, uint32_t> node_to_local;
    for (uint32_t i = 0; i < cnt; i++) {
        node_to_local[nodes[i]] = i;
    }

    std::vector<uint32_t> in_block_degree(cnt, 0);
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t nid = nodes[i];
        for (uint32_t nb : adj_new[nid]) {
            auto it = node_to_local.find(nb);
            if (it != node_to_local.end()) {
                in_block_adj[i].push_back(it->second);
                in_block_degree[i]++;
            }
        }
    }

    // 2. 贪心页分配
    std::vector<uint32_t> new_order;
    new_order.reserve(cnt);
    std::vector<bool> assigned(cnt, false);

    // 页内已分配节点的 local indices (用于计算候选与页的共享邻居数)
    std::vector<uint32_t> current_page;
    current_page.reserve(vpp);

    uint32_t assigned_count = 0;
    while (assigned_count < cnt) {
        // 开新页: 选 in_block_degree 最高的未分配节点作为种子
        uint32_t seed = cnt;  // invalid
        uint32_t best_deg = 0;
        for (uint32_t i = 0; i < cnt; i++) {
            if (!assigned[i] && in_block_degree[i] >= best_deg) {
                best_deg = in_block_degree[i];
                seed = i;
            }
        }

        current_page.clear();
        current_page.push_back(seed);
        assigned[seed] = true;
        assigned_count++;
        new_order.push_back(seed);

        // 填充当前页剩余位置
        while (current_page.size() < vpp && assigned_count < cnt) {
            uint32_t best_node = cnt;
            uint32_t best_shared = 0;
            uint32_t best_tiebreak = 0;

            for (uint32_t i = 0; i < cnt; i++) {
                if (assigned[i]) continue;

                // 计算节点 i 与当前页的共享邻居数
                uint32_t shared = 0;
                for (uint32_t nb : in_block_adj[i]) {
                    // 检查 nb 是否在当前页
                    for (uint32_t p : current_page) {
                        if (nb == p) { shared++; break; }
                    }
                }

                if (shared > best_shared ||
                    (shared == best_shared && in_block_degree[i] > best_tiebreak)) {
                    best_shared = shared;
                    best_tiebreak = in_block_degree[i];
                    best_node = i;
                }
            }

            if (best_node == cnt) break;  // 不应发生

            current_page.push_back(best_node);
            assigned[best_node] = true;
            assigned_count++;
            new_order.push_back(best_node);
        }
    }

    return new_order;
}

// ---- 随机重排 (baseline) ----
static std::vector<uint32_t> random_shuffle(
    uint32_t cnt, uint64_t seed)
{
    std::vector<uint32_t> order(cnt);
    std::iota(order.begin(), order.end(), 0);
    // 简单 LCG 随机
    uint64_t s = seed;
    for (uint32_t i = cnt - 1; i > 0; i--) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t j = (uint32_t)(s % (i + 1));
        std::swap(order[i], order[j]);
    }
    return order;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <graph.bin> <bfs.bin> <vecblocks.bin> <output.bin>" << std::endl;
        std::cerr << "  DEC-018: Page Shuffle - 按 4KB 页粒度重排 vecblocks" << std::endl;
        std::cerr << "  Env: PAGE_SHUFFLE_STRATEGY=greedy|random (default: greedy)" << std::endl;
        std::cerr << "       PAGE_SHUFFLE_SEED=42 (random strategy)" << std::endl;
        std::cerr << "       PAGE_SHUFFLE_STATS=1 (print clustering stats)" << std::endl;
        return 1;
    }

    std::string graph_path   = argv[1];
    std::string bfs_path      = argv[2];
    std::string input_path   = argv[3];
    std::string output_path  = argv[4];

    const char* strategy_env = std::getenv("PAGE_SHUFFLE_STRATEGY");
    std::string strategy = strategy_env ? strategy_env : "greedy";
    const char* seed_env = std::getenv("PAGE_SHUFFLE_SEED");
    uint64_t seed = seed_env ? std::stoull(seed_env) : 42;
    bool print_stats = std::getenv("PAGE_SHUFFLE_STATS") && std::atoi(std::getenv("PAGE_SHUFFLE_STATS")) != 0;

    std::cout << "=== Page Shuffle (DEC-018) ===" << std::endl;
    std::cout << "  graph:    " << graph_path << std::endl;
    std::cout << "  bfs:      " << bfs_path << std::endl;
    std::cout << "  input:    " << input_path << std::endl;
    std::cout << "  output:   " << output_path << std::endl;
    std::cout << "  strategy: " << strategy << std::endl;
    std::cout << "  seed:     " << seed << std::endl;

    auto t_start = std::chrono::high_resolution_clock::now();

    // ---- 1. 加载图邻接表 (slim_adj, 不加载全量向量) ----
    std::cout << "[1] Loading graph adjacency (slim_adj)..." << std::endl;
    GraphStructure g = load_graph_structure_slim_adj(graph_path);
    uint32_t N = g.num_nodes;
    uint32_t dim = g.dim;
    std::cout << "  nodes: " << N << ", dim: " << dim << std::endl;

    // ---- 2. 加载 BFS 映射 ----
    std::cout << "[2] Loading BFS order..." << std::endl;
    std::ifstream bfs_in(bfs_path, std::ios::binary);
    if (!bfs_in.is_open()) {
        std::cerr << "ERROR: Cannot open " << bfs_path << std::endl;
        return 1;
    }
    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    if (bhdr.magic != MAGIC_BFS) {
        std::cerr << "ERROR: Invalid BFS file magic" << std::endl;
        return 1;
    }
    std::vector<uint32_t> old_to_new(N), new_to_old(N);
    bfs_in.read(reinterpret_cast<char*>(old_to_new.data()), N * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(new_to_old.data()), N * sizeof(uint32_t));
    bfs_in.close();
    std::cout << "  BFS nodes: " << bhdr.num_nodes << ", entry: " << bhdr.entry_point << std::endl;

    // ---- 3. 将邻接表转换到 new_id 空间 ----
    std::cout << "[3] Converting adjacency to new_id space..." << std::endl;
    std::vector<std::vector<uint32_t>> adj_new(N);
    size_t total_edges = 0;
    for (uint32_t old_id = 0; old_id < N; old_id++) {
        uint32_t new_id = old_to_new[old_id];
        adj_new[new_id].reserve(g.adjacency0[old_id].size());
        for (uint32_t nb_old : g.adjacency0[old_id]) {
            adj_new[new_id].push_back(old_to_new[nb_old]);
        }
        total_edges += adj_new[new_id].size();
    }
    std::cout << "  Total edges (new_id space): " << total_edges << std::endl;

    // ---- 4. 读取 vecblocks 文件头 ----
    std::cout << "[4] Reading vecblocks header..." << std::endl;
    std::ifstream vb_in(input_path, std::ios::binary);
    if (!vb_in.is_open()) {
        std::cerr << "ERROR: Cannot open " << input_path << std::endl;
        return 1;
    }
    char hdr_buf[4096];
    vb_in.read(hdr_buf, 4096);
    BlocksFileHeader fhdr;
    std::memcpy(&fhdr, hdr_buf, sizeof(BlocksFileHeader));
    if (fhdr.magic != MAGIC_BLOCKS) {
        std::cerr << "ERROR: Invalid vecblocks file magic" << std::endl;
        return 1;
    }
    uint32_t block_size = fhdr.block_size;
    uint32_t num_blocks = fhdr.num_blocks;
    std::cout << "  block_size=" << block_size << " (" << (block_size/1024) << "KB)"
              << ", num_blocks=" << num_blocks << std::endl;

    uint32_t vpp = vectors_per_page(dim);
    size_t vec_bytes = (size_t)dim * sizeof(float);
    std::cout << "  dim=" << dim << ", vec_bytes=" << vec_bytes
              << ", vectors_per_page=" << vpp << std::endl;

    if (vpp <= 1) {
        std::cerr << "WARNING: vectors_per_page=" << vpp << " (dim=" << dim
                  << " too large, shuffle has no benefit). Copying as-is." << std::endl;
        vb_in.seekg(0, std::ios::beg);
        std::ofstream dst(output_path, std::ios::binary);
        dst << vb_in.rdbuf();
        return 0;
    }

    // ---- 5. 打开输出文件 ----
    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "ERROR: Cannot open " << output_path << std::endl;
        return 1;
    }
    // 写文件头 (原样拷贝 4096B)
    out.write(hdr_buf, 4096);

    // ---- 6. 逐块处理 ----
    std::cout << "[5] Processing blocks..." << std::endl;

    // 统计
    uint64_t total_nodes = 0;
    uint64_t total_pages = 0;
    uint64_t total_neighbor_pairs_in_page = 0;  // 页内邻居对数 (greedy)
    uint64_t total_neighbor_pairs_in_page_orig = 0;  // 原始顺序页内邻居对数
    uint64_t total_in_block_edges = 0;

    // 块缓冲区
    std::vector<char> block_buf(block_size, 0);
    std::vector<char> out_block_buf(block_size, 0);

    for (uint32_t b = 0; b < num_blocks; b++) {
        // 读取块
        vb_in.read(block_buf.data(), block_size);
        if (!vb_in) {
            std::cerr << "ERROR: Read failed at block " << b << std::endl;
            return 1;
        }

        // 解析 header
        uint32_t block_id, cnt, data_offset, flags;
        std::memcpy(&block_id, block_buf.data() + 0, 4);
        std::memcpy(&cnt, block_buf.data() + 4, 4);
        std::memcpy(&data_offset, block_buf.data() + 8, 4);
        std::memcpy(&flags, block_buf.data() + 12, 4);

        if (cnt == 0) {
            // 空块, 直接拷贝
            out.write(block_buf.data(), block_size);
            continue;
        }

        // 读取 node_ids
        std::vector<uint32_t> nodes(cnt);
        std::memcpy(nodes.data(), block_buf.data() + 16, cnt * sizeof(uint32_t));

        // 读取 vectors (指向 block_buf 中的数据)
        const char* vec_data = block_buf.data() + data_offset;

        // 构建块内节点集合
        std::unordered_set<uint32_t> block_set(nodes.begin(), nodes.end());

        // 计算新顺序
        std::vector<uint32_t> new_order;
        if (strategy == "random") {
            new_order = random_shuffle(cnt, seed + b);
        } else {
            new_order = greedy_page_cluster(nodes, cnt, adj_new, block_set, vpp);
        }

        // 统计: 页内邻居对数
        if (print_stats) {
            // 原始顺序
            for (uint32_t p = 0; p < cnt; p += vpp) {
                uint32_t pend = std::min(p + vpp, cnt);
                for (uint32_t i = p; i < pend; i++) {
                    uint32_t nid = nodes[i];
                    for (uint32_t nb : adj_new[nid]) {
                        if (block_set.count(nb)) {
                            // 检查 nb 是否在同一页
                            uint32_t nb_local = 0;
                            for (uint32_t k = 0; k < cnt; k++) {
                                if (nodes[k] == nb) { nb_local = k; break; }
                            }
                            if (nb_local >= p && nb_local < pend) {
                                total_neighbor_pairs_in_page_orig++;
                            }
                        }
                    }
                }
            }
            // 新顺序
            for (uint32_t p = 0; p < cnt; p += vpp) {
                uint32_t pend = std::min(p + vpp, cnt);
                for (uint32_t i = p; i < pend; i++) {
                    uint32_t nid = nodes[new_order[i]];
                    for (uint32_t nb : adj_new[nid]) {
                        if (block_set.count(nb)) {
                            uint32_t nb_local = 0;
                            for (uint32_t k = 0; k < cnt; k++) {
                                if (nodes[new_order[k]] == nb) { nb_local = k; break; }
                            }
                            if (nb_local >= p && nb_local < pend) {
                                total_neighbor_pairs_in_page++;
                            }
                        }
                    }
                }
            }
            // 块内总边数
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t nid = nodes[i];
                for (uint32_t nb : adj_new[nid]) {
                    if (block_set.count(nb)) total_in_block_edges++;
                }
            }
            total_pages += (cnt + vpp - 1) / vpp;
        }

        // 写出重排后的块
        memset(out_block_buf.data(), 0, block_size);

        // header 不变
        std::memcpy(out_block_buf.data(), block_buf.data(), 16);

        // 重排 node_ids
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t nid = nodes[new_order[i]];
            std::memcpy(out_block_buf.data() + 16 + i * sizeof(uint32_t),
                        &nid, sizeof(uint32_t));
        }

        // 重排 vectors
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t src_slot = new_order[i];
            std::memcpy(out_block_buf.data() + data_offset + i * vec_bytes,
                        vec_data + src_slot * vec_bytes,
                        vec_bytes);
        }

        // 拷贝剩余部分 (如果有 padding)
        size_t used = data_offset + cnt * vec_bytes;
        if (used < block_size) {
            // 保留原始 padding (通常是零)
            std::memcpy(out_block_buf.data() + used,
                        block_buf.data() + used,
                        block_size - used);
        }

        out.write(out_block_buf.data(), block_size);
        total_nodes += cnt;

        if ((b + 1) % 1000 == 0 || b == num_blocks - 1) {
            std::cout << "  Block " << (b + 1) << "/" << num_blocks << std::endl;
        }
    }

    vb_in.close();
    out.close();

    auto t_end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    std::cout << "\n=== DEC-018 Page Shuffle complete ===" << std::endl;
    std::cout << "  output:      " << output_path << std::endl;
    std::cout << "  blocks:      " << num_blocks << std::endl;
    std::cout << "  total_nodes: " << total_nodes << std::endl;
    std::cout << "  strategy:    " << strategy << std::endl;
    std::cout << "  elapsed:     " << ms << " ms" << std::endl;

    if (print_stats && total_in_block_edges > 0) {
        std::cout << "\n=== Page Clustering Statistics ===" << std::endl;
        std::cout << "  total_pages:              " << total_pages << std::endl;
        std::cout << "  total_in_block_edges:     " << total_in_block_edges << std::endl;
        std::cout << "  neighbor_pairs_in_page (orig):  " << total_neighbor_pairs_in_page_orig
                  << " (" << (100.0 * total_neighbor_pairs_in_page_orig / total_in_block_edges) << "%)" << std::endl;
        std::cout << "  neighbor_pairs_in_page (shuffled): " << total_neighbor_pairs_in_page
                  << " (" << (100.0 * total_neighbor_pairs_in_page / total_in_block_edges) << "%)" << std::endl;
        double improvement = 0;
        if (total_neighbor_pairs_in_page_orig > 0) {
            improvement = 100.0 * (total_neighbor_pairs_in_page - total_neighbor_pairs_in_page_orig)
                        / total_neighbor_pairs_in_page_orig;
        }
        std::cout << "  improvement:              " << improvement << "%" << std::endl;
    }

    return 0;
}
