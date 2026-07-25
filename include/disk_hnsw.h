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
#include "block_heat_evaluator.h"
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
#include <functional>
#include <fcntl.h>

// ============================================================
// PQ (Product Quantization) 支持
// ============================================================

struct PQParams {
    uint32_t M = 0;        // 子量化器数
    uint32_t nbits = 0;    // 每个子量化器位数
    uint32_t dim = 0;      // 原始向量维度
    uint32_t dsub = 0;     // 子向量维度 = dim / M
    uint32_t ksub = 0;     // 每个子量化器的中心点数 = 2^nbits
};

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

    // ---- 查询状态机 (事件驱动批量搜索) ----
    struct QueryState {
        uint32_t query_id = 0;
        const float* query = nullptr;
        size_t k = 0;
        size_t ef = 0;

        // candidate_set: 最小堆, 距离小的优先展开
        std::priority_queue<std::pair<float, uint32_t>,
                            std::vector<std::pair<float, uint32_t>>,
                            std::greater<std::pair<float, uint32_t>>> candidate_set;
        // top_candidates: 最大堆, 距离大的在堆顶方便淘汰
        std::priority_queue<std::pair<float, uint32_t>,
                            std::vector<std::pair<float, uint32_t>>,
                            std::less<std::pair<float, uint32_t>>> top_candidates;
        std::unique_ptr<VisitedList> visited;
        float lowerBound = 0.0f;

        // 调度状态
        bool done = false;
        uint32_t waitingBlockId = 0;  // 正在等待的 block (0=不需要等待)
        bool entryLoaded = false;     // 入口节点是否已加载
        uint32_t entryNewId = 0;      // Layer 0 入口 (new_id)

        // deferred 邻居列表: block 不在缓存的邻居, 等 I/O 完成后处理
        struct DeferredNeighbor {
            uint32_t neighborId;
            uint32_t blockId;
        };
        std::vector<DeferredNeighbor> deferred;
    };

    // 构造函数（原始接口，向后兼容）
    DiskHNSW(const std::string& graph_path,
             const std::string& bfs_path,
             const std::string& blocks_path,
             const std::string& route_path,
             size_t cache_slots = 64,
             uint32_t dim = 128);

    // 构造函数（可插拔接口，接受外部构造的 BlockCache）
    DiskHNSW(const std::string& graph_path,
             const std::string& bfs_path,
             std::unique_ptr<BlockCache> cache);

    ~DiskHNSW() = default;

    // 设置 ef_search 参数
    void setEf(size_t ef) { ef_search_ = ef; }
    size_t getEf() const { return ef_search_; }

    // KNN搜索
    std::vector<SearchResult> searchKnn(const float* query, size_t k);
    std::vector<std::vector<SearchResult>> batchSearch(const std::vector<float>& queries, size_t k, size_t batch_size = 8);

    // 事件驱动批量搜索 (单线程多查询并发)
    // 查询 A 阻塞 I/O 时切换到查询 B, 无锁竞争
    std::vector<std::vector<SearchResult>>
    batchSearchEventDriven(const std::vector<float>& queries, size_t k, size_t batch_size = 4);

    // 并发批量搜索 (多线程, 共享 BlockCache 和 GraphPrefetcher)
    // num_threads: 并发线程数
    std::vector<std::vector<SearchResult>>
    batchSearchConcurrent(const std::vector<float>& queries, size_t k, size_t num_threads);

    // 获取缓存统计信息
    const BlockCache::Stats& getCacheStats() const { return cache_->getStats(); }

    // 重置缓存统计
    void resetCacheStats() { cache_->resetStats(); }

    // 释放 blocks 文件的 page cache (每次查询后调用)
    void dropPageCache() { cache_->dropPageCache(); }

    // ---- Phase 3a: 查询间预取 ----
    void startRecordingBlocks() { recorded_blocks_.clear(); recording_ = true; }
    void stopRecordingBlocks() { recording_ = false; }
    const std::set<uint32_t>& getRecordedBlocks() const { return recorded_blocks_; }

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
    void enableGraphPrefetch(bool use_odirect = true);
    void disableGraphPrefetch();
    bool isGraphPrefetchEnabled() const { return graph_prefetch_enabled_; }
    GraphPrefetcher::Stats getGraphPrefetchStats() const;
    void resetGraphPrefetchStats();

    // ---- Phase 3: 轨迹采集 ----
    void setTraceCallback(std::function<void(uint32_t, bool)> cb) { cache_->setTraceCallback(std::move(cb)); }
    void clearTraceCallback() { cache_->clearTraceCallback(); }

    // ---- PQ 支持 ----
    void loadPQCodes(const std::string& pq_path);
    bool isPQEnabled() const { return pq_enabled_; }
    const PQParams& getPQParams() const { return pq_params_; }
    float pqDistance(const float* query, uint32_t node_id_new) const;

private:
    // ---- 图数据（常驻内存，old_id空间）----
    GraphStructure graph_;
    uint32_t dim_;
    size_t ef_search_;
    size_t dim_param_;  // 向量维度参数 (兼容旧代码)

    // ---- BFS映射 ----
    std::vector<uint32_t> old_to_new_;
    std::vector<uint32_t> new_to_old_;

    // ---- BlockCache（new_id空间）----
    std::unique_ptr<BlockCache> cache_;
    size_t cache_slots_ = 0;

    // ---- Phase 3 Redesign: 图引导预取器 ----
    std::unique_ptr<GraphPrefetcher> graph_prefetcher_;
    std::unique_ptr<BlockHeatEvaluator> heat_evaluator_;
    bool graph_prefetch_enabled_ = false;

    // ---- Phase 3 CPU Opt: 路由表缓存 ----
    const std::vector<uint32_t>* route_table_ = nullptr;

    // ---- Phase 3a: 查询间预取 ----
    bool recording_ = false;
    std::set<uint32_t> recorded_blocks_;

    // ---- PQ 数据 ----
    bool pq_enabled_ = false;
    PQParams pq_params_;
    std::vector<float> pq_codebook_;    // [M * ksub * dsub] floats
    std::vector<uint8_t> pq_codes_;    // [N * M] bytes, indexed by new_id

    // ---- 距离计算 ----
    float l2Distance(const float* a, const float* b) const;

    // ---- 搜索内部方法 ----
    uint32_t greedyDescent(const float* query);

    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>>
    searchLayer0(uint32_t entry_new_id, const float* query, size_t ef,
                 VisitedList& visited);

    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>>
    searchLayer0NonBlocking(uint32_t entry_new_id, const float* query, size_t ef,
                            VisitedList& visited);

    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>>
    searchLayer0Beam(uint32_t entry_new_id, const float* query, size_t ef,
                     VisitedList& visited, int beam_width);

    void expandBeamCandidate(uint32_t nodeId, uint32_t blockId,
                             const float* query, size_t ef, float frozenLB,
                             std::priority_queue<std::pair<float, uint32_t>,
                                 std::vector<std::pair<float, uint32_t>>,
                                 std::less<std::pair<float, uint32_t>>>& top_candidates,
                             std::priority_queue<std::pair<float, uint32_t>,
                                 std::vector<std::pair<float, uint32_t>>,
                                 std::greater<std::pair<float, uint32_t>>>& candidate_set,
                             VisitedList& visited,
                             const std::function<uint32_t(uint32_t)>& getBlockIdFast);

    // ---- 批量并行 I/O 搜索 ----
    // 取 candidate queue 的 top-N, 批量收集所有未访问邻居,
    // 一次性提交 io_uring, 并行返回后批量算距离
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>>
    searchLayer0BatchIO(uint32_t entry_new_id, const float* query, size_t ef,
                        VisitedList& visited, int batch_size);

    // ---- 事件驱动批量搜索内部方法 ----
    void initQueryState(QueryState& qs, const float* query, size_t k, size_t ef);
    void stepQueryState(QueryState& qs);
};
