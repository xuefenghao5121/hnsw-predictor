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
#include <cstdlib>
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
    // 标量 L2 距离计算（不优化，保持与 hnswlib 一致，作为公平对比基线）
    float result = 0.0f;
    for (size_t i = 0; i < dim_; i++) {
        float t = a[i] - b[i];
        result += t * t;
    }
    return result;
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

    // 路由表快速访问 lambda（避免虚函数调用）
    auto getBlockIdFast = [&](uint32_t node_id) -> uint32_t {
        if (route_table_) return (*route_table_)[node_id];
        return cache_->getBlockId(node_id);
    };

    // 初始化：计算入口节点距离
    const float* entryVec = cache_->getNodeVector(entry_new_id);
    if (!entryVec) {
        std::cerr << "[DiskHNSW] ERROR: Failed to get vector for entry node " << entry_new_id << std::endl;
        return candidate_set;  // 返回空
    }
    float entryDist = l2Distance(query, entryVec);
    top_candidates.emplace(entryDist, entry_new_id);
    candidate_set.emplace(entryDist, entry_new_id);
    visited.markVisited(entry_new_id);

    float lowerBound = entryDist;

    // 时效性实验: lookahead 预取深度 (环境变量 LOOKAHEAD_HOPS, 0=关闭=原 baseline)
    static const int kLookaheadHops = [](){
        const char* e = std::getenv("LOOKAHEAD_HOPS");
        return e ? std::atoi(e) : 0;
    }();

    while (!candidate_set.empty()) {
        auto [candidateDist, candidateId] = candidate_set.top();

        if (candidateDist > lowerBound && top_candidates.size() == ef) {
            break;
        }
        candidate_set.pop();

        // ---- 时效性实验: lookahead 预取 ----
        // 偏看 candidate_set 里即将展开的后 N 个 candidate,提前预取它们邻居的 block
        // 不碰遍历顺序/lowerBound/visited/top_candidates -> recall 不受影响
        if (kLookaheadHops > 0 && graph_prefetch_enabled_ && graph_prefetcher_) {
            auto cs_copy = candidate_set;  // 拷贝, 不动原队列
            std::vector<uint32_t> la_blocks;
            int hops = 0;
            while (!cs_copy.empty() && hops < kLookaheadHops) {
                uint32_t la_cand = cs_copy.top().second;
                cs_copy.pop();
                hops++;
                uint32_t la_cand_block = getBlockIdFast(la_cand);
                // 只能从已缓存的 candidate block 读邻居(能进 candidate_set 说明已加载)
                CachedBlock* lb = cache_->peekCachedBlockById(la_cand_block);
                if (!lb) continue;
                uint32_t nc = 0;
                const uint32_t* nbrs = lb->getNeighbors(la_cand, nc);
                if (!nbrs) continue;
                for (uint32_t k2 = 0; k2 < nc; k2++) {
                    uint32_t nb = getBlockIdFast(nbrs[k2]);
                    la_blocks.push_back(nb);
                }
            }
            if (!la_blocks.empty()) {
                std::sort(la_blocks.begin(), la_blocks.end());
                la_blocks.erase(std::unique(la_blocks.begin(), la_blocks.end()), la_blocks.end());
                graph_prefetcher_->submitPrefetch(la_blocks, true);  // 内部自动跳过已缓存/在途
            }
        }

        // ---- 优化：直接获取 CachedBlock，避免后续多次锁 + 路由查找 ----
        uint32_t curr_block_id = getBlockIdFast(candidateId);
        CachedBlock* candidateBlock = cache_->getCachedBlockById(curr_block_id);
        // 更新 block 热度
        if (heat_evaluator_) heat_evaluator_->onBlockAccess(curr_block_id);
        if (!candidateBlock) {
            // 块不在缓存中（可能被淘汰），回退到 getNodeNeighbors
            uint32_t neighborCount = 0;
            const uint32_t* neighbors = cache_->getNodeNeighbors(candidateId, neighborCount);
            if (!neighbors || neighborCount == 0) continue;
            std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

            // 回退路径：使用原始逻辑
            // 提交预取
            if (graph_prefetch_enabled_ && graph_prefetcher_) {
                std::vector<uint32_t> prefetch_blocks;
                for (uint32_t nid : local_neighbors) {
                    uint32_t nblock = getBlockIdFast(nid);
                    if (nblock != curr_block_id) prefetch_blocks.push_back(nblock);
                }
                std::sort(prefetch_blocks.begin(), prefetch_blocks.end());
                prefetch_blocks.erase(std::unique(prefetch_blocks.begin(), prefetch_blocks.end()), prefetch_blocks.end());
                if (!prefetch_blocks.empty()) graph_prefetcher_->submitPrefetch(prefetch_blocks, true);
            }

            struct PendingNeighbor { uint32_t neighborId; uint32_t blockId; };
            std::vector<PendingNeighbor> pending_neighbors;

            for (uint32_t nid : local_neighbors) {
                if (nid >= graph_.num_nodes) continue;
                if (visited.isVisited(nid)) continue;
                visited.markVisited(nid);
                uint32_t nblock = getBlockIdFast(nid);
                if (cache_->isInCache(nblock)) {
                    const float* nvec = cache_->getNodeVector(nid);
                    if (!nvec) continue;
                    float dist = l2Distance(query, nvec);
                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(dist, nid);
                        top_candidates.emplace(dist, nid);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                } else {
                    pending_neighbors.push_back({nid, nblock});
                }
            }

            if (!pending_neighbors.empty()) {
                std::set<uint32_t> needed_blocks;
                for (const auto& pn : pending_neighbors) needed_blocks.insert(pn.blockId);
                if (graph_prefetch_enabled_ && graph_prefetcher_) graph_prefetcher_->waitForBlocks(needed_blocks);
                for (const auto& pn : pending_neighbors) {
                    const float* nvec = cache_->getNodeVector(pn.neighborId);
                    if (!nvec) continue;
                    float dist = l2Distance(query, nvec);
                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(dist, pn.neighborId);
                        top_candidates.emplace(dist, pn.neighborId);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                }
            }
            continue;
        }

        // ---- 快速路径：从 CachedBlock 直接获取邻居 ----
        uint32_t neighborCount = 0;
        const uint32_t* neighbors = candidateBlock->getNeighbors(candidateId, neighborCount);
        if (!neighbors || neighborCount == 0) continue;

        // 复制邻居ID到本地缓冲区（因为后续操作可能导致 block 被淘汰）
        std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

        // ---- 提交预取 (1-hop, 热度排序但不丢充) ----
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            std::vector<uint32_t> prefetch_blocks;
            for (uint32_t nid : local_neighbors) {
                uint32_t neighbor_block = getBlockIdFast(nid);
                if (neighbor_block != curr_block_id) {
                    prefetch_blocks.push_back(neighbor_block);
                }
            }
            std::sort(prefetch_blocks.begin(), prefetch_blocks.end());
            prefetch_blocks.erase(
                std::unique(prefetch_blocks.begin(), prefetch_blocks.end()),
                prefetch_blocks.end());

            // 热度排序: 热 block 排前面优先预取 (但不丢弃冷 block)
            if (heat_evaluator_ && heat_evaluator_->getQueryCount() > 5) {
                std::sort(prefetch_blocks.begin(), prefetch_blocks.end(),
                    [this](uint32_t a, uint32_t b) {
                        return heat_evaluator_->getHeat(a) > heat_evaluator_->getHeat(b);
                    });
            }

            if (!prefetch_blocks.empty()) {
                graph_prefetcher_->submitPrefetch(prefetch_blocks, true);
            }
        }

        // ---- 处理 in-cache 邻居, 收集 cache-miss 邻居 ----
        // 优化：用 getCachedBlockById 替代 isInCache + getNodeVector
        // 减少：2 次锁获取 -> 1 次，N 次路由查找 -> 0 次（block_id 已知）
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

            uint32_t neighbor_block = getBlockIdFast(neighborId);

            // 优化：用 getCachedBlockById 一次锁获取获取 block + vector
            // 不再需要 isInCache + getNodeVector 两次锁
            CachedBlock* nBlock = cache_->getCachedBlockById(neighbor_block);
            if (nBlock) {
                const float* neighborVec = nBlock->getVector(neighborId);
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
                pending_neighbors.push_back({neighborId, neighbor_block});
            }
        }

        // ---- 处理 pending 邻居 (批量等待) ----
        if (!pending_neighbors.empty()) {
            std::set<uint32_t> needed_blocks;
            for (const auto& pn : pending_neighbors) {
                needed_blocks.insert(pn.blockId);
            }

            if (graph_prefetch_enabled_ && graph_prefetcher_) {
                graph_prefetcher_->waitForBlocks(needed_blocks);

                // 预取完成后，用 getCachedBlockById 快速访问
                for (const auto& pn : pending_neighbors) {
                    CachedBlock* nBlock = cache_->getCachedBlockById(pn.blockId);
                    if (!nBlock) continue;
                    const float* neighborVec = nBlock->getVector(pn.neighborId);
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
            } else {
                // 无预取器：用 getNodeVector 触发磁盘加载
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
    }

    // 将top_candidates转换为最小堆返回
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
// 非阻塞 Layer 0 搜索 (I/O overlap 优化)
// ============================================================

// Deferred item: 邻居或候选节点的 block 不在缓存, 需要等待 I/O
struct DeferredItem {
    uint32_t nodeId;
    uint32_t blockId;
    float savedLowerBound;  // defer 时的 lowerBound
};

std::priority_queue<std::pair<float, uint32_t>,
                    std::vector<std::pair<float, uint32_t>>,
                    std::greater<std::pair<float, uint32_t>>>
DiskHNSW::searchLayer0NonBlocking(uint32_t entry_new_id, const float* query, size_t ef,
                                  VisitedList& visited) {
    // 最大堆维护 top candidates (距离大的在堆顶, 方便淘汰)
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::less<std::pair<float, uint32_t>>> top_candidates;
    // 最小堆维护 candidate set (距离小的在堆顶, 优先展开)
    std::priority_queue<std::pair<float, uint32_t>,
                        std::vector<std::pair<float, uint32_t>>,
                        std::greater<std::pair<float, uint32_t>>> candidate_set;

    auto getBlockIdFast = [&](uint32_t node_id) -> uint32_t {
        if (route_table_) return (*route_table_)[node_id];
        return cache_->getBlockId(node_id);
    };

    // 初始化: 入口节点
    const float* entryVec = cache_->getNodeVector(entry_new_id);
    if (!entryVec) {
        std::cerr << "[DiskHNSW] ERROR: Failed to get vector for entry node " << entry_new_id << std::endl;
        return candidate_set;
    }
    float entryDist = l2Distance(query, entryVec);
    top_candidates.emplace(entryDist, entry_new_id);
    candidate_set.emplace(entryDist, entry_new_id);
    visited.markVisited(entry_new_id);
    float lowerBound = entryDist;

    // deferred 列表: block 不在缓存的邻居
    std::vector<DeferredItem> deferred;

    while (true) {
        // ---- Phase 1: 处理所有 in-cache candidates (非阻塞) ----
        while (!candidate_set.empty()) {
            auto [candidateDist, candidateId] = candidate_set.top();

            if (candidateDist > lowerBound && top_candidates.size() == ef) {
                goto check_deferred;
            }
            candidate_set.pop();

            uint32_t curr_block_id = getBlockIdFast(candidateId);
            CachedBlock* candidateBlock = cache_->getCachedBlockById(curr_block_id);

            if (!candidateBlock) {
                // 候选 block miss: 同步加载 (避免 io_uring 死锁)
                cache_->getBlockById(curr_block_id);
                candidateBlock = cache_->getCachedBlockById(curr_block_id);
                if (!candidateBlock) continue;
                // 继续处理 (和阻塞版一样)
            }

            // 快速路径: 从 CachedBlock 获取邻居
            uint32_t neighborCount = 0;
            const uint32_t* neighbors = candidateBlock->getNeighbors(candidateId, neighborCount);
            if (!neighbors || neighborCount == 0) continue;
            std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

            // 提交 1-hop 预取
            if (graph_prefetch_enabled_ && graph_prefetcher_) {
                std::vector<uint32_t> prefetch_blocks;
                for (uint32_t nid : local_neighbors) {
                    uint32_t neighbor_block = getBlockIdFast(nid);
                    if (neighbor_block != curr_block_id) {
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

            // 处理 in-cache 邻居, defer out-of-cache 邻居
            for (uint32_t j = 0; j < local_neighbors.size(); j++) {
                uint32_t neighborId = local_neighbors[j];
                if (neighborId >= graph_.num_nodes) continue;
                if (visited.isVisited(neighborId)) continue;
                visited.markVisited(neighborId);

                uint32_t neighbor_block = getBlockIdFast(neighborId);
                CachedBlock* nBlock = cache_->getCachedBlockById(neighbor_block);
                if (nBlock) {
                    const float* neighborVec = nBlock->getVector(neighborId);
                    if (!neighborVec) continue;
                    float dist = l2Distance(query, neighborVec);
                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(dist, neighborId);
                        top_candidates.emplace(dist, neighborId);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                } else {
                    // 非阻塞: defer neighbor, 延迟 visited 标记
                    deferred.push_back({neighborId, neighbor_block, lowerBound});
                }
            }
            // 不调用 waitForBlocks! 继续处理下一个 candidate
        }

        check_deferred:
        // ---- Phase 2: candidate_set 空, 检查 deferred ----
        if (deferred.empty()) break;  // 真正完成!

        // 非阻塞 reap
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            graph_prefetcher_->reapCompletions();
        }

        // 检查哪些 deferred 邻居的 block 已就绪
        std::vector<DeferredItem> still_deferred;
        bool any_ready = false;

        for (auto& item : deferred) {
            CachedBlock* block = cache_->getCachedBlockById(item.blockId);
            if (block) {
                if (!visited.isVisited(item.nodeId)) {
                    visited.markVisited(item.nodeId);
                    const float* vec = block->getVector(item.nodeId);
                    if (vec) {
                        float dist = l2Distance(query, vec);
                        // 用 savedLowerBound (等价于阻塞版: 等待期间 lowerBound 不变)
                        if (top_candidates.size() < ef || lowerBound > dist) {
                            candidate_set.emplace(dist, item.nodeId);
                            top_candidates.emplace(dist, item.nodeId);
                            if (top_candidates.size() > ef) top_candidates.pop();
                            if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                        }
                    }
                }
                any_ready = true;
            } else {
                still_deferred.push_back(item);
            }
        }
        deferred = std::move(still_deferred);

        if (!candidate_set.empty()) {
            // 有新的 candidate, 回 Phase 1
            continue;
        }

        if (deferred.empty()) break;  // 全部处理完

        // ---- Phase 3: 所有 candidate 处理完, deferred 仍无就绪 -> 等 I/O ----
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            // 提交 deferred block 的预取 (可能尚未提交)
            std::vector<uint32_t> need_prefetch;
            for (const auto& item : deferred) {
                if (!cache_->isInCache(item.blockId)) {
                    need_prefetch.push_back(item.blockId);
                }
            }
            std::sort(need_prefetch.begin(), need_prefetch.end());
            need_prefetch.erase(std::unique(need_prefetch.begin(), need_prefetch.end()),
                                need_prefetch.end());
            if (!need_prefetch.empty()) {
                graph_prefetcher_->submitPrefetch(need_prefetch, true);
            }

            // 同步加载第一个 deferred block (避免 io_uring waitForAnyBlock 死锁)
            if (!deferred.empty()) {
                cache_->getBlockById(deferred.front().blockId);
            }
        } else {
            // 无预取器: 同步加载 (回退)
            for (const auto& item : deferred) {
                const float* vec = cache_->getNodeVector(item.nodeId);
                if (!vec) continue;
                float dist = l2Distance(query, vec);
                if (top_candidates.size() < ef || lowerBound > dist) {
                    candidate_set.emplace(dist, item.nodeId);
                    top_candidates.emplace(dist, item.nodeId);
                    if (top_candidates.size() > ef) top_candidates.pop();
                    if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                }
            }
            deferred.clear();
        }
        // 回 Phase 1 或 Phase 2
    }

    // 返回 candidate_set (最小堆) 以保持与 searchLayer0 接口一致
    // 将 top_candidates (最大堆) 中的元素转移到 candidate_set (最小堆)
    while (!top_candidates.empty()) {
        candidate_set.emplace(top_candidates.top());
        top_candidates.pop();
    }
    return candidate_set;
}

// ============================================================
// KNN搜索
// ============================================================

std::vector<DiskHNSW::SearchResult> DiskHNSW::searchKnn(const float* query, size_t k) {
    std::vector<SearchResult> result;
    if (graph_.num_nodes == 0) return result;

    // 热度评价器: 查询开始
    if (heat_evaluator_) heat_evaluator_->onQueryStart();

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

    // 热度评价器: 查询结束
    if (heat_evaluator_) heat_evaluator_->onQueryEnd();

    return result;
}

// ============================================================
// 批量 KNN 搜索 (I/O overlap 优化)
// ============================================================

std::vector<std::vector<DiskHNSW::SearchResult>>
DiskHNSW::batchSearch(const std::vector<float>& queries, size_t k, size_t batch_size) {
    std::vector<std::vector<SearchResult>> results;
    size_t dim = dim_;
    size_t total = queries.size() / dim;
    results.reserve(total);

    for (size_t batch_start = 0; batch_start < total; batch_start += batch_size) {
        size_t batch_end = std::min(batch_start + batch_size, total);
        size_t n = batch_end - batch_start;

        // Phase 1: 对所有查询做贪心下降, 收集 entry blocks
        std::vector<uint32_t> entry_new_ids(n);
        std::vector<uint32_t> entry_blocks;
        for (size_t i = 0; i < n; i++) {
            uint32_t entryOldId = greedyDescent(&queries[(batch_start + i) * dim]);
            entry_new_ids[i] = old_to_new_[entryOldId];
            uint32_t block = route_table_ ? (*route_table_)[entry_new_ids[i]]
                                          : cache_->getBlockId(entry_new_ids[i]);
            entry_blocks.push_back(block);
        }

        // Phase 2: 批量预取所有 entry blocks - DISABLED (查询间预取更高效)
        // if (graph_prefetch_enabled_ && graph_prefetcher_) {
        //     std::sort(entry_blocks.begin(), entry_blocks.end());
        //     entry_blocks.erase(std::unique(entry_blocks.begin(), entry_blocks.end()),
        //                        entry_blocks.end());
        //     graph_prefetcher_->submitPrefetch(entry_blocks, true);
        // }

        // Phase 3: 顺序搜索 (使用阻塞搜索保证 recall)
        // 后续查询受益于: (a) entry block 已预取 (b) 前序查询的缓存预热
        for (size_t i = 0; i < n; i++) {
            size_t ef = std::max(ef_search_, k);
            VisitedList visited(graph_.num_nodes);
            auto top_candidates = searchLayer0(
                entry_new_ids[i], &queries[(batch_start + i) * dim], ef, visited);

            // 提取 top-k
            std::vector<SearchResult> result;
            size_t numResults = std::min(k, top_candidates.size());
            result.reserve(numResults);
            for (size_t j = 0; j < numResults && !top_candidates.empty(); j++) {
                auto [dist, newId] = top_candidates.top();
                top_candidates.pop();
                uint32_t oldId = new_to_old_[newId];
                uint64_t label = graph_.labels[oldId];
                result.emplace_back(dist, label);
            }
            results.push_back(std::move(result));
        }
    }

    return results;
}

// ============================================================
// Phase 3 Redesign: 图引导预取支持
// ============================================================

void DiskHNSW::enableGraphPrefetch(bool use_odirect) {
    graph_prefetcher_ = std::make_unique<GraphPrefetcher>(cache_.get(), 512, use_odirect);
    graph_prefetch_enabled_ = true;

    // 初始化热度评价器
    heat_evaluator_ = std::make_unique<BlockHeatEvaluator>(cache_->num_blocks_);
    cache_->setHeatEvaluator(heat_evaluator_.get());

    // 缓存路由表指针，避免虚函数调用开销
    // 通过 BfsLayoutProvider 的 getRouteTable() 获取
    auto* bfs_layout = dynamic_cast<BfsLayoutProvider*>(cache_->layout_.get());
    if (bfs_layout) {
        route_table_ = &bfs_layout->getRouteTable();
    }

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


