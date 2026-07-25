// block_cache.h - BlockCache 管理器：管理 DRAM 中的热块缓存
//
// 功能：
//   1. 从磁盘文件按需加载 Block（pread）
//   2. 可插拔替换策略（LRU / LFU / LRU-K）
//   3. 可插拔布局编排器（BFS / Random / 自定义）
//   4. Block 内存格式展开（磁盘紧凑格式 -> 可访问的节点结构）
//   5. 线程安全（std::mutex 粗粒度锁）
//   6. 支持 O_DIRECT / page cache 清除 / 模拟延迟
//
// 设计文档: hnsw-research/phase2-design.md

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>

#include "common.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include "block_heat_evaluator.h"

// ============================================================
// IOConfig: I/O 模式配置
// ============================================================

struct IOConfig {
    bool use_odirect = false;          // 使用 O_DIRECT 打开文件
    bool drop_page_cache = false;      // pread 后用 posix_fadvise 清除 page cache
    double simulated_latency_us = 0.0; // 模拟磁盘延迟（微秒），0 = 不模拟
    bool use_mmap = false;             // 使用 mmap 映射 blocks 文件

    // 获取模式名称
    std::string modeName() const {
        if (use_mmap) return "mmap";
        if (use_odirect) return "direct";
        if (simulated_latency_us > 0) return "simulated";
        return "cached";
    }
};

// ============================================================
// CachedBlock: 内存中展开的 Block
// ============================================================

// 缓存中的单个节点信息（指针指向 raw_data 内部，不拥有内存）
struct CachedNode {
    uint32_t node_id;              // 全局节点 ID（BFS 重排后的新 ID）
    const float* vector;           // 指向 raw_data 中的向量数据
    uint32_t neighbor_count;       // 邻居数量
    const uint32_t* neighbors;     // 指向 raw_data 中的邻居列表
};

// 缓存中的 Block：包含原始数据和展开后的索引
struct CachedBlock {
    uint32_t block_id = 0;         // Block ID
    uint32_t node_count = 0;       // Block 内节点数
    uint32_t dim = 0;              // 向量维度
    uint32_t first_node_id = 0;    // Block 内第一个 node_id（BFS 重排保证连续）

    // ---- 预取准确率度量（纯观测，不影响搜索/recall）----
    bool was_prefetched = false;   // 是否经预取路径插入（vs 按需 miss 加载）
    bool was_accessed = false;     // 在缓存期间是否被搜索真正访问过

    // 原始磁盘数据（保持不释放，vectors 指针指向这里）
    std::vector<uint8_t> raw_data;

    // 解码后的邻居列表存储（delta+varint 解码后存这里）
    // CachedNode.neighbors 指向此 buffer
    std::vector<uint32_t> neighbor_pool;

    // 展开后的节点索引
    std::vector<CachedNode> nodes;

    // 获取指定全局节点 ID 的向量指针
    // 返回 nullptr 如果节点不在此 Block 中
    // 优化：BFS 重排保证 block 内 node_id 连续，用减法替代 hash map
    const float* getVector(uint32_t node_id) const {
        uint32_t local = node_id - first_node_id;
        if (local >= node_count) return nullptr;
        return nodes[local].vector;
    }

    // 获取指定全局节点 ID 的邻居列表
    // 返回 nullptr 如果节点不在此 Block 中
    // out_count 输出邻居数量
    const uint32_t* getNeighbors(uint32_t node_id, uint32_t& out_count) const {
        uint32_t local = node_id - first_node_id;
        if (local >= node_count) return nullptr;
        out_count = nodes[local].neighbor_count;
        return nodes[local].neighbors;
    }

    // 检查 node_id 是否在此 Block 中
    bool containsNode(uint32_t node_id) const {
        uint32_t local = node_id - first_node_id;
        return local < node_count;
    }
};

// ============================================================
// BlockCache: 块缓存管理器（可插拔设计）
// ============================================================

class BlockCache {
public:
    // 统计信息
    struct Stats {
        std::atomic<size_t> total_accesses{0};   // 总访问次数
        std::atomic<size_t> cache_hits{0};       // 缓存命中次数
        std::atomic<size_t> cache_misses{0};     // 缓存未命中次数
        std::atomic<size_t> evictions{0};        // 淘汰次数
        std::atomic<size_t> disk_reads{0};       // 磁盘读取次数
        // ---- 预取准确率度量 ----
        std::atomic<size_t> prefetch_useful{0};  // 预取块淘汰/结束时已被访问
        std::atomic<size_t> prefetch_wasted{0};  // 预取块淘汰/结束时从未被访问
    };

    // ---- 新构造函数（可插拔接口）----

    // 构造函数：接受 LayoutProvider 和 ReplacementPolicy
    // blocks_path:  blocks.bin 文件路径
    // layout:       布局编排器（BFS / Random / 自定义）
    // policy:       替换策略（LRU / LFU / LRU-K）
    // cache_slots:  最大缓存槽位数（默认 64，约 16MB DRAM）
    // dim:          向量维度（默认 128，SIFT1M）
    // io_config:    I/O 模式配置
    BlockCache(const std::string& blocks_path,
               std::unique_ptr<LayoutProvider> layout,
               std::unique_ptr<ReplacementPolicy> policy = std::make_unique<LRUPolicy>(),
               size_t cache_slots = 64,
               uint32_t dim = 128,
               IOConfig io_config = {});

    // ---- 向后兼容构造函数 ----
    // 从 route_path 加载 BfsLayoutProvider，使用默认 LRUPolicy
    BlockCache(const std::string& blocks_path,
               const std::string& route_path,
               size_t cache_slots = 64,
               uint32_t dim = 128,
               IOConfig io_config = {});

    ~BlockCache();

    // 禁止拷贝和赋值（持有文件描述符和互斥锁）
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    // ---- 节点级访问接口 ----

    // 获取节点的向量指针
    // 如果 Block 不在缓存中，触发按需加载
    // 返回 nullptr 表示节点不存在或加载失败
    const float* getNodeVector(uint32_t node_id);

    // 获取节点的邻居列表
    // out_count 输出邻居数量
    // 返回 nullptr 表示节点不存在或加载失败
    const uint32_t* getNodeNeighbors(uint32_t node_id, uint32_t& out_count);

    // ---- Block 级访问接口 ----

    // 获取包含指定节点的 CachedBlock
    // 如果 Block 不在缓存中，触发按需加载
    // 返回 nullptr 表示加载失败
    CachedBlock* getBlockByNodeId(uint32_t node_id);

    // 通过 Block ID 获取 CachedBlock
    // 用于预取等场景
    CachedBlock* getBlockById(uint32_t block_id);

    // 预取 Block（当前阶段同步实现，后续阶段改为异步）
    // 返回 true 表示成功加载到缓存
    bool prefetchBlock(uint32_t block_id);

    // ---- Phase 3: 预取支持接口 ----

    // 检查 Block 是否已在缓存中（不加锁，线程安全读取）
    bool isInCache(uint32_t block_id) const;

    // 批量检查 blocks 是否在缓存中（一次加锁）
    // 返回不在缓存中的 block_id 列表
    std::vector<uint32_t> filterNotInCache(const std::vector<uint32_t>& block_ids) const;

    // 尝试预取 Block（线程安全，用于后台预取线程）
    // 如果 block 已在缓存，返回 true（无需加载）
    // 如果 block 不在缓存，加载到缓存并返回 true，失败返回 false
    bool tryPrefetch(uint32_t block_id);

    // 时效性实验: 只读穚探缓存中的 block, 不改任何统计/LRU (避免污染命中率对比)
    // 不在缓存返回 nullptr, 不触发磁盘加载
    CachedBlock* peekCachedBlockById(uint32_t block_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_map_.find(block_id);
        return it != cache_map_.end() ? &it->second : nullptr;
    }

    // ---- Phase 3 Redesign: io_uring 预取支持 ----

    // 从外部预加载的数据插入缓存（io_uring 完成后调用）
    // block_id:   Block ID
    // raw_data:   原始磁盘数据（调用后所有权转移给缓存）
    // data_size:  数据大小（应等于 block_size_）
    // 返回 true 表示成功插入
    bool insertBlock(uint32_t block_id, std::vector<uint8_t>&& raw_data, size_t data_size);

    // 零拷贝插入：直接从对齐缓冲区指针插入
    // 避免 processCompletion 中的临时 vector 分配 + memcpy
    // 内部会分配 raw_data 并做一次拷贝（无法避免，因为 CachedBlock 需要 vector）
    bool insertBlockFromPtr(uint32_t block_id, const void* data, size_t data_size);

    // ---- Phase 3 CPU Opt: 批量插入 ----

    // 批量插入条目
    struct BatchEntry {
        uint32_t block_id;
        const void* data;
        size_t data_size;
    };

    // 批量插入：在锁外解析所有 block，然后一次加锁插入所有 block
    // 减少 N 次加锁到 1 次
    bool insertBlocksBatch(const std::vector<BatchEntry>& entries);

    // 获取已在缓存中的 Block（不加锁磁盘加载，miss 返回 nullptr）
    // 用于 searchLayer0 中快速访问 in-cache block
    CachedBlock* getCachedBlockById(uint32_t block_id);

    // 获取 Block 文件描述符（io_uring 读取用）
    int getBlocksFd() const { return blocks_fd_; }

    // 获取 Block 大小（字节）
    uint32_t getBlockSizeBytes() const { return block_size_; }

    // 获取 Blocks 文件头部大小（用于计算偏移，O_DIRECT 对齐）
    static constexpr size_t getHeaderSize() { return BLOCKS_FILE_HEADER_SIZE; }

    // 获取当前缓存中 block 数量（不加锁，用于快速检查）
    size_t getNumCachedBlocksUnsafe() const {
        return cache_map_.size();
    }

    // 主动释放 blocks 文件的 page cache (posix_fadvise DONTNEED)
    // 在每次查询后调用，防止 page cache 无限增长
    void dropPageCache();

    // 获取最近访问的 Block ID（用于预测器推理）
    // 返回最近 N 个被加载的 block_id（按时间顺序）
    std::vector<uint32_t> getRecentBlockAccesses(size_t n = 10) const;

    // ---- Phase 3: 轨迹记录回调 ----
    using TraceCallback = std::function<void(uint32_t block_id, bool is_hit)>;
    friend class DiskHNSW;

    // 设置轨迹回调（每次 block 访问时调用，hit 或 miss）
    void setTraceCallback(TraceCallback cb) { trace_cb_ = std::move(cb); }
    void clearTraceCallback() { trace_cb_ = nullptr; }

    // ---- 路由查询 ----

    // 获取节点所在的 Block ID（通过布局编排器）
    // 不触发缓存加载，仅查询路由
    uint32_t getBlockId(uint32_t node_id) const;

    // 获取路由表条目数（= 节点总数）
    uint32_t getNumNodes() const;

    // 获取 Block 总数
    uint32_t getNumBlocks() const { return num_blocks_; }

    // ---- 统计信息 ----

    const Stats& getStats() const { return stats_; }
    void resetStats();
    double hitRate() const;

    // ---- 配置信息 ----

    size_t getCacheSlots() const { return cache_slots_; }
    size_t getNumCachedBlocks() const;
    uint32_t getBlockSize() const { return block_size_; }

    // 获取布局和策略信息
    const std::string& getLayoutName() const { return layout_name_; }
    const std::string& getPolicyName() const { return policy_name_; }

    // 设置热度评价器 (用于热度加权淘汰)
    void setHeatEvaluator(BlockHeatEvaluator* eval) { heat_evaluator_ = eval; }
    const IOConfig& getIOConfig() const { return io_config_; }

    // ---- Flat Cache: 快速路径统计 ----
    struct FlatStats {
        std::atomic<size_t> vec_hits{0};       // 向量快速路径命中
        std::atomic<size_t> vec_misses{0};     // 向量快速路径未命中
        std::atomic<size_t> block_hits{0};     // block指针快速路径命中
        std::atomic<size_t> block_misses{0};   // block指针快速路径未命中
    };
    const FlatStats& getFlatStats() const { return flat_stats_; }

private:
    // ---- 磁盘 I/O ----
    int blocks_fd_;                 // blocks.bin 文件描述符
    uint32_t block_size_;           // Block 固定大小（字节）
    uint32_t num_blocks_;           // Block 总数

    // ---- 可插拔布局编排器 ----
    std::unique_ptr<LayoutProvider> layout_;
    std::string layout_name_;

    // ---- 可插拔替换策略 ----
    std::unique_ptr<ReplacementPolicy> policy_;

    // 热度评价器 (可选, 由 DiskHNSW 设置)
    BlockHeatEvaluator* heat_evaluator_ = nullptr;
    std::string policy_name_;

    // ---- I/O 配置 ----
    IOConfig io_config_;
    void* aligned_buffer_ = nullptr;  // O_DIRECT 用的对齐缓冲区
    size_t aligned_buffer_size_ = 0;

    // ---- mmap 模式 ----
    void* mmap_ptr_ = nullptr;        // mmap 映射起始地址
    size_t mmap_size_ = 0;            // mmap 映射大小

    // ---- 缓存配置 ----
    size_t cache_slots_;            // 最大缓存槽位数
    uint32_t dim_;                  // 向量维度

    // ---- 缓存存储 ----
    // block_id -> CachedBlock
    std::unordered_map<uint32_t, CachedBlock> cache_map_;

    // ---- 线程安全 ----
    mutable std::mutex mutex_;

    // ---- 统计 ----
    Stats stats_;

    // ---- Flat Cache 统计 ----
    FlatStats flat_stats_;

    // ---- Phase 3: 访问历史记录 ----
    std::vector<uint32_t> recent_accesses_;  // 最近访问的 block_id 序列
    static constexpr size_t MAX_RECENT_ACCESSES = 1024;

    // ---- Phase 3: 轨迹回调 ----
    TraceCallback trace_cb_;
    friend class DiskHNSW;

    // ---- Flat Vector Cache (lock-free fast path) ----
    // 直接映射缓存，按 node_id % num_slots 散列
    // 存储向量数据，无需 mutex
    float* flat_vec_data_ = nullptr;           // num_slots * dim_ floats
    uint32_t* flat_vec_owners_ = nullptr;       // num_slots uint32_t (UINT32_MAX = empty)
    size_t flat_vec_num_slots_ = 0;             // slot 数量

    // ---- Flat Block Pointer Cache (lock-free fast path) ----
    // 直接映射缓存，按 block_id % num_slots 散列
    // 存储 CachedBlock* 指针，无需 mutex
    CachedBlock** flat_block_ptrs_ = nullptr;   // num_slots CachedBlock* 指针
    uint32_t* flat_block_owners_ = nullptr;     // num_slots uint32_t (UINT32_MAX = empty)
    size_t flat_block_num_slots_ = 0;           // slot 数量

    // ---- Flat Cache 内部方法 ----
    void initFlatCache(size_t cache_bytes);
    void populateFlatCache(const CachedBlock& block);
    void invalidateFlatBlockCache(uint32_t block_id);
    void clearFlatCache();

    // ---- 内部方法 ----

    // 从磁盘加载 Block（不加锁，调用者负责加锁）
    // 返回加载好的 CachedBlock，失败时抛出异常
    CachedBlock loadBlockFromDisk(uint32_t block_id);

    // 淘汰一个 Block（通过替换策略选择 victim）
    // 返回 true 表示成功淘汰，false 表示缓存为空或策略不允许
    bool evictOne();

    // 解析 Block 原始数据，构建 CachedBlock 的索引结构
    void parseBlock(CachedBlock& block);

    // 初始化对齐缓冲区（O_DIRECT 用）
    void initAlignedBuffer();

    // 模拟磁盘延迟
    void simulateLatency();
};
