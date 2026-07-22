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
#include "predictor.h"
#include "prefetcher.h"

#include <vector>
#include <string>
#include <queue>
#include <utility>
#include <memory>
#include <mutex>
#include <atomic>

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

    // ---- Phase 3: 预取支持 ----

    // 启用预取（加载 Markov 模型）
    void enablePrefetch(const std::string& model_path);

    // 禁用预取
    void disablePrefetch();

    // 是否已启用预取
    bool isPrefetchEnabled() const { return prefetcher_ != nullptr; }

    // 获取预取统计
    const Prefetcher::Stats& getPrefetchStats() const;

    // 重置预取统计
    void resetPrefetchStats();

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

    // ---- Phase 3: 预测器 + 预取器 ----
    std::unique_ptr<MarkovPredictor> predictor_;
    std::unique_ptr<Prefetcher> prefetcher_;
    uint32_t last_accessed_block_ = UINT32_MAX;  // 上次访问的 block_id

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
