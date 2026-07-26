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
#include <thread>
#include <atomic>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>

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
    graph_ = load_graph_structure_slim_adj(graph_path);
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
    
    // ---- 构建 BFS-remapped CSR 邻接表 (常驻内存, 用于 multi-hop 预取) ----
    buildInMemoryAdjacency();

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
    graph_ = load_graph_structure_slim_adj(graph_path);
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
    
    // ---- 构建 BFS-remapped CSR 邻接表 (常驻内存, 用于 multi-hop 预取) ----
    buildInMemoryAdjacency();
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
// PQ 支持: 加载 PQ codes 和 codebook
// ============================================================

void DiskHNSW::loadPQCodes(const std::string& pq_path) {
    std::cout << "[DiskHNSW] Loading PQ codes from " << pq_path << "..." << std::endl;

    std::ifstream in(pq_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open PQ codes file: " + pq_path);
    }

    // 读取 PQ 文件头
    // 格式: magic(4B 'PQCO') + n(8B) + M(4B) + nbits(4B) + dim(4B)
    //        + codebook_M(4B) + codebook_K(4B) + codebook_dsub(4B)
    //        + codebook_data(M*K*dsub*4B) + pq_codes(n*M bytes)
    char magic[4];
    in.read(magic, 4);
    if (std::memcmp(magic, "PQCO", 4) != 0) {
        throw std::runtime_error("Invalid PQ codes file magic");
    }

    uint64_t n;
    uint32_t M, nbits, dim;
    in.read(reinterpret_cast<char*>(&n), sizeof(uint64_t));
    in.read(reinterpret_cast<char*>(&M), sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(&nbits), sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(&dim), sizeof(uint32_t));

    uint32_t cb_M, cb_K, cb_dsub;
    in.read(reinterpret_cast<char*>(&cb_M), sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(&cb_K), sizeof(uint32_t));
    in.read(reinterpret_cast<char*>(&cb_dsub), sizeof(uint32_t));

    pq_params_.M = M;
    pq_params_.nbits = nbits;
    pq_params_.dim = dim;
    pq_params_.dsub = cb_dsub;
    pq_params_.ksub = cb_K;

    std::cout << "  PQ params: N=" << n << ", M=" << M << ", nbits=" << nbits
              << ", dim=" << dim << ", dsub=" << cb_dsub
              << ", ksub=" << cb_K << std::endl;

    if (dim != dim_) {
        throw std::runtime_error("PQ dim mismatch: " + std::to_string(dim) +
                                 " vs graph dim " + std::to_string(dim_));
    }
    if (cb_M != M || cb_dsub * M != dim) {
        throw std::runtime_error("PQ codebook dimensions mismatch");
    }

    // 读取 codebook: M * ksub * dsub floats
    size_t codebook_size = (size_t)M * cb_K * cb_dsub;
    pq_codebook_.resize(codebook_size);
    in.read(reinterpret_cast<char*>(pq_codebook_.data()),
            codebook_size * sizeof(float));

    // 读取 PQ codes: n * M bytes (old_id 顺序)
    std::vector<uint8_t> pq_codes_old(n * M);
    in.read(reinterpret_cast<char*>(pq_codes_old.data()), n * M);
    in.close();

    // 按 BFS-reordered (new_id) 顺序重排 PQ codes
    // old_to_new_[old_id] = new_id
    // pq_codes_old[old_id * M .. old_id * M + M] -> pq_codes_[new_id * M .. new_id * M + M]
    pq_codes_.resize(n * M);
    for (uint32_t new_id = 0; new_id < n && new_id < graph_.num_nodes; new_id++) {
        uint32_t old_id = new_to_old_[new_id];
        std::memcpy(&pq_codes_[new_id * M], &pq_codes_old[old_id * M], M);
    }

    pq_enabled_ = true;

    size_t codebook_mb = codebook_size * sizeof(float) / (1024 * 1024);
    size_t codes_mb = n * M / (1024 * 1024);
    std::cout << "  PQ codebook: " << codebook_mb << " MB" << std::endl;
    std::cout << "  PQ codes: " << codes_mb << " MB (" << n << " x " << M << " bytes)" << std::endl;
    std::cout << "  PQ enabled: ADC distance will be used for Layer 0" << std::endl;
}

// ============================================================
// PQ ADC (Asymmetric Distance Computation)
// ============================================================

float DiskHNSW::pqDistance(const float* query, uint32_t node_id_new) const {
    // ADC: 用 query (原始向量) 和 PQ code (量化向量) 计算近似 L2 距离
    // 对每个子量化器 m:
    //   centroid = codebook[m][code[m]]  // dsub floats
    //   diff = query[m*dsub : (m+1)*dsub] - centroid
    //   dist += |diff|^2
    const uint8_t* code = &pq_codes_[(size_t)node_id_new * pq_params_.M];
    const float* cb = pq_codebook_.data();
    float dist = 0.0f;

    for (uint32_t m = 0; m < pq_params_.M; m++) {
        const float* centroid = cb + (size_t)m * pq_params_.ksub * pq_params_.dsub
                                    + (size_t)code[m] * pq_params_.dsub;
        const float* q_sub = query + (size_t)m * pq_params_.dsub;
        for (uint32_t j = 0; j < pq_params_.dsub; j++) {
            float d = q_sub[j] - centroid[j];
            dist += d * d;
        }
    }
    return dist;
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

    // 距离计算 helper: PQ 模式用 ADC, 否则用精确 L2
    auto computeNeighborDist = [&](uint32_t neighborId, const float* neighborVec) -> float {
        if (pq_enabled_) {
            return pqDistance(query, neighborId);
        }
        return l2Distance(query, neighborVec);
    };

    // 路由表快速访问 lambda（避免虚函数调用）
    auto getBlockIdFast = [&](uint32_t node_id) -> uint32_t {
        if (route_table_) return (*route_table_)[node_id];
        return cache_->getBlockId(node_id);
    };

    // 初始化：计算入口节点距离
    // PQ 模式: 用 ADC 距离; 否则: 从 block cache 获取向量算精确距离
    float entryDist;
    if (pq_enabled_) {
        entryDist = pqDistance(query, entry_new_id);
    } else {
        const float* entryVec = cache_->getNodeVector(entry_new_id);
        if (!entryVec) {
            std::cerr << "[DiskHNSW] ERROR: Failed to get vector for entry node " << entry_new_id << std::endl;
            return candidate_set;  // 返回空
        }
        entryDist = l2Distance(query, entryVec);
    }
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
        if (kLookaheadHops > 0 && graph_prefetch_enabled_ && graph_prefetcher_ && !pq_enabled_) {
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
            // 块不在缓存中（可能被淘汰）
            // 优先用 CSR 内存邻接表, 其次回退到 getNodeNeighbors
            uint32_t neighborCount = 0;
            const uint32_t* neighbors = nullptr;
            if (has_inmem_adjacency_) {
                neighbors = getInMemNeighbors(candidateId, neighborCount);
            }
            if (!neighbors) {
                neighbors = cache_->getNodeNeighbors(candidateId, neighborCount);
            }
            if (!neighbors || neighborCount == 0) continue;
            std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

            // 回退路径：使用原始逻辑
            // 提交预取 (PQ 模式跳过: 不走向量 I/O, 预取只会堵精排队列)
            if (graph_prefetch_enabled_ && graph_prefetcher_ && !pq_enabled_) {
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
                if (pq_enabled_) {
                    // PQ 模式: 不需要向量, 直接算 ADC 距离
                    float dist = pqDistance(query, nid);
                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(dist, nid);
                        top_candidates.emplace(dist, nid);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                } else if (cache_->isInCache(nblock)) {
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
                    if (pq_enabled_) {
                        float dist = pqDistance(query, pn.neighborId);
                        if (top_candidates.size() < ef || lowerBound > dist) {
                            candidate_set.emplace(dist, pn.neighborId);
                            top_candidates.emplace(dist, pn.neighborId);
                            if (top_candidates.size() > ef) top_candidates.pop();
                            if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                        }
                    } else {
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
            }
            continue;
        }

        // ---- 快速路径：从 CSR 内存邻接表或 CachedBlock 获取邻居 ----
        uint32_t neighborCount = 0;
        const uint32_t* neighbors = nullptr;
        if (has_inmem_adjacency_) {
            neighbors = getInMemNeighbors(candidateId, neighborCount);
        }
        if (!neighbors) {
            neighbors = candidateBlock->getNeighbors(candidateId, neighborCount);
        }
        if (!neighbors || neighborCount == 0) continue;

        // 复制邻居ID到本地缓冲区（因为后续操作可能导致 block 被淘汰）
        std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

        // ---- 提交预取 (1-hop, 热度排序但不丢充) ----
        // PQ 模式跳过: 搜索不读向量, 预取只会堵 Phase B 精排队列
        if (graph_prefetch_enabled_ && graph_prefetcher_ && !pq_enabled_) {
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
            if (pq_enabled_) {
                // PQ 模式: ADC 距离; PQ_HYBRID=1 时 cache 命中用精确距离(提升粗筛质量), miss 用 PQ(零等待)
                static const bool kPqHybrid = std::getenv("PQ_HYBRID") && std::atoi(std::getenv("PQ_HYBRID")) != 0;
                const float* nvec = nullptr;
                if (kPqHybrid) {
                    nvec = cache_->getFlatVector(neighborId);   // 热向量 cache (无锁, 无 I/O)
                    if (!nvec && nBlock) nvec = nBlock->getVector(neighborId);  // block cache
                }
                float dist = nvec ? l2Distance(query, nvec) : pqDistance(query, neighborId);
                if (top_candidates.size() < ef || lowerBound > dist) {
                    candidate_set.emplace(dist, neighborId);
                    top_candidates.emplace(dist, neighborId);
                    if (top_candidates.size() > ef) {
                        top_candidates.pop();
                    }
                    if (!top_candidates.empty()) {
                        lowerBound = top_candidates.top().first;
                    }
                    // 投机预取: top_candidates 的 miss blocks 周期性提交, I/O 被后续搜索掩盖
                    // (候选直接服务 Phase B 精排, accuracy 远高于 1-hop 预取)
                    static const bool kSpecPf = std::getenv("SPEC_PREFETCH") && std::atoi(std::getenv("SPEC_PREFETCH")) != 0;
                    if (kSpecPf && graph_prefetch_enabled_ && graph_prefetcher_ && top_candidates.size() == ef) {
                        if (++spec_pf_counter_ >= 16) {
                            spec_pf_counter_ = 0;
                            auto tc_copy = top_candidates;
                            std::vector<uint32_t> spec_blocks;
                            while (!tc_copy.empty()) {
                                uint32_t b = getBlockIdFast(tc_copy.top().second);
                                tc_copy.pop();
                                if (!cache_->isInCache(b)) spec_blocks.push_back(b);
                            }
                            if (!spec_blocks.empty()) {
                                std::sort(spec_blocks.begin(), spec_blocks.end());
                                spec_blocks.erase(std::unique(spec_blocks.begin(), spec_blocks.end()), spec_blocks.end());
                                graph_prefetcher_->submitPrefetch(spec_blocks, true);
                            }
                        }
                    }
                }
            } else if (nBlock) {
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
                    if (pq_enabled_) {
                        float dist = pqDistance(query, pn.neighborId);
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
                    } else {
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
                }
            } else {
                // 无预取器：用 getNodeVector 触发磁盘加载
                for (const auto& pn : pending_neighbors) {
                    if (pq_enabled_) {
                        float dist = pqDistance(query, pn.neighborId);
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
                    } else {
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
    float entryDist;
    if (pq_enabled_) {
        entryDist = pqDistance(query, entry_new_id);
    } else {
        const float* entryVec = cache_->getNodeVector(entry_new_id);
        if (!entryVec) {
            std::cerr << "[DiskHNSW] ERROR: Failed to get vector for entry node " << entry_new_id << std::endl;
            return candidate_set;
        }
        entryDist = l2Distance(query, entryVec);
    }
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

            // 快速路径: 从 CSR 内存或 CachedBlock 获取邻居
            uint32_t neighborCount = 0;
            const uint32_t* neighbors = nullptr;
            if (has_inmem_adjacency_) {
                neighbors = getInMemNeighbors(candidateId, neighborCount);
            }
            if (!neighbors) {
                neighbors = candidateBlock->getNeighbors(candidateId, neighborCount);
            }
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
                if (pq_enabled_) {
                    // PQ 模式: 不需要 block 中的向量, 直接算 ADC 距离
                    float dist = pqDistance(query, neighborId);
                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(dist, neighborId);
                        top_candidates.emplace(dist, neighborId);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                } else {
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
            if (pq_enabled_) {
                // PQ 模式: 不需要 block 中的向量
                if (!visited.isVisited(item.nodeId)) {
                    visited.markVisited(item.nodeId);
                    float dist = pqDistance(query, item.nodeId);
                    // 用 savedLowerBound (等价于阻塞版: 等待期间 lowerBound 不变)
                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(dist, item.nodeId);
                        top_candidates.emplace(dist, item.nodeId);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                }
                any_ready = true;
            } else {
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
                if (pq_enabled_) {
                    float dist = pqDistance(query, item.nodeId);
                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(dist, item.nodeId);
                        top_candidates.emplace(dist, item.nodeId);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                } else {
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
// Cache-Aware Beam Search (beam round 内 lowerBound 冻结)
// ============================================================

void DiskHNSW::expandBeamCandidate(
    uint32_t nodeId, uint32_t blockId,
    const float* query, size_t ef, float frozenLB,
    std::priority_queue<std::pair<float, uint32_t>,
        std::vector<std::pair<float, uint32_t>>,
        std::less<std::pair<float, uint32_t>>>& top_candidates,
    std::priority_queue<std::pair<float, uint32_t>,
        std::vector<std::pair<float, uint32_t>>,
        std::greater<std::pair<float, uint32_t>>>& candidate_set,
    VisitedList& visited,
    const std::function<uint32_t(uint32_t)>& getBlockIdFast) {

    // 获取候选的 block (此时应在缓存中)
    CachedBlock* block = cache_->getCachedBlockById(blockId);
    if (!block) {
        // 异常: block 被淘汰, 回退到同步加载
        if (pq_enabled_) {
            // PQ 模式: 不需要向量, 只需邻居列表
            uint32_t neighborCount = 0;
            const uint32_t* neighbors = cache_->getNodeNeighbors(nodeId, neighborCount);
            if (!neighbors || neighborCount == 0) return;
            std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);
            for (uint32_t nid : local_neighbors) {
                if (nid >= graph_.num_nodes) continue;
                if (visited.isVisited(nid)) continue;
                visited.markVisited(nid);
                float dist = pqDistance(query, nid);
                if (top_candidates.size() < ef || frozenLB > dist) {
                    candidate_set.emplace(dist, nid);
                    top_candidates.emplace(dist, nid);
                    if (top_candidates.size() > ef) top_candidates.pop();
                }
            }
            return;
        }
        const float* vec = cache_->getNodeVector(nodeId);
        if (!vec) return;
        uint32_t neighborCount = 0;
        const uint32_t* neighbors = cache_->getNodeNeighbors(nodeId, neighborCount);
        if (!neighbors || neighborCount == 0) return;
        std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

        struct PendingNeighbor { uint32_t neighborId; uint32_t blockId; };
        std::vector<PendingNeighbor> pending;

        for (uint32_t nid : local_neighbors) {
            if (nid >= graph_.num_nodes) continue;
            if (visited.isVisited(nid)) continue;
            visited.markVisited(nid);
            uint32_t nb = getBlockIdFast(nid);
            CachedBlock* nBlock = cache_->getCachedBlockById(nb);
            if (nBlock) {
                const float* nvec = nBlock->getVector(nid);
                if (!nvec) continue;
                float dist = l2Distance(query, nvec);
                if (top_candidates.size() < ef || frozenLB > dist) {
                    candidate_set.emplace(dist, nid);
                    top_candidates.emplace(dist, nid);
                    if (top_candidates.size() > ef) top_candidates.pop();
                }
            } else {
                pending.push_back({nid, nb});
            }
        }
        if (!pending.empty()) {
            std::set<uint32_t> needed;
            for (const auto& pn : pending) needed.insert(pn.blockId);
            if (graph_prefetch_enabled_ && graph_prefetcher_)
                graph_prefetcher_->waitForBlocks(needed);
            for (const auto& pn : pending) {
                CachedBlock* nBlock = cache_->getCachedBlockById(pn.blockId);
                if (!nBlock) continue;
                const float* nvec = nBlock->getVector(pn.neighborId);
                if (!nvec) continue;
                float dist = l2Distance(query, nvec);
                if (top_candidates.size() < ef || frozenLB > dist) {
                    candidate_set.emplace(dist, pn.neighborId);
                    top_candidates.emplace(dist, pn.neighborId);
                    if (top_candidates.size() > ef) top_candidates.pop();
                }
            }
        }
        return;
    }

    // 快速路径: 从 CSR 内存或 CachedBlock 获取邻居
    uint32_t neighborCount = 0;
    const uint32_t* neighbors = nullptr;
    if (has_inmem_adjacency_) {
        neighbors = getInMemNeighbors(nodeId, neighborCount);
    }
    if (!neighbors) {
        neighbors = block->getNeighbors(nodeId, neighborCount);
    }
    if (!neighbors || neighborCount == 0) return;
    std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

    // 预取邻居 blocks (fire-and-forget, 延迟提交)
    if (graph_prefetch_enabled_ && graph_prefetcher_) {
        std::vector<uint32_t> prefetch_blocks;
        for (uint32_t nid : local_neighbors) {
            uint32_t nb = getBlockIdFast(nid);
            if (nb != blockId) prefetch_blocks.push_back(nb);
        }
        std::sort(prefetch_blocks.begin(), prefetch_blocks.end());
        prefetch_blocks.erase(std::unique(prefetch_blocks.begin(),
                              prefetch_blocks.end()), prefetch_blocks.end());
        if (!prefetch_blocks.empty())
            graph_prefetcher_->submitPrefetch(prefetch_blocks, true);  // 立即提交, 尽早启动 I/O
    }

    // 处理邻居: 用 frozenLB 过滤
    struct PendingNeighbor { uint32_t neighborId; uint32_t blockId; };
    std::vector<PendingNeighbor> pending;

    for (uint32_t nid : local_neighbors) {
        if (nid >= graph_.num_nodes) continue;
        if (visited.isVisited(nid)) continue;
        visited.markVisited(nid);
        if (pq_enabled_) {
            // PQ 模式: 不需要向量, 直接算 ADC 距离
            float dist = pqDistance(query, nid);
            // ★ 用 frozenLB 过滤, 不更新 lowerBound
            if (top_candidates.size() < ef || frozenLB > dist) {
                candidate_set.emplace(dist, nid);
                top_candidates.emplace(dist, nid);
                if (top_candidates.size() > ef) top_candidates.pop();
            }
        } else {
            uint32_t nb = getBlockIdFast(nid);
            CachedBlock* nBlock = cache_->getCachedBlockById(nb);
            if (nBlock) {
                const float* nvec = nBlock->getVector(nid);
                if (!nvec) continue;
                float dist = l2Distance(query, nvec);
                // ★ 用 frozenLB 过滤, 不更新 lowerBound
                if (top_candidates.size() < ef || frozenLB > dist) {
                    candidate_set.emplace(dist, nid);
                    top_candidates.emplace(dist, nid);
                    if (top_candidates.size() > ef) top_candidates.pop();
                }
            } else {
                pending.push_back({nid, nb});
            }
        }
    }

    // 处理 pending 邻居 (block miss)
    if (!pending.empty()) {
        std::set<uint32_t> needed;
        for (const auto& pn : pending) needed.insert(pn.blockId);
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            graph_prefetcher_->waitForBlocks(needed);
        }
        for (const auto& pn : pending) {
            CachedBlock* nBlock = cache_->getCachedBlockById(pn.blockId);
            if (!nBlock) continue;
            const float* nvec = nBlock->getVector(pn.neighborId);
            if (!nvec) continue;
            float dist = l2Distance(query, nvec);
            // ★ 用 frozenLB 过滤
            if (top_candidates.size() < ef || frozenLB > dist) {
                candidate_set.emplace(dist, pn.neighborId);
                top_candidates.emplace(dist, pn.neighborId);
                if (top_candidates.size() > ef) top_candidates.pop();
            }
        }
    }
}

std::priority_queue<std::pair<float, uint32_t>,
                    std::vector<std::pair<float, uint32_t>>,
                    std::greater<std::pair<float, uint32_t>>>
DiskHNSW::searchLayer0Beam(uint32_t entry_new_id, const float* query, size_t ef,
                            VisitedList& visited, int beam_width) {
    // 最大堆: top candidates (距离大的在堆顶, 方便淘汰)
    std::priority_queue<std::pair<float, uint32_t>,
        std::vector<std::pair<float, uint32_t>>,
        std::less<std::pair<float, uint32_t>>> top_candidates;
    // 最小堆: candidate set (距离小的在堆顶, 优先展开)
    std::priority_queue<std::pair<float, uint32_t>,
        std::vector<std::pair<float, uint32_t>>,
        std::greater<std::pair<float, uint32_t>>> candidate_set;

    auto getBlockIdFast = [&](uint32_t node_id) -> uint32_t {
        if (route_table_) return (*route_table_)[node_id];
        return cache_->getBlockId(node_id);
    };

    // 初始化: 入口节点
    float entryDist;
    if (pq_enabled_) {
        entryDist = pqDistance(query, entry_new_id);
    } else {
        const float* entryVec = cache_->getNodeVector(entry_new_id);
        if (!entryVec) {
            std::cerr << "[DiskHNSW] ERROR: Failed to get vector for entry node " << entry_new_id << std::endl;
            return candidate_set;
        }
        entryDist = l2Distance(query, entryVec);
    }
    top_candidates.emplace(entryDist, entry_new_id);
    candidate_set.emplace(entryDist, entry_new_id);
    visited.markVisited(entry_new_id);
    float lowerBound = entryDist;

    while (!candidate_set.empty()) {
        // ===== Phase 1: 取 beam (最多 B 个候选) =====
        float frozenLB = lowerBound;  // ★ 冻结 lowerBound

        struct BeamItem {
            float dist;
            uint32_t nodeId;
            uint32_t blockId;
            bool inCache;
        };
        std::vector<BeamItem> beam;

        while ((int)beam.size() < beam_width && !candidate_set.empty()) {
            auto [dist, nodeId] = candidate_set.top();
            // 终止条件: 搜索收敛
            if (dist > frozenLB && top_candidates.size() == ef) {
                // 这个候选超出范围, 搜索可以结束
                goto search_done;
            }
            candidate_set.pop();
            uint32_t blockId = getBlockIdFast(nodeId);
            // 用 peek (不更新 LRU) 检查 cache 状态
            CachedBlock* blk = cache_->peekCachedBlockById(blockId);
            beam.push_back({dist, nodeId, blockId, blk != nullptr});
        }

        if (beam.empty()) break;

        // ===== Phase 2: 分类 + 批量 I/O 提交 =====
        std::vector<int> hitIdx, missIdx;
        std::vector<uint32_t> missBlocks;

        for (int i = 0; i < (int)beam.size(); i++) {
            if (beam[i].inCache) {
                hitIdx.push_back(i);
            } else {
                missIdx.push_back(i);
                missBlocks.push_back(beam[i].blockId);
            }
        }

        // 去重 miss blocks
        std::sort(missBlocks.begin(), missBlocks.end());
        missBlocks.erase(std::unique(missBlocks.begin(), missBlocks.end()),
                         missBlocks.end());

        // 批量提交 I/O
        if (!missBlocks.empty() && graph_prefetch_enabled_ && graph_prefetcher_) {
            graph_prefetcher_->submitPrefetch(missBlocks, true);  // async + auto submit
        }

        // ===== Phase 3: 展开 hit 候选 (I/O 在途, CPU 不空闲) =====
        for (int idx : hitIdx) {
            if (heat_evaluator_) heat_evaluator_->onBlockAccess(beam[idx].blockId);
            expandBeamCandidate(beam[idx].nodeId, beam[idx].blockId,
                                query, ef, frozenLB,
                                top_candidates, candidate_set,
                                visited, getBlockIdFast);
        }

        // ===== Phase 4: 等待 I/O + 展开 miss 候选 =====
        if (!missIdx.empty()) {
            if (graph_prefetch_enabled_ && graph_prefetcher_ && !missBlocks.empty()) {
                std::set<uint32_t> needed(missBlocks.begin(), missBlocks.end());
                graph_prefetcher_->waitForBlocks(needed);
            }
            for (int idx : missIdx) {
                if (heat_evaluator_) heat_evaluator_->onBlockAccess(beam[idx].blockId);
                expandBeamCandidate(beam[idx].nodeId, beam[idx].blockId,
                                    query, ef, frozenLB,
                                    top_candidates, candidate_set,
                                    visited, getBlockIdFast);
            }
        }

        // ===== Phase 5: 更新 lowerBound =====
        if (!top_candidates.empty()) {
            lowerBound = top_candidates.top().first;
        }
    }

search_done:
    // 转换 top_candidates 为最小堆返回
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
// 批量并行 I/O 搜索 (Batch I/O)
//
// 核心优化: 取 candidate queue 的 top-N, 批量收集所有未访问邻居,
// 一次性提交 io_uring (~200 个 I/O), 并行返回后批量算距离.
// 相比逐个 candidate 展开, I/O 并行度从 1 提升到 ~N×22.
//
// 使用 frozenLB (冻结 lowerBound) 保证 recall 100%:
// 在一个 batch 内不更新 lowerBound, 可能多保留一些 candidate,
// 但不会丢掉任何应该保留的.
// ============================================================

std::priority_queue<std::pair<float, uint32_t>,
                    std::vector<std::pair<float, uint32_t>>,
                    std::greater<std::pair<float, uint32_t>>>
DiskHNSW::searchLayer0BatchIO(uint32_t entry_new_id, const float* query, size_t ef,
                              VisitedList& visited, int batch_size) {
    // 最大堆: top candidates (距离大的在堆顶, 方便淘汰)
    std::priority_queue<std::pair<float, uint32_t>,
        std::vector<std::pair<float, uint32_t>>,
        std::less<std::pair<float, uint32_t>>> top_candidates;
    // 最小堆: candidate set (距离小的在堆顶, 优先展开)
    std::priority_queue<std::pair<float, uint32_t>,
        std::vector<std::pair<float, uint32_t>>,
        std::greater<std::pair<float, uint32_t>>> candidate_set;

    auto getBlockIdFast = [&](uint32_t node_id) -> uint32_t {
        if (route_table_) return (*route_table_)[node_id];
        return cache_->getBlockId(node_id);
    };

    // 初始化: 入口节点
    float entryDist;
    if (pq_enabled_) {
        entryDist = pqDistance(query, entry_new_id);
    } else {
        const float* entryVec = cache_->getNodeVector(entry_new_id);
        if (!entryVec) {
            std::cerr << "[DiskHNSW] ERROR: Failed to get vector for entry node " << entry_new_id << std::endl;
            return candidate_set;
        }
        entryDist = l2Distance(query, entryVec);
    }
    top_candidates.emplace(entryDist, entry_new_id);
    candidate_set.emplace(entryDist, entry_new_id);
    visited.markVisited(entry_new_id);
    float lowerBound = entryDist;

    while (!candidate_set.empty()) {
        // ===== Phase 1: 取 batch (最多 batch_size 个候选) =====
        float frozenLB = lowerBound;  // 冻结 lowerBound, 保证 recall

        struct BatchCandidate {
            float dist;
            uint32_t nodeId;
            uint32_t blockId;
        };
        std::vector<BatchCandidate> batch;

        while ((int)batch.size() < batch_size && !candidate_set.empty()) {
            auto [dist, nodeId] = candidate_set.top();
            // 终止条件: 搜索收敛
            if (dist > frozenLB && top_candidates.size() == ef) {
                goto search_done;
            }
            candidate_set.pop();
            uint32_t blockId = getBlockIdFast(nodeId);
            batch.push_back({dist, nodeId, blockId});
        }

        if (batch.empty()) break;

        // ===== Phase 2: 确保所有候选 block 已加载 =====
        // 收集不在缓存的候选 block, 批量提交 I/O
        std::vector<uint32_t> cand_miss_blocks;
        for (auto& bc : batch) {
            if (!cache_->peekCachedBlockById(bc.blockId)) {
                cand_miss_blocks.push_back(bc.blockId);
            }
        }
        std::sort(cand_miss_blocks.begin(), cand_miss_blocks.end());
        cand_miss_blocks.erase(
            std::unique(cand_miss_blocks.begin(), cand_miss_blocks.end()),
            cand_miss_blocks.end());

        if (!cand_miss_blocks.empty()) {
            if (graph_prefetch_enabled_ && graph_prefetcher_) {
                graph_prefetcher_->submitPrefetch(cand_miss_blocks, true);
                std::set<uint32_t> needed(cand_miss_blocks.begin(),
                                          cand_miss_blocks.end());
                graph_prefetcher_->waitForBlocks(needed);
            } else {
                for (uint32_t b : cand_miss_blocks) cache_->getBlockById(b);
            }
        }

        // ===== Phase 3: 展开所有候选, 收集未访问邻居 =====
        // in-cache 邻居: 立即算距离
        // out-of-cache 邻居: 收集到 pending 列表
        struct PendingNbr {
            uint32_t neighborId;
            uint32_t blockId;
        };
        std::vector<PendingNbr> pending;
        std::vector<uint32_t> pending_blocks;  // for batch I/O

        for (auto& bc : batch) {
            CachedBlock* blk = cache_->getCachedBlockById(bc.blockId);
            if (!blk) continue;
            if (heat_evaluator_) heat_evaluator_->onBlockAccess(bc.blockId);

            uint32_t neighborCount = 0;
            const uint32_t* neighbors = nullptr;
            if (has_inmem_adjacency_) {
                neighbors = getInMemNeighbors(bc.nodeId, neighborCount);
            }
            if (!neighbors) {
                neighbors = blk->getNeighbors(bc.nodeId, neighborCount);
            }
            if (!neighbors || neighborCount == 0) continue;

            for (uint32_t j = 0; j < neighborCount; j++) {
                uint32_t neighborId = neighbors[j];
                if (neighborId >= graph_.num_nodes) continue;
                if (visited.isVisited(neighborId)) continue;
                visited.markVisited(neighborId);

                uint32_t nb = getBlockIdFast(neighborId);
                if (pq_enabled_) {
                    // PQ 模式: 不需要向量, 直接算 ADC 距离
                    float d = pqDistance(query, neighborId);
                    if (top_candidates.size() < ef || frozenLB > d) {
                        candidate_set.emplace(d, neighborId);
                        top_candidates.emplace(d, neighborId);
                        if (top_candidates.size() > ef) top_candidates.pop();
                    }
                } else {
                    CachedBlock* nblk = cache_->getCachedBlockById(nb);
                    if (nblk) {
                        // in-cache: 立即算距离
                        const float* nvec = nblk->getVector(neighborId);
                        if (!nvec) continue;
                        float d = l2Distance(query, nvec);
                        if (top_candidates.size() < ef || frozenLB > d) {
                            candidate_set.emplace(d, neighborId);
                            top_candidates.emplace(d, neighborId);
                            if (top_candidates.size() > ef) top_candidates.pop();
                        }
                    } else {
                        // out-of-cache: 收集
                        pending.push_back({neighborId, nb});
                        pending_blocks.push_back(nb);
                    }
                }
            }
        }

        // ===== Phase 4: 批量提交所有邻居 block I/O =====
        // 一次提交 ~200 个 I/O, io_uring 并行处理
        std::sort(pending_blocks.begin(), pending_blocks.end());
        pending_blocks.erase(
            std::unique(pending_blocks.begin(), pending_blocks.end()),
            pending_blocks.end());

        if (!pending_blocks.empty()) {
            if (graph_prefetch_enabled_ && graph_prefetcher_) {
                graph_prefetcher_->submitPrefetch(pending_blocks, true);
                std::set<uint32_t> needed(pending_blocks.begin(),
                                          pending_blocks.end());
                graph_prefetcher_->waitForBlocks(needed);
            } else {
                for (uint32_t b : pending_blocks) cache_->getBlockById(b);
            }
        }

        // ===== Phase 5: 批量计算 pending 邻居距离 =====
        for (auto& p : pending) {
            if (pq_enabled_) {
                float d = pqDistance(query, p.neighborId);
                if (top_candidates.size() < ef || frozenLB > d) {
                    candidate_set.emplace(d, p.neighborId);
                    top_candidates.emplace(d, p.neighborId);
                    if (top_candidates.size() > ef) top_candidates.pop();
                }
            } else {
                CachedBlock* nblk = cache_->getCachedBlockById(p.blockId);
                if (!nblk) continue;
                const float* nvec = nblk->getVector(p.neighborId);
                if (!nvec) continue;
                float d = l2Distance(query, nvec);
                if (top_candidates.size() < ef || frozenLB > d) {
                    candidate_set.emplace(d, p.neighborId);
                    top_candidates.emplace(d, p.neighborId);
                    if (top_candidates.size() > ef) top_candidates.pop();
                }
            }
        }

        // ===== Phase 6: 更新 lowerBound =====
        if (!top_candidates.empty()) {
            lowerBound = top_candidates.top().first;
        }
    }

search_done:
    // 转换 top_candidates 为最小堆返回
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

    // 环境变量 BEAM_WIDTH 控制 beam search (0=标准 best-first, >0=beam search)
    static const int kBeamWidth = []() {
        const char* e = std::getenv("BEAM_WIDTH");
        return e ? std::atoi(e) : 0;
    }();

    // 环境变量 NONBLOCK 控制非阻塞搜索 (1=非阻塞, 0=阻塞)
    static const int kNonBlock = []() {
        const char* e = std::getenv("NONBLOCK");
        return e ? std::atoi(e) : 0;
    }();

    // 环境变量 BATCH_IO_N 控制批量并行 I/O 搜索 (0=关闭, >0=batch size)
    static const int kBatchIO_N = []() {
        const char* e = std::getenv("BATCH_IO_N");
        return e ? std::atoi(e) : 0;
    }();

    // 环境变量 TWO_STAGE 控制两阶段搜索 (1=PQ粗筛+精确精排, 0=关闭)
    static const int kTwoStage = []() {
        const char* e = std::getenv("TWO_STAGE");
        return e ? std::atoi(e) : 0;
    }();

    // 环境变量 REFINE_EF 控制两阶段粗筛 ef (默认 200)
    static const int kRefineEf = []() {
        const char* e = std::getenv("REFINE_EF");
        return e ? std::atoi(e) : 200;
    }();

    std::priority_queue<std::pair<float, uint32_t>,
        std::vector<std::pair<float, uint32_t>>,
        std::greater<std::pair<float, uint32_t>>> top_candidates;

    if (kTwoStage && pq_enabled_) {
        // 计时插桩 (环境变量 PROFILE_TS=1 输出分解)
        static const bool kProfile = std::getenv("PROFILE_TS") && std::atoi(std::getenv("PROFILE_TS")) != 0;
        static double acc_a = 0, acc_wait = 0, acc_rerank = 0;
        static long acc_n = 0;
        auto tp0 = std::chrono::high_resolution_clock::now();

        // === Phase A: PQ 粗筛 (ADC 距离, 无向量 I/O) ===
        size_t ef_coarse = std::max(ef, (size_t)kRefineEf);
        auto coarse = searchLayer0(entryNewId, query, ef_coarse, visited);
        auto tpA = std::chrono::high_resolution_clock::now();

        std::vector<uint32_t> cand_ids;
        cand_ids.reserve(coarse.size());
        while (!coarse.empty()) {
            cand_ids.push_back(coarse.top().second);
            coarse.pop();
        }

        // === Phase B: 精确距离精排 ===
        static const bool kFineRerank = std::getenv("FINE_RERANK") && std::atoi(std::getenv("FINE_RERANK")) != 0;
        if (kFineRerank) {
            // 懒初始化 (VEC_BLOCKS_PATH 或复用 cache blocks 路径)
            if (!fine_rerank_ok_) {
                const char* bp = std::getenv("VEC_BLOCKS_PATH");
                if (bp) fine_rerank_ok_ = buildFineRerank(bp, graph_.num_nodes);
                if (!fine_rerank_ok_) {
                    std::cerr << "[FineRerank] init failed, fallback to block rerank" << std::endl;
                }
            }
        }

        if (kFineRerank && fine_rerank_ok_) {
            // ---- 4KB 页粒度精排: cache hit 取向量, miss 按 4KB 页批量 io_uring 读 ----
            static const bool kProfFine = std::getenv("PROFILE_FINE") && std::atoi(std::getenv("PROFILE_FINE")) != 0;
            static double pf_collect = 0, pf_submit = 0, pf_first = 0, pf_iorest = 0, pf_compute = 0;
            static long pf_pages = 0, pf_cached = 0, pf_iters = 0, pf_n = 0;
            auto tf0 = std::chrono::high_resolution_clock::now();

            std::priority_queue<std::pair<float, uint32_t>> refined;
            auto consider = [&](uint32_t nid, const float* vec) {
                float d = l2Distance(query, vec);
                if (refined.size() < k) refined.emplace(d, nid);
                else if (d < refined.top().first) { refined.pop(); refined.emplace(d, nid); }
            };

            // 收集 miss 候选的 4KB 页 (注意: data_offset=520 非512对齐, slot%8==6 的向量跨页!)
            struct CandIO { uint32_t nid; uint32_t page0; uint16_t oip; bool cross; };
            std::vector<CandIO> io_cands;
            std::set<uint32_t> pages_needed;
            for (uint32_t nid : cand_ids) {
                uint32_t b = route_table_ ? (*route_table_)[nid] : cache_->getBlockId(nid);
                // 只查 cache 不触发加载 (getNodeVector miss 会同步读 64KB block!)
                if (CachedBlock* cb = cache_->getCachedBlockById(b)) {
                    if (const float* v = cb->getVector(nid)) { consider(nid, v); continue; }
                }
                uint64_t off = 4096ull + (uint64_t)b * vec_block_size_
                             + block_data_offset_[b]
                             + (uint64_t)node_slot_table_[nid] * dim_ * sizeof(float);
                uint32_t page0 = (uint32_t)(off >> 12);
                uint16_t oip = (uint16_t)(off & 4095);
                bool cross = (oip + dim_ * sizeof(float)) > 4096;
                io_cands.push_back({nid, page0, oip, cross});
                pages_needed.insert(page0);
                if (cross) pages_needed.insert(page0 + 1);
            }

            // 提交所有页读
            auto tf1 = std::chrono::high_resolution_clock::now();
            std::unordered_map<uint32_t, int> page_buf;
            page_buf.reserve(pages_needed.size());
            for (uint32_t page : pages_needed) {
                int buf = vec_ring_->allocBuffer();
                if (buf < 0) continue;  // 兜底: 后面统一同步 pread (不应发生, pool 256>116)
                vec_ring_->submitRead(vec_blocks_fd_, (off_t)page << 12, 4096, buf, page);
                page_buf[page] = buf;
            }
            vec_ring_->submit();
            auto tf2 = std::chrono::high_resolution_clock::now();

            // 等全部完成
            bool first_cqe_done = false;
            double first_cqe_us = 0;
            size_t done = 0;
            const size_t total = page_buf.size();
            std::vector<IoUring::CqeResult> results;
            long wait_iters = 0;
            while (done < total) {
                vec_ring_->waitCompletion();
                if (!first_cqe_done) {
                    first_cqe_us = std::chrono::duration<double, std::micro>(
                        std::chrono::high_resolution_clock::now() - tf2).count();
                    first_cqe_done = true;
                }
                wait_iters++;
                results.clear();
                vec_ring_->reapCompletions(results);
                done += results.size();
                for (const auto& cqe : results) {
                    if (cqe.res != 4096) page_buf[(uint32_t)cqe.user_data] = -1;  // 标记失败
                }
            }
            auto tf3 = std::chrono::high_resolution_clock::now();

            // 统一算距离 (跨页拼两页)
            char tmp_vec[512];
            for (const auto& c : io_cands) {
                auto it0 = page_buf.find(c.page0);
                if (it0 == page_buf.end() || it0->second < 0) continue;
                const char* p0 = (const char*)vec_ring_->getBuffer(it0->second);
                const float* vec;
                if (!c.cross) {
                    vec = reinterpret_cast<const float*>(p0 + c.oip);
                } else {
                    auto it1 = page_buf.find(c.page0 + 1);
                    if (it1 == page_buf.end() || it1->second < 0) continue;
                    const char* p1 = (const char*)vec_ring_->getBuffer(it1->second);
                    size_t first = 4096 - c.oip;
                    std::memcpy(tmp_vec, p0 + c.oip, first);
                    std::memcpy(tmp_vec + first, p1, dim_ * sizeof(float) - first);
                    vec = reinterpret_cast<const float*>(tmp_vec);
                }
                consider(c.nid, vec);
                cache_->putFlatVector(c.nid, vec);  // 回填热向量 cache, Phase A hybrid 用
            }

            // 释放 buffer
            for (auto& [page, buf] : page_buf) {
                if (buf >= 0) vec_ring_->freeBuffer(buf);
            }

            if (kProfFine) {
                auto tf4 = std::chrono::high_resolution_clock::now();
                using us = std::chrono::duration<double, std::micro>;
                pf_collect += us(tf1 - tf0).count();
                pf_submit += us(tf2 - tf1).count();
                pf_first += first_cqe_us;
                pf_iorest += us(tf3 - tf2).count() - first_cqe_us;
                pf_compute += us(tf4 - tf3).count();
                pf_pages += page_buf.size();
                pf_cached += (cand_ids.size() - io_cands.size());
                pf_iters += wait_iters;
                pf_n++;
                if (pf_n % 200 == 0) {
                    fprintf(stderr,
                        "[PROFILE_FINE] n=%ld collect=%.0f submit=%.0f io_1st=%.0f io_rest=%.0f compute=%.0f | pages=%.1f cached=%.1f iters=%.1f\n",
                        pf_n, pf_collect/pf_n, pf_submit/pf_n, pf_first/pf_n, pf_iorest/pf_n, pf_compute/pf_n,
                        (double)pf_pages/pf_n, (double)pf_cached/pf_n, (double)pf_iters/pf_n);
                }
            }

            while (!refined.empty()) {
                top_candidates.push(refined.top());
                refined.pop();
            }

            if (kProfile) {
                auto tp2 = std::chrono::high_resolution_clock::now();
                double a = std::chrono::duration<double, std::micro>(tpA - tp0).count();
                double w = std::chrono::duration<double, std::micro>(tp2 - tpA).count();
                acc_a += a; acc_wait += w; acc_n++;
                if (acc_n % 200 == 0) {
                    fprintf(stderr, "[PROFILE_TS] n=%ld PhaseA=%.0fus FineIO+rerank=%.0fus\n",
                            acc_n, acc_a/acc_n, acc_wait/acc_n);
                }
            }
        } else {
        // ---- 块粒度精排 (原路径) ----
        // 收集 miss 的 blocks, 一次性提交 io_uring 并行读 (NVMe 队列深度掩盖 I/O 延迟)
        std::set<uint32_t> needed_blocks;
        for (uint32_t nid : cand_ids) {
            uint32_t b = route_table_ ? (*route_table_)[nid] : cache_->getBlockId(nid);
            if (!cache_->isInCache(b)) needed_blocks.insert(b);
        }
        if (!needed_blocks.empty() && graph_prefetcher_) {
            std::vector<uint32_t> bv(needed_blocks.begin(), needed_blocks.end());
            graph_prefetcher_->submitPrefetch(bv, true);
            graph_prefetcher_->waitForBlocks(needed_blocks);
        }
        auto tp1 = std::chrono::high_resolution_clock::now();

        // 精确 L2 重排, 最大堆保持 top-k
        std::priority_queue<std::pair<float, uint32_t>> refined;
        for (uint32_t nid : cand_ids) {
            const float* vec = cache_->getNodeVector(nid);
            if (!vec) continue;
            float d = l2Distance(query, vec);
            if (refined.size() < k) {
                refined.emplace(d, nid);
            } else if (d < refined.top().first) {
                refined.pop();
                refined.emplace(d, nid);
            }
        }

        // 转为最小堆返回 (与接口一致)
        while (!refined.empty()) {
            top_candidates.push(refined.top());
            refined.pop();
        }

        if (kProfile) {
            auto tp2 = std::chrono::high_resolution_clock::now();
            double a = std::chrono::duration<double, std::micro>(tpA - tp0).count();
            double w = std::chrono::duration<double, std::micro>(tp1 - tpA).count();
            double r = std::chrono::duration<double, std::micro>(tp2 - tp1).count();
            acc_a += a; acc_wait += w; acc_rerank += r; acc_n++;
            if (acc_n % 200 == 0) {
                fprintf(stderr, "[PROFILE_TS] n=%ld PhaseA=%.0fus IOwait=%.0fus rerank=%.0fus\n",
                        acc_n, acc_a/acc_n, acc_wait/acc_n, acc_rerank/acc_n);
            }
        }
        }  // end else (块粒度精排)
    } else if (kBatchIO_N > 1) {
        top_candidates = searchLayer0BatchIO(entryNewId, query, ef, visited, kBatchIO_N);
    } else if (kBeamWidth > 1) {
        top_candidates = searchLayer0Beam(entryNewId, query, ef, visited, kBeamWidth);
    } else if (kNonBlock) {
        top_candidates = searchLayer0NonBlocking(entryNewId, query, ef, visited);
    } else {
        top_candidates = searchLayer0(entryNewId, query, ef, visited);
    }

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

// ============================================================
// 细粒度精排读 (FINE_RERANK): 候选向量 4KB 页读
// ============================================================
bool DiskHNSW::buildFineRerank(const std::string& blocks_path, uint32_t num_nodes) {
    // 懒构建: node→slot 表 + block→data_offset 表 + 4KB io_uring
    // FINE_BUFFERED=1 时用 buffered I/O 吃 page cache (热区页零 I/O)
    static const bool kFineBuffered = std::getenv("FINE_BUFFERED") && std::atoi(std::getenv("FINE_BUFFERED")) != 0;
    int fd = -1;
    if (kFineBuffered) {
        fd = open(blocks_path.c_str(), O_RDONLY);
        std::cerr << "[FineRerank] buffered mode (page cache)" << std::endl;
    }
    if (fd < 0) fd = open(blocks_path.c_str(), O_RDONLY | O_DIRECT);
    if (fd < 0) fd = open(blocks_path.c_str(), O_RDONLY);  // fallback buffered
    if (fd < 0) {
        std::cerr << "[FineRerank] open failed: " << blocks_path << std::endl;
        return false;
    }

    // 读文件头 (pad 到 4096)
    char hdr_buf[4096];
    if (pread(fd, hdr_buf, 4096, 0) != 4096) { close(fd); return false; }
    uint32_t block_size, num_blocks;
    std::memcpy(&block_size, hdr_buf + 8, 4);
    std::memcpy(&num_blocks, hdr_buf + 12, 4);
    vec_block_size_ = block_size;

    node_slot_table_.assign(num_nodes, 0);
    block_data_offset_.assign(num_blocks, 0);

    // 每 block 读 header(16B) + node_ids(4B×cnt), 建 slot 表
    std::vector<char> buf(4096);
    auto t0 = std::chrono::high_resolution_clock::now();
    for (uint32_t b = 0; b < num_blocks; b++) {
        off_t off = (off_t)4096 + (off_t)b * block_size;
        ssize_t r = pread(fd, buf.data(), 4096, off);  // header+ids 都在前 4KB
        if (r < 16) { close(fd); return false; }
        uint32_t cnt, data_offset, flags;
        std::memcpy(&cnt, buf.data() + 4, 4);
        std::memcpy(&data_offset, buf.data() + 8, 4);
        std::memcpy(&flags, buf.data() + 12, 4);
        if (!(flags & FLAG_VEC_ONLY)) { close(fd); return false; }  // 仅支持 vec-only
        block_data_offset_[b] = data_offset;
        const uint32_t* ids = reinterpret_cast<const uint32_t*>(buf.data() + 16);
        uint32_t max_cnt = (4096 - 16) / 4;
        if (cnt > max_cnt) { close(fd); return false; }  // 超出 4KB 窗口(cnt≤126 不会发生)
        for (uint32_t i = 0; i < cnt; i++) {
            if (ids[i] < num_nodes) node_slot_table_[ids[i]] = (uint16_t)i;
        }
    }

    try {
        vec_ring_ = std::make_unique<IoUring>(256);
        vec_ring_->setBufferSize(4096);
    } catch (const std::exception& e) {
        std::cerr << "[FineRerank] io_uring init failed: " << e.what() << std::endl;
        close(fd);
        return false;
    }
    vec_blocks_fd_ = fd;

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cerr << "[FineRerank] slot table built: " << num_blocks << " blocks, "
              << ms << " ms" << std::endl;
    return true;
}

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

GraphPrefetcher::Stats DiskHNSW::getGraphPrefetchStats() const {
    static const GraphPrefetcher::Stats empty_stats;
    if (graph_prefetcher_) return graph_prefetcher_->getStats();
    return empty_stats;
}

void DiskHNSW::resetGraphPrefetchStats() {
    if (graph_prefetcher_) graph_prefetcher_->resetStats();
}

// ============================================================
// 事件驱动批量搜索 (单线程多查询并发)
// ============================================================

void DiskHNSW::initQueryState(QueryState& qs, const float* query, size_t k, size_t ef) {
    qs.query = query;
    qs.k = k;
    qs.ef = ef;
    qs.visited = std::make_unique<VisitedList>(graph_.num_nodes);
    qs.done = false;
    qs.waitingBlockId = 0;
    qs.entryLoaded = false;
    qs.lowerBound = std::numeric_limits<float>::max();

    // Phase 1: 贪心下降
    uint32_t entryOldId = greedyDescent(query);
    qs.entryNewId = old_to_new_[entryOldId];
}

void DiskHNSW::stepQueryState(QueryState& qs) {
    // 如果还没加载入口节点
    if (!qs.entryLoaded) {
        uint32_t entryBlock = route_table_ ? (*route_table_)[qs.entryNewId]
                                           : cache_->getBlockId(qs.entryNewId);
        // 先 peek 检查是否在缓存 (不更新 LRU)
        if (!cache_->peekCachedBlockById(entryBlock)) {
            if (graph_prefetch_enabled_ && graph_prefetcher_) {
                graph_prefetcher_->submitPrefetch({entryBlock}, true);
            }
            qs.waitingBlockId = entryBlock;
            return;
        }
        // 用 getCachedBlockById 获取 (更新 LRU)
        CachedBlock* blk = cache_->getCachedBlockById(entryBlock);
        float entryDist;
        if (pq_enabled_) {
            entryDist = pqDistance(qs.query, qs.entryNewId);
        } else {
            const float* entryVec = nullptr;
            if (!blk) {
                entryVec = cache_->getNodeVector(qs.entryNewId);
                if (!entryVec) {
                    qs.done = true;
                    return;
                }
            } else {
                entryVec = blk->getVector(qs.entryNewId);
            }
            if (!entryVec) {
                qs.done = true;
                return;
            }
            entryDist = l2Distance(qs.query, entryVec);
        }
        qs.top_candidates.emplace(entryDist, qs.entryNewId);
        qs.candidate_set.emplace(entryDist, qs.entryNewId);
        qs.visited->markVisited(qs.entryNewId);
        qs.lowerBound = entryDist;
        qs.entryLoaded = true;
    }

    // 如果在等待 block, 检查是否已就绪
    if (qs.waitingBlockId) {
        if (cache_->peekCachedBlockById(qs.waitingBlockId)) {
            qs.waitingBlockId = 0;
        } else {
            return;
        }
    }

    // ---- Phase 0: 处理 deferred 邻居 ----
    if (!qs.deferred.empty()) {
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            graph_prefetcher_->reapCompletions();
        }

        std::vector<QueryState::DeferredNeighbor> still_deferred;
        for (auto& dn : qs.deferred) {
            if (pq_enabled_) {
                // PQ 模式: 不需要 block 中的向量
                float dist = pqDistance(qs.query, dn.neighborId);
                if (qs.top_candidates.size() < qs.ef || qs.lowerBound > dist) {
                    qs.candidate_set.emplace(dist, dn.neighborId);
                    qs.top_candidates.emplace(dist, dn.neighborId);
                    if (qs.top_candidates.size() > qs.ef) qs.top_candidates.pop();
                    if (!qs.top_candidates.empty()) qs.lowerBound = qs.top_candidates.top().first;
                }
            } else {
                // 用 getCachedBlockById 更新 LRU
                CachedBlock* nBlock = cache_->getCachedBlockById(dn.blockId);
                if (nBlock) {
                    const float* neighborVec = nBlock->getVector(dn.neighborId);
                    if (neighborVec) {
                        float dist = l2Distance(qs.query, neighborVec);
                        if (qs.top_candidates.size() < qs.ef || qs.lowerBound > dist) {
                            qs.candidate_set.emplace(dist, dn.neighborId);
                            qs.top_candidates.emplace(dist, dn.neighborId);
                            if (qs.top_candidates.size() > qs.ef) qs.top_candidates.pop();
                            if (!qs.top_candidates.empty()) qs.lowerBound = qs.top_candidates.top().first;
                        }
                    }
                } else {
                    still_deferred.push_back(dn);
                }
            }
        }
        qs.deferred = std::move(still_deferred);
    }

    // 如果 candidate_set 为空且 deferred 非空, 需要等待 I/O
    if (qs.candidate_set.empty()) {
        if (qs.deferred.empty()) {
            qs.done = true;
            return;
        }
        std::vector<uint32_t> need_prefetch;
        for (const auto& dn : qs.deferred) {
            if (!cache_->isInCache(dn.blockId)) {
                need_prefetch.push_back(dn.blockId);
            }
        }
        if (!need_prefetch.empty() && graph_prefetch_enabled_ && graph_prefetcher_) {
            graph_prefetcher_->submitPrefetch(need_prefetch, true);
        }
        qs.waitingBlockId = qs.deferred[0].blockId;
        return;
    }

    // 弹出候选
    auto [candidateDist, candidateId] = qs.candidate_set.top();

    if (candidateDist > qs.lowerBound && qs.top_candidates.size() == qs.ef) {
        qs.done = true;
        return;
    }
    qs.candidate_set.pop();

    uint32_t curr_block_id = route_table_ ? (*route_table_)[candidateId]
                                          : cache_->getBlockId(candidateId);
    // peek 检查 (调度决策, 不更新 LRU)
    if (!cache_->peekCachedBlockById(curr_block_id)) {
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            graph_prefetcher_->submitPrefetch({curr_block_id}, true);
        }
        qs.waitingBlockId = curr_block_id;
        qs.candidate_set.emplace(candidateDist, candidateId);
        return;
    }

    // getCachedBlockById 获取数据 (更新 LRU)
    CachedBlock* candidateBlock = cache_->getCachedBlockById(curr_block_id);
    if (!candidateBlock) {
        // 被 evict 了, 回退
        if (graph_prefetch_enabled_ && graph_prefetcher_) {
            graph_prefetcher_->submitPrefetch({curr_block_id}, true);
        }
        qs.waitingBlockId = curr_block_id;
        qs.candidate_set.emplace(candidateDist, candidateId);
        return;
    }

    if (heat_evaluator_) heat_evaluator_->onBlockAccess(curr_block_id);

    // 优先使用内存 CSR 邻接表 (无需从 block 解码)
    uint32_t neighborCount = 0;
    const uint32_t* neighbors = nullptr;
    if (has_inmem_adjacency_) {
        neighbors = getInMemNeighbors(candidateId, neighborCount);
    }
    if (!neighbors) {
        neighbors = candidateBlock->getNeighbors(candidateId, neighborCount);
    }
    if (!neighbors || neighborCount == 0) return;
    std::vector<uint32_t> local_neighbors(neighbors, neighbors + neighborCount);

    // 提交 1-hop 预取 + multi-hop 预取 (使用内存 CSR 邻接表)
    // MULTIHOP_DEPTH 环境变量控制: 0=关闭, 1=仅1-hop, 2=1+2-hop (默认 2)
    static const int multihop_depth = [](){
        const char* e = std::getenv("MULTIHOP_DEPTH");
        return e ? std::atoi(e) : 2;
    }();
    if (graph_prefetch_enabled_ && graph_prefetcher_) {
        std::vector<uint32_t> prefetch_blocks;  // 1-hop
        std::vector<uint32_t> multi_hop_blocks; // 2+ hop
        
        for (uint32_t nid : local_neighbors) {
            uint32_t nb = route_table_ ? (*route_table_)[nid] : cache_->getBlockId(nid);
            if (nb != curr_block_id) {
                prefetch_blocks.push_back(nb);
                
                // Multi-hop: 如果该邻居的 block 不在缓存 (cache miss),
                // 用内存 CSR 邻接表读取它的邻居, 预取更远的 block
                if (multihop_depth >= 2 && has_inmem_adjacency_ && !cache_->peekCachedBlockById(nb)) {
                    uint32_t nn_count = 0;
                    const uint32_t* nn = getInMemNeighbors(nid, nn_count);
                    if (nn) {
                        // 只预取前 8 个 2-hop 邻居的 block (避免 io_uring 淹没)
                        int hop2_count = 0;
                        for (uint32_t k2 = 0; k2 < nn_count && hop2_count < 8; k2++) {
                            uint32_t nb2 = route_table_ ? (*route_table_)[nn[k2]]
                                                        : cache_->getBlockId(nn[k2]);
                            if (nb2 != curr_block_id && nb2 != nb) {
                                multi_hop_blocks.push_back(nb2);
                                hop2_count++;
                            }
                        }
                    }
                }
            }
        }
        
        // 1-hop 预取
        std::sort(prefetch_blocks.begin(), prefetch_blocks.end());
        prefetch_blocks.erase(std::unique(prefetch_blocks.begin(), prefetch_blocks.end()),
                              prefetch_blocks.end());
        if (!prefetch_blocks.empty())
            graph_prefetcher_->submitPrefetch(prefetch_blocks, true);
        
        // Multi-hop 预取
        std::sort(multi_hop_blocks.begin(), multi_hop_blocks.end());
        multi_hop_blocks.erase(std::unique(multi_hop_blocks.begin(), multi_hop_blocks.end()),
                               multi_hop_blocks.end());
        if (!multi_hop_blocks.empty())
            graph_prefetcher_->submitPrefetch(multi_hop_blocks, true);
    }

    // 处理邻居: in-cache 直接计算距离, out-of-cache 加入 deferred
    for (uint32_t j = 0; j < local_neighbors.size(); j++) {
        uint32_t neighborId = local_neighbors[j];
        if (neighborId >= graph_.num_nodes) continue;
        if (qs.visited->isVisited(neighborId)) continue;
        qs.visited->markVisited(neighborId);

        if (pq_enabled_) {
            // PQ 模式: 不需要 block 中的向量, 直接算 ADC 距离
            float dist = pqDistance(qs.query, neighborId);
            if (qs.top_candidates.size() < qs.ef || qs.lowerBound > dist) {
                qs.candidate_set.emplace(dist, neighborId);
                qs.top_candidates.emplace(dist, neighborId);
                if (qs.top_candidates.size() > qs.ef) qs.top_candidates.pop();
                if (!qs.top_candidates.empty()) qs.lowerBound = qs.top_candidates.top().first;
            }
        } else {
            uint32_t neighbor_block = route_table_ ? (*route_table_)[neighborId]
                                                   : cache_->getBlockId(neighborId);
            // getCachedBlockById 更新 LRU
            CachedBlock* nBlock = cache_->getCachedBlockById(neighbor_block);
            if (nBlock) {
                const float* neighborVec = nBlock->getVector(neighborId);
                if (!neighborVec) continue;
                float dist = l2Distance(qs.query, neighborVec);
                if (qs.top_candidates.size() < qs.ef || qs.lowerBound > dist) {
                    qs.candidate_set.emplace(dist, neighborId);
                    qs.top_candidates.emplace(dist, neighborId);
                    if (qs.top_candidates.size() > qs.ef) qs.top_candidates.pop();
                    if (!qs.top_candidates.empty()) qs.lowerBound = qs.top_candidates.top().first;
                }
            } else {
                qs.deferred.push_back({neighborId, neighbor_block});
            }
        }
    }
}

// 事件驱动: 连续处理候选直到 I/O miss 或完成
// 返回处理的步数, 0 表示已完成或第一步就 miss
int DiskHNSW::runQueryUntilMiss(QueryState& qs, int max_steps) {
    int steps = 0;
    while (steps < max_steps && !qs.done) {
        // 如果在等待 block, 不能继续
        if (qs.waitingBlockId) return steps;
        // 如果 deferred 非空且 candidate_set 为空, 需要等 I/O
        if (qs.candidate_set.empty() && !qs.deferred.empty()) {
            // 尝试处理 deferred
            stepQueryState(qs);
            steps++;
            if (qs.waitingBlockId) return steps;
            continue;
        }
        if (qs.candidate_set.empty() && qs.deferred.empty()) {
            qs.done = true;
            return steps;
        }
        stepQueryState(qs);
        steps++;
        // 检查是否开始等待
        if (qs.waitingBlockId) return steps;
    }
    return steps;
}

// 构建 BFS-remapped CSR 邻接表
void DiskHNSW::buildInMemoryAdjacency() {
    if (graph_.adjacency0.empty()) {
        std::cerr << "[DiskHNSW] Warning: graph_.adjacency0 is empty, cannot build CSR" << std::endl;
        return;
    }

    uint32_t N = graph_.num_nodes;
    adj_csr_offsets_.resize(N + 1, 0);

    // 第一步: 计算每个节点的邻居数 (在 new_id 空间)
    // graph_.adjacency0 使用 old_id, 需要映射
    for (uint32_t old_id = 0; old_id < N; old_id++) {
        uint32_t new_id = old_to_new_[old_id];
        adj_csr_offsets_[new_id + 1] = graph_.adjacency0[old_id].size();
    }

    // 前缀和
    for (uint32_t i = 0; i < N; i++) {
        adj_csr_offsets_[i + 1] += adj_csr_offsets_[i];
    }

    size_t total_edges = adj_csr_offsets_[N];
    adj_csr_neighbors_.resize(total_edges);

    // 第二步: 填充邻居 (映射到 new_id)
    // 临时 offset 指针
    std::vector<uint32_t> write_pos(adj_csr_offsets_.begin(), adj_csr_offsets_.end());

    for (uint32_t old_id = 0; old_id < N; old_id++) {
        uint32_t new_id = old_to_new_[old_id];
        for (uint32_t old_neighbor : graph_.adjacency0[old_id]) {
            uint32_t new_neighbor = old_to_new_[old_neighbor];
            adj_csr_neighbors_[write_pos[new_id]++] = new_neighbor;
        }
    }

    has_inmem_adjacency_ = true;

    // 释放原始邻接表内存 (CSR 已包含所有信息)
    graph_.adjacency0.clear();
    graph_.adjacency0.shrink_to_fit();

    size_t adj_mb = (total_edges * 4 + (N + 1) * 4) / (1024 * 1024);
    std::cout << "  [CSR] Built in-memory adjacency: " << total_edges << " edges, "
              << adj_mb << " MB" << std::endl;
}

// 从内存 CSR 邻接表获取邻居
const uint32_t* DiskHNSW::getInMemNeighbors(uint32_t new_id, uint32_t& out_count) const {
    if (!has_inmem_adjacency_ || new_id >= graph_.num_nodes) {
        out_count = 0;
        return nullptr;
    }
    uint32_t start = adj_csr_offsets_[new_id];
    uint32_t end = adj_csr_offsets_[new_id + 1];
    out_count = end - start;
    return &adj_csr_neighbors_[start];
}

std::vector<std::vector<DiskHNSW::SearchResult>>
DiskHNSW::batchSearchEventDriven(const std::vector<float>& queries, size_t k, size_t batch_size) {
    std::vector<std::vector<SearchResult>> results;
    size_t dim = dim_;
    size_t total = queries.size() / dim;
    results.reserve(total);

    if (heat_evaluator_) heat_evaluator_->onQueryStart();

    for (size_t batch_start = 0; batch_start < total; batch_start += batch_size) {
        size_t batch_end = std::min(batch_start + batch_size, total);
        size_t n = batch_end - batch_start;

        // 初始化 n 个 QueryState
        std::vector<QueryState> states(n);
        for (size_t i = 0; i < n; i++) {
            states[i].query_id = batch_start + i;
            size_t ef = std::max(ef_search_, k);
            initQueryState(states[i], &queries[(batch_start + i) * dim], k, ef);
        }

        // round-robin 事件驱动循环 (贪心版: 每个查询处理到 I/O miss 才 yield)
        const int greedy_limit = 256;  // 每个 query 在一次调度中最多处理的候选数
        while (true) {
            bool all_done = true;
            int idle_count = 0;

            for (size_t i = 0; i < n; i++) {
                if (states[i].done) continue;
                all_done = false;

                if (states[i].waitingBlockId) {
                    CachedBlock* blk = cache_->peekCachedBlockById(states[i].waitingBlockId);
                    if (blk) {
                        states[i].waitingBlockId = 0;
                    } else {
                        idle_count++;
                        continue;
                    }
                }

                // 贪心: 连续处理直到 I/O miss 或达到步数上限
                int steps_this_round = 0;
                while (!states[i].done && !states[i].waitingBlockId && steps_this_round < greedy_limit) {
                    stepQueryState(states[i]);
                    steps_this_round++;
                }
                if (states[i].waitingBlockId) idle_count++;
            }

            if (all_done) break;

            if (idle_count > 0) {
                bool any_ready = false;
                if (graph_prefetch_enabled_ && graph_prefetcher_) {
                    graph_prefetcher_->reapCompletions();
                }
                for (size_t i = 0; i < n; i++) {
                    if (states[i].done || !states[i].waitingBlockId) continue;
                    CachedBlock* blk = cache_->peekCachedBlockById(states[i].waitingBlockId);
                    if (blk) {
                        states[i].waitingBlockId = 0;
                        any_ready = true;
                    }
                }
                if (!any_ready) {
                    std::vector<uint32_t> need_prefetch;
                    for (size_t i = 0; i < n; i++) {
                        if (states[i].done || !states[i].waitingBlockId) continue;
                        if (!cache_->isInCache(states[i].waitingBlockId)) {
                            need_prefetch.push_back(states[i].waitingBlockId);
                        }
                    }
                    if (!need_prefetch.empty() && graph_prefetch_enabled_ && graph_prefetcher_) {
                        graph_prefetcher_->submitPrefetch(need_prefetch, true);
                        graph_prefetcher_->waitForBlocks(
                            std::set<uint32_t>(need_prefetch.begin(), need_prefetch.end()));
                    } else if (!need_prefetch.empty()) {
                        for (uint32_t bid : need_prefetch) {
                            cache_->getBlockById(bid);
                            break;
                        }
                    }
                }
            }
        }

        // 提取结果
        for (size_t i = 0; i < n; i++) {
            std::vector<SearchResult> result;
            size_t numResults = std::min(k, states[i].top_candidates.size());
            result.reserve(numResults);

            // top_candidates 是最大堆, 需要按距离从小到大输出
            // 转移到临时 vector 排序
            std::vector<std::pair<float, uint32_t>> tmp;
            tmp.reserve(states[i].top_candidates.size());
            while (!states[i].top_candidates.empty()) {
                tmp.push_back(states[i].top_candidates.top());
                states[i].top_candidates.pop();
            }
            std::sort(tmp.begin(), tmp.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });

            for (size_t j = 0; j < numResults && j < tmp.size(); j++) {
                auto [dist, newId] = tmp[j];
                uint32_t oldId = new_to_old_[newId];
                uint64_t label = graph_.labels[oldId];
                result.emplace_back(dist, label);
            }
            results.push_back(std::move(result));
        }
    }

    if (heat_evaluator_) heat_evaluator_->onQueryEnd();

    return results;
}

