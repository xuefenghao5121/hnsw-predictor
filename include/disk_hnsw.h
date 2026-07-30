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
#include "io_uring_wrapper.h"
#include "csr_cache.h"

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
// VisitedList: 位压缩访问标记数组
// 使用 1 byte/节点 + 版本计数器, 避免每次搜索 memset
// 内存: N bytes (vs N*4 bytes for uint32_t), 10M 节点 = 10MB vs 40MB
// ============================================================
struct VisitedList {
    std::vector<uint8_t> mass;  // 1 byte/节点, 存当前版本号
    uint8_t curV;               // 当前标记值 (1-255)

    explicit VisitedList(size_t num_elements)
        : mass(num_elements, 0), curV(1) {}

    void reset() {
        curV++;
        if (curV == 0) {  // 溢出 (256 次搜索后), 清空
            std::memset(mass.data(), 0, mass.size());
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

    // P3: CSRCache 访问
    CSRCache* getCSRCache() { return csr_cache_.get(); }

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
    // 每 query 预计算 PQ 距离表 [M * ksub], pqDistance 退化为查表 (SIMD 化)
    void buildPqDistTable(const float* query);

private:
    // ---- 图数据（常驻内存，old_id空间）----
    GraphStructure graph_;
    uint32_t dim_;
    size_t ef_search_;
    size_t dim_param_;  // 向量维度参数 (兼容旧代码)

    // ---- BFS映射 ----
    std::vector<uint32_t> old_to_new_;
    std::vector<uint32_t> new_to_old_;

    // ---- BFS-remapped L0 邻接表 (CSR 格式, 常驻内存) ----
    // 用于 multi-hop 预取: 无需读 block 即可遍历邻居
    // adj_csr_offsets_[i] .. adj_csr_offsets_[i+1] = 节点 i(new_id) 的邻居范围
    // adj_csr_neighbors_[j] = 邻居的 new_id
    std::vector<uint32_t> adj_csr_offsets_;   // size = N+1
    std::vector<uint32_t> adj_csr_neighbors_; // size = total_edges
    bool has_inmem_adjacency_ = false;

    // ---- CSR Delta+Varint 压缩 ----
    // 邻居列表存为 delta+varint 压缩字节流, 节省 ~40% 内存
    // adj_csr_compact_: 压缩后的字节流
    // adj_csr_byte_offsets_: 每个节点的邻居列表在字节流中的起始偏移 (N+1)
    // 解码时: 从 byte_offsets_[nid] 开始读 varint delta, 还原排序后的邻居 ID
    std::vector<uint8_t> adj_csr_compact_;        // 压缩字节流
    std::vector<uint32_t> adj_csr_byte_offsets_;   // 字节偏移 (N+1)
    bool csr_compressed_ = false;

    // ---- P3: CSR 分页缓存 (磁盘驻留) ----
    // 当 CSR_ON_DISK=1 时, compact_ 数据写入磁盘文件,
    // 通过 CSRCache 按需分页加载. byte_offsets_ 常驻内存.
    std::unique_ptr<CSRCache> csr_cache_;
    bool csr_on_disk_ = false;

    // 解码 buffer (thread_local, 避免频繁分配)
    // getInMemNeighbors 解码到这里, 返回指针
    static thread_local std::vector<uint32_t> csr_decode_buf_;

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
    int spec_pf_counter_ = 0;  // 投机预取节流计数器 (searchLayer0 PQ 分支)

    // ---- 细粒度精排读 (FINE_RERANK=1): 候选向量 4KB 页粒度读, I/O 64KB→4KB (16x↓) ----
    std::vector<uint16_t> node_slot_table_;     // node -> block 内 slot (2MB @1M)
    std::vector<uint32_t> block_data_offset_;   // block -> 向量区 data_offset
    std::vector<uint32_t> vec_route_table_;     // node -> vecblocks block_id (修复 blocks/vecblocks ID 不一致 bug)
    std::unique_ptr<IoUring> vec_ring_;         // 4KB buffer pool ring (独立于 block 预取 ring)
    int vec_blocks_fd_ = -1;
    uint32_t vec_block_size_ = 0;
    bool fine_rerank_ok_ = false;
    bool buildFineRerank(const std::string& blocks_path, uint32_t num_nodes);
    PQParams pq_params_;
    std::vector<float> pq_dist_table_;  // [M * ksub] 每 query 预计算的距离表
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
    // 事件驱动: 处理多个候选直到 I/O miss 或完成, 返回处理的步数
    int runQueryUntilMiss(QueryState& qs, int max_steps = 64);

    // 构建 BFS-remapped CSR 邻接表 (从 graph_.adjacency0 + old_to_new_ 映射)
    void buildInMemoryAdjacency();

    // 从内存 CSR 邻接表获取邻居 (new_id 空间)
    // 返回 nullptr + out_count=0 如果没有内存邻接表
    // 注意: 压缩模式下返回 thread_local buffer 指针, 调用方用完前不能再调
    const uint32_t* getInMemNeighbors(uint32_t new_id, uint32_t& out_count);

    // 解码单个节点的压缩 CSR 邻居列表到 buffer
    // 返回解码的邻居数量, neighbors 指向 csr_decode_buf_
    uint32_t decodeCsrNeighbors(uint32_t new_id);
};
