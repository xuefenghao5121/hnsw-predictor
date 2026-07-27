// test_block_cache.cpp - BlockCache 单元测试
//
// 测试内容：
//   1. 基础加载测试：构造 BlockCache，验证文件正确加载
//   2. 缓存命中/未命中测试：访问节点，验证第一次 miss、第二次 hit
//   3. LRU 淘汰测试：cache_slots=1，访问两个不同 Block 的节点
//   4. 节点数据正确性测试：对比 BlockCache 返回的数据与 graph.bin 原始数据
//   5. 统计信息测试：验证 hit/miss/eviction 计数正确
//
// 编译: g++ -std=c++17 -O2 -o test_block_cache test_block_cache.cpp block_cache.cpp
// 运行: ./test_block_cache <output_dir>
//   output_dir 包含 test_graph.bin, test_blocks.bin, test_route.bin, test_bfs.bin

#include "common.h"
#include "block_cache.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

// 测试框架
static int test_count = 0;
static int test_passed = 0;

#define TEST(name) \
    do { \
        std::cout << "\n  [TEST] " << name << " ... "; \
        test_count++; \
    } while (0)

#define PASS() \
    do { \
        std::cout << "PASS" << std::endl; \
        test_passed++; \
    } while (0)

#define FAIL(msg) \
    do { \
        std::cout << "FAIL: " << msg << std::endl; \
    } while (0)

#define ASSERT_TRUE(cond, msg) \
    do { \
        if (!(cond)) { FAIL(msg); return; } \
    } while (0)

#define ASSERT_EQ(a, b, msg) \
    do { \
        if ((a) != (b)) { FAIL(msg << " (expected=" << (b) << ", got=" << (a) << ")"); return; } \
    } while (0)

// 加载 BFS order（旧ID->新ID 和 新ID->旧ID）
struct BfsOrder {
    std::vector<uint32_t> old_to_new;  // old_to_new[old_id] = new_id
    std::vector<uint32_t> new_to_old;  // new_to_old[new_id] = old_id
    uint32_t num_nodes;
    uint32_t entry_point;  // 原始 entry_point (old ID)
};

BfsOrder loadBfsOrder(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open BFS order file: " + path);
    }

    BfsHeader hdr;
    in.read(reinterpret_cast<char*>(&hdr), sizeof(BfsHeader));
    if (hdr.magic != MAGIC_BFS) {
        throw std::runtime_error("Invalid BFS order file magic");
    }

    BfsOrder order;
    order.num_nodes = hdr.num_nodes;
    order.entry_point = hdr.entry_point;
    order.old_to_new.resize(order.num_nodes);
    order.new_to_old.resize(order.num_nodes);
    in.read(reinterpret_cast<char*>(order.old_to_new.data()),
            order.num_nodes * sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(order.new_to_old.data()),
            order.num_nodes * sizeof(uint32_t));
    in.close();
    return order;
}

// ============================================================
// 测试用例
// ============================================================

void test_basic_loading(const std::string& output_dir) {
    TEST("Basic Loading - BlockCache construction");

    std::string blocks_path = output_dir + "/test_blocks.bin";
    std::string route_path = output_dir + "/test_route.bin";

    BlockCache cache(blocks_path, route_path, 64, 128);

    ASSERT_EQ(cache.getNumBlocks(), 3u, "Expected 3 blocks for 1K nodes");
    ASSERT_EQ(cache.getNumNodes(), 1000u, "Expected 1000 nodes in route table");
    ASSERT_EQ(cache.getBlockSize(), (uint32_t)(256 * 1024), "Expected 256KB block size");
    ASSERT_EQ(cache.getCacheSlots(), (size_t)64, "Expected 64 cache slots");
    ASSERT_EQ(cache.getNumCachedBlocks(), (size_t)0, "Expected 0 cached blocks initially");

    PASS();
}

void test_cache_hit_miss(const std::string& output_dir) {
    TEST("Cache Hit/Miss - First access miss, second access hit");

    std::string blocks_path = output_dir + "/test_blocks.bin";
    std::string route_path = output_dir + "/test_route.bin";

    BlockCache cache(blocks_path, route_path, 64, 128);

    // 第一次访问节点 0 -> should be miss
    cache.resetStats();
    const float* vec1 = cache.getNodeVector(0);
    ASSERT_TRUE(vec1 != nullptr, "getNodeVector(0) returned nullptr");

    auto& stats1 = cache.getStats();
    ASSERT_EQ(stats1.total_accesses.load(), (size_t)1, "Expected 1 total access");
    ASSERT_EQ(stats1.cache_misses.load(), (size_t)1, "Expected 1 cache miss");
    ASSERT_EQ(stats1.cache_hits.load(), (size_t)0, "Expected 0 cache hits");

    // 第二次访问同一个节点 -> should be hit
    const float* vec2 = cache.getNodeVector(0);
    ASSERT_TRUE(vec2 != nullptr, "getNodeVector(0) second call returned nullptr");
    ASSERT_TRUE(vec1 == vec2, "Vector pointers should be the same (same cached block)");

    auto& stats2 = cache.getStats();
    ASSERT_EQ(stats2.total_accesses.load(), (size_t)2, "Expected 2 total accesses");
    ASSERT_EQ(stats2.cache_misses.load(), (size_t)1, "Expected still 1 cache miss");
    ASSERT_EQ(stats2.cache_hits.load(), (size_t)1, "Expected 1 cache hit");

    PASS();
}

void test_lru_eviction(const std::string& output_dir) {
    TEST("LRU Eviction - cache_slots=1, access two different blocks");

    std::string blocks_path = output_dir + "/test_blocks.bin";
    std::string route_path = output_dir + "/test_route.bin";

    // 只给 1 个缓存槽位
    BlockCache cache(blocks_path, route_path, 1, 128);

    cache.resetStats();

    // 访问 Block 0 中的节点（node 0 在 block 0 中）
    uint32_t block_id_0 = cache.getBlockId(0);
    const float* vec0 = cache.getNodeVector(0);
    ASSERT_TRUE(vec0 != nullptr, "getNodeVector(0) returned nullptr");
    ASSERT_EQ(cache.getNumCachedBlocks(), (size_t)1, "Expected 1 cached block");
    ASSERT_EQ(cache.getStats().cache_misses.load(), (size_t)1, "Expected 1 miss");
    ASSERT_EQ(cache.getStats().evictions.load(), (size_t)0, "Expected 0 evictions");

    // 找一个在不同 Block 中的节点
    // test 数据有 3 个 blocks: block 0 (nodes 0-435), block 1 (436-881), block 2 (882-999)
    uint32_t node_in_block_1 = 500;  // 应该在 block 1
    uint32_t block_id_1 = cache.getBlockId(node_in_block_1);
    ASSERT_TRUE(block_id_0 != block_id_1, "Node 0 and node 500 should be in different blocks");

    // 访问 Block 1 中的节点 -> 应该淘汰 Block 0
    const float* vec1 = cache.getNodeVector(node_in_block_1);
    ASSERT_TRUE(vec1 != nullptr, "getNodeVector(500) returned nullptr");
    ASSERT_EQ(cache.getNumCachedBlocks(), (size_t)1, "Expected still 1 cached block (evicted block 0)");
    ASSERT_EQ(cache.getStats().cache_misses.load(), (size_t)2, "Expected 2 misses");
    ASSERT_EQ(cache.getStats().evictions.load(), (size_t)1, "Expected 1 eviction");

    // 再次访问 Block 0 中的节点 -> 应该淘汰 Block 1
    const float* vec0_again = cache.getNodeVector(0);
    ASSERT_TRUE(vec0_again != nullptr, "getNodeVector(0) after eviction returned nullptr");
    ASSERT_EQ(cache.getStats().evictions.load(), (size_t)2, "Expected 2 evictions");

    PASS();
}

void test_data_correctness(const std::string& output_dir) {
    TEST("Data Correctness - Compare BlockCache data with graph.bin");

    // 加载原始图结构
    std::string graph_path = output_dir + "/test_graph.bin";
    GraphStructure g = load_graph_structure(graph_path);

    // 加载 BFS order
    std::string bfs_path = output_dir + "/test_bfs.bin";
    BfsOrder bfs = loadBfsOrder(bfs_path);

    // 构造 BlockCache
    std::string blocks_path = output_dir + "/test_blocks.bin";
    std::string route_path = output_dir + "/test_route.bin";
    BlockCache cache(blocks_path, route_path, 64, 128);

    // 验证所有节点的向量数据和邻居列表
    int vector_errors = 0;
    int neighbor_errors = 0;
    int nodes_checked = 0;

    for (uint32_t new_id = 0; new_id < bfs.num_nodes; new_id++) {
        uint32_t old_id = bfs.new_to_old[new_id];

        // ---- 验证向量 ----
        const float* cached_vec = cache.getNodeVector(new_id);
        ASSERT_TRUE(cached_vec != nullptr, "getNodeVector returned nullptr for node " + std::to_string(new_id));

        const float* original_vec = &g.vectors[(size_t)old_id * g.dim];
        for (uint32_t d = 0; d < g.dim; d++) {
            if (std::fabs(cached_vec[d] - original_vec[d]) > 1e-6f) {
                if (vector_errors < 5) {
                    std::cerr << "  Vector mismatch at node " << new_id
                              << " (old_id=" << old_id << "), dim " << d
                              << ": cached=" << cached_vec[d]
                              << ", original=" << original_vec[d] << std::endl;
                }
                vector_errors++;
                break;
            }
        }

        // ---- 验证邻居列表 ----
        uint32_t cached_neighbor_count = 0;
        const uint32_t* cached_neighbors = cache.getNodeNeighbors(new_id, cached_neighbor_count);

        // 原始邻居列表（需要映射为新 ID）
        const auto& orig_neighbors = g.adjacency0[old_id];
        ASSERT_TRUE(cached_neighbors != nullptr, "getNodeNeighbors returned nullptr for node " + std::to_string(new_id));

        // 邻居数量应一致
        if (cached_neighbor_count != orig_neighbors.size()) {
            if (neighbor_errors < 5) {
                std::cerr << "  Neighbor count mismatch at node " << new_id
                          << " (old_id=" << old_id << "): cached=" << cached_neighbor_count
                          << ", original=" << orig_neighbors.size() << std::endl;
            }
            neighbor_errors++;
            continue;
        }

        // 逐个比较邻居（原始邻居是 old_id，需要映射为 new_id）
        bool neighbor_ok = true;
        for (uint32_t k = 0; k < cached_neighbor_count; k++) {
            uint32_t expected_new_id = bfs.old_to_new[orig_neighbors[k]];
            if (cached_neighbors[k] != expected_new_id) {
                if (neighbor_errors < 5) {
                    std::cerr << "  Neighbor mismatch at node " << new_id
                              << " (old_id=" << old_id << "), neighbor " << k
                              << ": cached=" << cached_neighbors[k]
                              << ", expected(new_id)=" << expected_new_id
                              << " (old_id=" << orig_neighbors[k] << ")" << std::endl;
                }
                neighbor_ok = false;
                break;
            }
        }
        if (!neighbor_ok) {
            neighbor_errors++;
        }

        nodes_checked++;
    }

    ASSERT_EQ(vector_errors, 0, "Vector data mismatch in " << vector_errors << " nodes");
    ASSERT_EQ(neighbor_errors, 0, "Neighbor data mismatch in " << neighbor_errors << " nodes");
    ASSERT_EQ(nodes_checked, (int)bfs.num_nodes, "Not all nodes checked");

    std::cout << "(" << nodes_checked << " nodes verified) ";
    PASS();
}

void test_stats_and_prefetch(const std::string& output_dir) {
    TEST("Stats and Prefetch - Verify statistics and prefetch interface");

    std::string blocks_path = output_dir + "/test_blocks.bin";
    std::string route_path = output_dir + "/test_route.bin";

    BlockCache cache(blocks_path, route_path, 64, 128);
    cache.resetStats();

    // 预取 Block 0
    bool ok = cache.prefetchBlock(0);
    ASSERT_TRUE(ok, "prefetchBlock(0) failed");

    // 预取后访问 Block 0 中的节点应该命中
    const float* vec = cache.getNodeVector(0);
    ASSERT_TRUE(vec != nullptr, "getNodeVector(0) after prefetch returned nullptr");
    ASSERT_EQ(cache.getStats().cache_hits.load(), (size_t)1, "Expected 1 hit after prefetch");
    ASSERT_EQ(cache.getStats().cache_misses.load(), (size_t)0, "Expected 0 misses after prefetch");

    // 验证命中率
    double rate = cache.hitRate();
    ASSERT_TRUE(rate == 1.0, "Expected 100% hit rate, got " << rate);

    // 预取不存在的 Block
    bool ok2 = cache.prefetchBlock(9999);
    ASSERT_TRUE(!ok2, "prefetchBlock(9999) should fail");

    // 验证 getBlockId
    uint32_t bid = cache.getBlockId(0);
    ASSERT_EQ(bid, 0u, "Node 0 should be in block 0");

    // 验证 getBlockById
    CachedBlock* block = cache.getBlockById(0);
    ASSERT_TRUE(block != nullptr, "getBlockById(0) returned nullptr");
    ASSERT_EQ(block->block_id, 0u, "Block ID mismatch");
    ASSERT_TRUE(block->node_count > 0, "Block should have nodes");

    PASS();
}

void test_all_blocks_accessible(const std::string& output_dir) {
    TEST("All Blocks Accessible - Access nodes in every block");

    std::string blocks_path = output_dir + "/test_blocks.bin";
    std::string route_path = output_dir + "/test_route.bin";

    BlockCache cache(blocks_path, route_path, 64, 128);
    cache.resetStats();

    // 访问每个 Block 中的至少一个节点
    for (uint32_t b = 0; b < cache.getNumBlocks(); b++) {
        CachedBlock* block = cache.getBlockById(b);
        ASSERT_TRUE(block != nullptr, "getBlockById(" + std::to_string(b) + ") returned nullptr");
        ASSERT_EQ(block->block_id, b, "Block ID mismatch");
        ASSERT_TRUE(block->node_count > 0, "Block " + std::to_string(b) + " has 0 nodes");
    }

    // 3 个 block，应该 3 次 miss
    ASSERT_EQ(cache.getStats().cache_misses.load(), (size_t)3, "Expected 3 misses");
    ASSERT_EQ(cache.getStats().cache_hits.load(), (size_t)0, "Expected 0 hits (first access to each)");

    PASS();
}

// ============================================================
// 主函数
// ============================================================

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <output_dir>" << std::endl;
        std::cerr << "  output_dir should contain: test_graph.bin, test_blocks.bin, test_route.bin, test_bfs.bin" << std::endl;
        return 1;
    }

    std::string output_dir = argv[1];
    std::cout << "========================================" << std::endl;
    std::cout << "BlockCache Unit Tests" << std::endl;
    std::cout << "  Output dir: " << output_dir << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        test_basic_loading(output_dir);
        test_cache_hit_miss(output_dir);
        test_lru_eviction(output_dir);
        test_data_correctness(output_dir);
        test_stats_and_prefetch(output_dir);
        test_all_blocks_accessible(output_dir);
    } catch (const std::exception& e) {
        std::cerr << "\nFATAL ERROR: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << test_passed << "/" << test_count << " passed" << std::endl;
    if (test_passed == test_count) {
        std::cout << "ALL TESTS PASSED ✅" << std::endl;
    } else {
        std::cout << (test_count - test_passed) << " TESTS FAILED ❌" << std::endl;
    }
    std::cout << "========================================" << std::endl;

    return (test_passed == test_count) ? 0 : 1;
}
