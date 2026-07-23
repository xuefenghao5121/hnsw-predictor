// disk_hnsw.cpp - DiskHNSW 实现
//
// 实现要点：
// 1. 贪心下降使用内存中的graph_structure数据（old_id空间）
// 2. Layer 0搜索通过BlockCache按需加载（new_id空间）
// 3. 两层之间通过old_to_new/new_to_old映射转换
// 4. 搜索结果转换为label返回
//
// 设计文档: hnsw-research/phase2-design.md

#include "disk_hnsw.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

// ============================================================
// 构造函数（原始接口，向后兼容）
// ============================================================

DiskHNSW::DiskHNSW(const std::string& graph_path,
                   const std::string& bfs_path,
                   const std::string& blocks_path,
                   const std::string& route_path,
                   size_t cache_slots,
                   uint32_t dim)
    : dim_(dim)
    , ef_search_(10)
    , dim_param_(dim)
{
    // ---- 1. 加载图结构 (slim 模式: 只加载上层节点数据) ----
    std::cout << "[DiskHNSW] Loading graph structure (slim) from " << graph_path << "..." << std::endl;
    graph_ = load_graph_structure_slim(graph_path);
    dim_ = graph_.dim;
    dim_param_ = dim_;

    std::cout << "[DiskHNSW] Graph: " << graph_.num_nodes << " nodes, dim=" << dim_
              << ", max_level=" << graph_.max_level
              << ", entry_point=" << graph_.entry_point << std::endl;

    // ---- 2. 加载BFS映射 ----
    std::cout << "[DiskHNSW] Loading BFS order from " << bfs_path << "..." << std::endl;
    std::ifstream bfs_in(bfs_path, std::ios::binary);
    if (!bfs_in.is_open()) {
        throw std::runtime_error("Cannot open BFS file: " + bfs_path);
    }

    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    if (bhdr.magic != MAGIC_BFS) {
        throw std::runtime_error("Invalid BFS file magic");
    }
    if (bhdr.num_nodes != graph_.num_nodes) {
        throw std::runtime_error("BFS node count mismatch: " + std::to_string(bhdr.num_nodes) +
                                 " vs graph " + std::to_string(graph_.num_nodes));
    }

    old_to_new_.resize(graph_.num_nodes);
    new_to_old_.resize(graph_.num_nodes);
    bfs_in.read(reinterpret_cast<char*>(old_to_new_.data()), graph_.num_nodes * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(new_to_old_.data()), graph_.num_nodes * sizeof(uint32_t));
    bfs_in.close();

    std::cout << "[DiskHNSW] BFS mapping loaded: " << old_to_new_.size() << " entries" << std::endl;

    // ---- 3. 初始化BlockCache ----
    std::cout << "[DiskHNSW] Initializing BlockCache..." << std::endl;
    cache_ = std::make_unique<BlockCache>(blocks_path, route_path, cache_slots, dim_);
    cache_slots_ = cache_slots;
}

// ============================================================
// 构造函数（可插拔接口）
// ============================================================

DiskHNSW::DiskHNSW(const std::string& graph_path,
                   const std::string& bfs_path,
                   std::unique_ptr<BlockCache> cache)
    : dim_(0)
    , ef_search_(10)
    , cache_(std::move(cache))
    , dim_param_(0)
{
    // ---- 1. 加载图结构 (slim 模式: 只加载上层节点数据) ----
    std::cout << "[DiskHNSW] Loading graph structure (slim) from " << graph_path << "..." << std::endl;
    graph_ = load_graph_structure_slim(graph_path);
    dim_ = graph_.dim;
    dim_param_ = dim_;

    std::cout << "[DiskHNSW] Graph: " << graph_.num_nodes << " nodes, dim=" << dim_
              << ", max_level=" << graph_.max_level
              << ", entry_point=" << graph_.entry_point << std::endl;

    // ---- 2. 加载BFS映射 ----
    std::cout << "[DiskHNSW] Loading BFS order from " << bfs_path << "..." << std::endl;
    std::ifstream bfs_in(bfs_path, std::ios::binary);
    if (!bfs_in.is_open()) {
        throw std::runtime_error("Cannot open BFS file: " + bfs_path);
    }

    BfsHeader bhdr;
    bfs_in.read(reinterpret_cast<char*>(&bhdr), sizeof(BfsHeader));
    if (bhdr.magic != MAGIC_BFS) {
        throw std::runtime_error("Invalid BFS file magic");
    }
    if (bhdr.num_nodes != graph_.num_nodes) {
        throw std::runtime_error("BFS node count mismatch: " + std::to_string(bhdr.num_nodes) +
                                 " vs graph " + std::to_string(graph_.num_nodes));
    }

    old_to_new_.resize(graph_.num_nodes);
    new_to_old_.resize(graph_.num_nodes);
    bfs_in.read(reinterpret_cast<char*>(old_to_new_.data()), graph_.num_nodes * sizeof(uint32_t));
    bfs_in.read(reinterpret_cast<char*>(new_to_old_.data()), graph_.num_nodes * sizeof(uint32_t));
    bfs_in.close();

    std::cout << "[DiskHNSW] BFS mapping loaded: " << old_to_new_.size() << " entries" << std::endl;
    std::cout << "[DiskHNSW] BlockCache (pluggable) initialized" << std::endl;
    cache_slots_ = cache_->getCacheSlots();
}

// ============================================================
// 距离计算
// ============================================================

float DiskHNSW::l2Distance(const float* a, const float* b) const {
    // 简单的L2距离计算（与hnswlib L2Sqr一致）
    // 后续可以替换为SIMD优化版本
    float res = 0;
    for (size_t i = 0; i < dim_; i++) {
        float t = a[i] - b[i];
        res += t * t;
    }
    return res;
}

// ============================================================
// 贪心下降（内存中的上层图，old_id空间）
// ============================================================

uint32_t DiskHNSW::greedyDescent(const float* query) {
    uint32_t currObj = graph_.entry_point;

    // 从最高层逐层下降到 Layer 1
    // 在每一层，遍历当前节点的邻居，如果有更近的就移动过去
    for (int level = graph_.max_level; level > 0; level--) {
        bool changed = true;
        while (changed) {
            changed = false;

            // 获取当前节点在本层的邻居列表（old_id空间）
            // 检查该节点是否有上层邻居
            if (graph_.levels[currObj] < level) {
                // 当前节点不在这一层，无法继续
                // 这种情况不应该发生（贪心下降保证当前节点在该层存在）
                break;
            }

            const auto& neighbors = graph_.upper_adjacency[currObj][level];

            // 当前节点到query的距离 (使用 upper_vectors)
            const float* currVec = graph_.upper_vectors[currObj].data();
            float curDist = l2Distance(query, currVec);

            // 遍历邻居，寻找更近的
            for (uint32_t neighbor : neighbors) {
                if (neighbor >= graph_.num_nodes) continue;

                const float* neighborVec = graph_.upper_vectors[neighbor].data();
                float d = l2Distance(query, neighborVec);

                if (d < curDist) {
                    curDist = d;
                    currObj = neighbor;
                    changed = true;
                }
            }
        }
    }

    return currObj;  // 返回old_id
}

// ============================================================
// Layer 0 搜索（BlockCache按需加载，new_id空间）
// ============================================================

std::priority_queue<std::pair<float, uint32_t>,
                    std::vector<std::pair<float, uint32_t>>,
                    std::greater<std::pair<float, uint32_t>>>
DiskHNSW::searchLayer0(uint32_t entry_new_id, const float* query, size_t ef,
                       VisitedList& visited) {
    // 使用最大堆维护top candidates（距离大的在堆顶，方便淘汰）
    // 使用最小堆维护candidate set（距离小的在堆顶，优先展开）
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::less<std::pair<float, uint32_t>>> top_candidates;  // 最大堆
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>> candidate_set;  // 最小堆

    // 初始化：计算入口节点距离
    // 重要：获取向量后立即计算距离，不在此之后进行其他BlockCache访问
    const float* entryVec = cache_->getNodeVector(entry_new_id);
    if (!entryVec) {
        std::cerr << "[DiskHNSW] ERROR: Failed to get vector for entry node " << entry_new_id << std::endl;
        return candidate_set;  // 返回空
    }
    float entryDist = l2Distance(query, entryVec);
    // 向量指针entryVec在此之后不再使用
    top_candidates.emplace(entryDist, entry_new_id);
    candidate_set.emplace(entryDist, entry_new_id);
    visited.markVisited(entry_new_id);

    float lowerBound = entryDist;

    while (!candidate_set.empty()) {
        // 取出距离最小的候选
        auto [candidateDist, candidateId] = candidate_set.top();

        // 如果最小候选距离已经大于当前top candidates中的最大距离
        // 且top candidates已满，可以停止
        if (candidateDist > lowerBound && top_candidates.size() == ef) {
            break;
        }
        candidate_set.pop();

        // 获取候选节点的邻居列表（通过BlockCache，new_id空间）
        // 重要：必须复制邻居ID列表，因为后续访问其他节点的向量时
        // 可能导致当前Block被LRU淘汰，使指针失效
        uint32_t neighborCount = 0;
        const uint32_t* neighbors = cache_->getNodeNeighbors(candidateId, neighborCount);

        if (!neighbors || neighborCount == 0) continue;

        // 复制邻居ID到本地缓冲区
        std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

        // ---- Phase 3 Redesign: 图引导预取 (I/O 与计算重叠) ----
        //
        // 优化后的流水线 (保持搜索顺序不变):
        //   1. 提交预取 -> I/O 在后台开始
        //   2. 处理 in-cache 邻居 -> 计算距离, 与 I/O 并行
        //   3. 收集 cache-miss 邻居的 block IDs
        //   4. 批量等待所有需要的 blocks (waitForBlocks)
        //   5. 处理 cache-miss 邻居 (此时已在缓存中)
        //
        // 关键改进:
        //   1. 选择性批量等待: waitForBlocks 只等需要的 block (Opt 1+2)
        //   2. 零拷贝: insertBlockFromPtr 避免 temp vector (Opt 3)
        //   3. 减少 reap: 只在 waitForBlocks 内部 reap (Opt 4)
        //   4. 批量提交: submitPrefetch 支持延迟提交 (Opt 5)

        // ---- 提交预取 (1-hop) ----
        // 预取当前候选的邻居所在的 block
        // I/O 在后台进行, 与后续 in-cache 邻居的距离计算重叠
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            std::vector<uint32_t> prefetch_blocks;
            uint32_t curr_block = cache_->getBlockId(candidateId);
            for (uint32_t j = 0; j < local_neighbors.size(); j++) {
                uint32_t neighbor_block = cache_->getBlockId(local_neighbors[j]);
                if (neighbor_block != curr_block) {
                    prefetch_blocks.push_back(neighbor_block);
                }
            }
            std::sort(prefetch_blocks.begin(), prefetch_blocks.end());
            prefetch_blocks.erase(
                std::unique(prefetch_blocks.begin(), prefetch_blocks.end()),
                prefetch_blocks.end());
            if (!prefetch_blocks.empty()) {
                graph_prefetcher_->submitPrefetch(prefetch_blocks, true);
            }
        }

        // ---- 处理 in-cache 邻居, 收集 cache-miss 邻居 ----
        struct PendingNeighbor {
            uint32_t neighborId;
            uint32_t blockId;
        };
        std::vector<PendingNeighbor> pending_neighbors;

        for (uint32_t j = 0; j < local_neighbors.size(); j++) {
            uint32_t neighborId = local_neighbors[j];

            if (neighborId >= graph_.num_nodes) continue;
            if (visited.isVisited(neighborId)) continue;

            visited.markVisited(neighborId);

            uint32_t neighbor_block = cache_->getBlockId(neighborId);
            if (cache_->isInCache(neighbor_block)) {
                // 缓存命中, 直接计算距离 (I/O 在后台并行进行)
                const float* neighborVec = cache_->getNodeVector(neighborId);
                if (!neighborVec) continue;

                float dist = l2Distance(query, neighborVec);

                if (top_candidates.size() < ef || lowerBound > dist) {
                    candidate_set.emplace(dist, neighborId);
                    top_candidates.emplace(dist, neighborId);
                    if (top_candidates.size() > ef) {
                        top_candidates.pop();
                    }
                    if (!top_candidates.empty()) {
                        lowerBound = top_candidates.top().first;
                    }
                }
            } else {
                // 缓存未命中, 加入 pending 列表
                pending_neighbors.push_back({neighborId, neighbor_block});
            }
        }

        // ---- 处理 pending 邻居 (批量等待) ----
        // 使用 waitForBlocks 批量等待所有需要的 blocks
        // 比 waitForCompletions 更高效: 只等需要的 block, 不等无关 I/O
        // 比逐个 waitForBlock 更高效: 一次 wait+reap 可能完成多个 block
        if (!pending_neighbors.empty()) {
            // 收集 unique block IDs
            std::set<uint32_t> needed_blocks;
            for (const auto& pn : pending_neighbors) {
                needed_blocks.insert(pn.blockId);
            }

            // 批量等待
            if (graph_prefetch_enabled_ && graph_prefetcher_) {
                graph_prefetcher_->waitForBlocks(needed_blocks);
            }

            // 处理所有 pending 邻居 (此时它们的 block 应已在缓存中)
            for (const auto& pn : pending_neighbors) {
                const float* neighborVec = cache_->getNodeVector(pn.neighborId);
                if (!neighborVec) continue;

                float dist = l2Distance(query, neighborVec);

                if (top_candidates.size() < ef || lowerBound > dist) {
                    candidate_set.emplace(dist, pn.neighborId);
                    top_candidates.emplace(dist, pn.neighborId);
                    if (top_candidates.size() > ef) {
                        top_candidates.pop();
                    }
                    if (!top_candidates.empty()) {
                        lowerBound = top_candidates.top().first;
                    }
                }
            }
        }
    }

    // 将top_candidates转换为最小堆返回（距离小的在堆顶）
    // top_candidates是最大堆，我们需要反转
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>> result;

    while (!top_candidates.empty()) {
        result.push(top_candidates.top());
        top_candidates.pop();
    }

    return result;
}

// ============================================================
// KNN搜索
// ============================================================

std::vector<DiskHNSW::SearchResult> DiskHNSW::searchKnn(const float* query, size_t k) {
    std::vector<SearchResult> result;
    if (graph_.num_nodes == 0) return result;

    // Phase 1: 贪心下降（内存中的上层图，old_id空间）
    uint32_t entryOldId = greedyDescent(query);

    // 转换为new_id用于Layer 0搜索
    uint32_t entryNewId = old_to_new_[entryOldId];

    // Phase 2: Layer 0搜索（BlockCache按需加载，new_id空间）
    size_t ef = std::max(ef_search_, k);

    // 创建VisitedList（new_id空间）
    VisitedList visited(graph_.num_nodes);

    auto top_candidates = searchLayer0(entryNewId, query, ef, visited);

    // 提取top-k结果
    size_t numResults = std::min(k, top_candidates.size());
    result.reserve(numResults);

    // top_candidates是最小堆，距离小的先出
    for (size_t i = 0; i < numResults && !top_candidates.empty(); i++) {
        auto [dist, newId] = top_candidates.top();
        top_candidates.pop();

        // 转换为old_id，然后获取label
        uint32_t oldId = new_to_old_[newId];
        uint64_t label = graph_.labels[oldId];

        result.emplace_back(dist, label);
    }

    return result;
}

// ============================================================
// Phase 3 Redesign: 图引导预取支持
// ============================================================

void DiskHNSW::enableGraphPrefetch(bool use_odirect) {
    graph_prefetcher_ = std::make_unique<GraphPrefetcher>(cache_.get(), 128, use_odirect);
    graph_prefetch_enabled_ = true;
    std::cout << "[DiskHNSW] Graph-guided prefetch enabled (io_uring, odirect="
              << (use_odirect ? "yes" : "no") << ")" << std::endl;
}

void DiskHNSW::disableGraphPrefetch() {
    if (graph_prefetcher_) {
        // 等待所有未完成的预取
        graph_prefetcher_->waitForCompletions(100000);  // 100ms max
        graph_prefetcher_.reset();
    }
    graph_prefetch_enabled_ = false;
    std::cout << "[DiskHNSW] Graph-guided prefetch disabled" << std::endl;
}

const GraphPrefetcher::Stats& DiskHNSW::getGraphPrefetchStats() const {
    static const GraphPrefetcher::Stats empty_stats;
    if (graph_prefetcher_) return graph_prefetcher_->getStats();
    return empty_stats;
}

void DiskHNSW::resetGraphPrefetchStats() {
    if (graph_prefetcher_) graph_prefetcher_->resetStats();
}


