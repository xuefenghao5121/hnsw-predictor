// disk_hnsw.h - DiskHNSW: 基于BlockCache的按需加载HNSW检索
//
// 设计要点：
// 1. 顶层（Layer 1+）常驻DRAM，从graph_structure.bin加载
// 2. Layer 0 通过BlockCache按需加载，使用BFS重排后的new_id
// 3. ID映射：old_id (hnswlib内部ID) <-> new_id (BFS重排ID)
// 4. 搜索流程：贪心下降（内存） -> ef_search（BlockCache按需加载）
// 5. 搜索结果recall必须与全内存HNSW一致
//
// 设计文档: hnsw-research/phase2-design.md

#pragma once

#include "common.h"
#include "block_cache.h"
#include "layout_provider.h"
#include "replacement_policy.h"
#include "graph_prefetcher.h"

#include <vector>
#include <string>
#include <queue>
#include <utility>
#include <memory>
#include <mutex>
#include <atomic>
#include <set>
#include <fcntl.h>

// ============================================================
// VisitedList: 简单的访问标记数组（不需要池化，每次搜索创建一个）
// ============================================================
struct VisitedList {
    std::vector<uint32_t> mass;  // 使用uint32_t作为标记类型
    uint32_t curV;               // 当前标记值

    explicit VisitedList(size_t num_elements)
        : mass(num_elements, 0), curV(1) {}

    void reset() {
        curV++;
        if (curV == 0) {  // 溢出，清空
            std::fill(mass.begin(), mass.end(), 0);
            curV = 1;
        }
    }

    inline bool isVisited(uint32_t id) const {
        return mass[id] == curV;
    }

    inline void markVisited(uint32_t id) {
        mass[id] = curV;
    }
};

// ============================================================
// DiskHNSW: 磁盘驻留HNSW检索器
// ============================================================
class DiskHNSW {
public:
    // 搜索结果类型: (distance, label)
    using SearchResult = std::pair<float, uint64_t>;

    // 构造函数（原始接口，向后兼容）
    // graph_path:  graph_structure.bin 路径
    // bfs_path:    bfs_order.bin 路径
    // blocks_path: blocks.bin 路径
    // route_path:  route_table.bin 路径
    // cache_slots: BlockCache最大缓存槽位数
    // dim:         向量维度
    DiskHNSW(const std::string& graph_path,
             const std::string& bfs_path,
             const std::string& blocks_path,
             const std::string& route_path,
             size_t cache_slots = 64,
             uint32_t dim = 128);

    // 构造函数（可插拔接口，接受外部构造的 BlockCache）
    // graph_path:  graph_structure.bin 路径
    // bfs_path:    bfs_order.bin 路径
    // cache:       外部构造的 BlockCache（使用可插拔 LayoutProvider + ReplacementPolicy）
    DiskHNSW(const std::string& graph_path,
             const std::string& bfs_path,
             std::unique_ptr<BlockCache> cache);

    ~DiskHNSW() = default;

    // 设置 ef_search 参数
    void setEf(size_t ef) { ef_search_ = ef; }
    size_t getEf() const { return ef_search_; }

    // KNN搜索
    // 返回按距离排序的 top-k 结果 (距离从小到大)
    std::vector<SearchResult> searchKnn(const float* query, size_t k);

    // 获取缓存统计信息
    const BlockCache::Stats& getCacheStats() const { return cache_->getStats(); }

    // 重置缓存统计
    void resetCacheStats() { cache_->resetStats(); }

    // 释放 blocks 文件的 page cache (每次查询后调用)
    void dropPageCache() { cache_->dropPageCache(); }

    // ---- Phase 3a: 查询间预取 ----

    // 记录当前查询访问的所有 unique block IDs
    // 在搜索过程中通过 trace callback 自动收集
    void startRecordingBlocks() { recorded_blocks_.clear(); recording_ = true; }
    void stopRecordingBlocks() { recording_ = false; }
    const std::set<uint32_t>& getRecordedBlocks() const { return recorded_blocks_; }

    // 预取最近 N 个查询的 Block 并集
    // 在下一个查询开始前调用
    // 方式: posix_fadvise(WILLNEED) 预热 page cache, 不进 BlockCache
    // 返回预取的 block 数
    size_t prefetchRecentBlocks(const std::vector<std::set<uint32_t>>& recent_sets, size_t n) {
        std::set<uint32_t> union_set;
        size_t start = recent_sets.size() > n ? recent_sets.size() - n : 0;
        for (size_t i = start; i < recent_sets.size(); i++) {
            union_set.insert(recent_sets[i].begin(), recent_sets[i].end());
        }

        size_t loaded = 0;
        int fd = cache_->getBlocksFd();
        size_t header = cache_->getHeaderSize();
        uint32_t bs = cache_->getBlockSizeBytes();
        for (uint32_t block_id : union_set) {
            // 用 fadvise(WILLNEED) 预热 page cache, 不污染 BlockCache
            off_t offset = (off_t)header + (off_t)block_id * bs;
            posix_fadvise(fd, offset, bs, POSIX_FADV_WILLNEED);
            loaded++;
        }
        return loaded;
    }

    // 获取图信息
    uint32_t getNumNodes() const { return graph_.num_nodes; }
    uint32_t getDim() const { return dim_; }
    int32_t getMaxLevel() const { return graph_.max_level; }
    uint32_t getEntryPoint() const { return graph_.entry_point; }

    // 获取缓存信息
    size_t getNumCachedBlocks() const { return cache_->getNumCachedBlocks(); }
    size_t getCacheSlots() const { return cache_->getCacheSlots(); }

    // ID转换工具
    uint32_t oldToNew(uint32_t old_id) const { return old_to_new_[old_id]; }
    uint32_t newToOld(uint32_t new_id) const { return new_to_old_[new_id]; }

    // ---- Phase 3 Redesign: 图引导预取支持 ----

    // 启用图引导预取（io_uring 异步批量预取）
    // use_odirect: 是否使用 O_DIRECT 模式
    void enableGraphPrefetch(bool use_odirect = true);

    // 禁用图引导预取
    void disableGraphPrefetch();

    // 是否已启用图引导预取
    bool isGraphPrefetchEnabled() const { return graph_prefetch_enabled_; }

    // 获取图引导预取统计
    const GraphPrefetcher::Stats& getGraphPrefetchStats() const;

    // 重置图引导预取统计
    void resetGraphPrefetchStats();

    // ---- Phase 3: 轨迹采集 ----
    void setTraceCallback(std::function<void(uint32_t, bool)> cb) { cache_->setTraceCallback(std::move(cb)); }
    void clearTraceCallback() { cache_->clearTraceCallback(); }

private:
    // ---- 图数据（常驻内存，old_id空间）----
    GraphStructure graph_;          // 从graph_structure.bin加载
    uint32_t dim_;                  // 向量维度
    size_t ef_search_;              // ef参数

    // ---- BFS映射 ----
    std::vector<uint32_t> old_to_new_;  // old_id -> new_id
    std::vector<uint32_t> new_to_old_;  // new_id -> old_id (bfs_order)

    // ---- BlockCache（new_id空间）----
    std::unique_ptr<BlockCache> cache_;

    // 缓存配置信息（从 cache_ 获取后保存，用于日志）
    size_t cache_slots_ = 0;

    // ---- Phase 3 Redesign: 图引导预取器 ----
    std::unique_ptr<GraphPrefetcher> graph_prefetcher_;
    bool graph_prefetch_enabled_ = false;

    // ---- Phase 3 CPU Opt: 路由表缓存 ----
    // 直接引用路由表，避免每次 getBlockId 的虚函数调用
    const std::vector<uint32_t>* route_table_ = nullptr;

    // ---- Phase 3a: 查询间预取 ----
    bool recording_ = false;
    std::set<uint32_t> recorded_blocks_;

    // ---- 访问标记 ----
    // 每次搜索创建新的VisitedList，不需要线程安全
    // 使用new_id空间标记

    // ---- 距离计算 ----
    // L2距离，使用hnswlib的优化实现
    size_t dim_param_;                  // 用于dist_func_param
    float l2Distance(const float* a, const float* b) const;

    // ---- 搜索内部方法 ----

    // 贪心下降：从enterpoint通过上层到达Layer 0的入口点
    // 使用内存中的图数据（old_id空间）
    // 返回: Layer 0入口点的old_id
    uint32_t greedyDescent(const float* query);

    // Layer 0 搜索（BlockCache按需加载，new_id空间）
    // entry_new_id: Layer 0入口点的new_id
    // 返回: top_candidates (distance, new_id)
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>>
    searchLayer0(uint32_t entry_new_id, const float* query, size_t ef,
                 VisitedList& visited);
};
