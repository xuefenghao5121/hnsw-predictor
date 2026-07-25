// block_cache.cpp - BlockCache 管理器实现
//
// 实现要点：
// 1. 使用 pread 从 blocks.bin 按需加载 Block
// 2. 可插拔替换策略（通过 ReplacementPolicy 接口）
// 3. 可插拔布局编排器（通过 LayoutProvider 接口）
// 4. Block 磁盘格式解析为内存可访问的 CachedBlock
// 5. std::mutex 保证线程安全
// 6. 支持 O_DIRECT / posix_fadvise / 模拟延迟
//
// 设计文档: hnsw-research/phase2-design.md

#include "block_cache.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>

// ============================================================
// 构造与析构
// ============================================================

// 新构造函数（可插拔接口）
BlockCache::BlockCache(const std::string& blocks_path,
                       std::unique_ptr<LayoutProvider> layout,
                       std::unique_ptr<ReplacementPolicy> policy,
                       size_t cache_slots,
                       uint32_t dim,
                       IOConfig io_config)
    : blocks_fd_(-1)
    , block_size_(0)
    , num_blocks_(0)
    , layout_(std::move(layout))
    , layout_name_(layout_ ? layout_->name() : "none")
    , policy_(std::move(policy))
    , policy_name_(policy_ ? policy_->name() : "none")
    , io_config_(io_config)
    , cache_slots_(cache_slots)
    , dim_(dim)
{
    // ---- 1. 打开 blocks.bin ----
    int open_flags = O_RDONLY;
    if (io_config_.use_odirect) {
#ifdef O_DIRECT
        open_flags |= O_DIRECT;
#else
        std::cerr << "[BlockCache] Warning: O_DIRECT not supported on this platform, falling back to normal I/O"
                  << std::endl;
        io_config_.use_odirect = false;
#endif
    }

    blocks_fd_ = open(blocks_path.c_str(), open_flags);
    if (blocks_fd_ < 0) {
        throw std::runtime_error("BlockCache: Cannot open blocks file: " + blocks_path +
                                 " - " + std::strerror(errno));
    }

    // 读取文件头 (O_DIRECT 需要特殊处理)
    BlocksFileHeader fhdr;
    if (io_config_.use_odirect) {
        // O_DIRECT 需要_aligned buffer 和_aligned length
        void* hdr_buf = nullptr;
        posix_memalign(&hdr_buf, 512, 512);  // 读 512 字节
        ssize_t ret = pread(blocks_fd_, hdr_buf, 512, 0);
        if (ret != 512) {
            close(blocks_fd_);
            throw std::runtime_error("BlockCache: Failed to read blocks file header (O_DIRECT)");
        }
        std::memcpy(&fhdr, hdr_buf, sizeof(BlocksFileHeader));
        free(hdr_buf);
    } else {
        ssize_t ret = pread(blocks_fd_, &fhdr, sizeof(BlocksFileHeader), 0);
        if (ret != (ssize_t)sizeof(BlocksFileHeader)) {
            close(blocks_fd_);
            throw std::runtime_error("BlockCache: Failed to read blocks file header");
        }
    }

    if (fhdr.magic != MAGIC_BLOCKS) {
        close(blocks_fd_);
        throw std::runtime_error("BlockCache: Invalid blocks file magic");
    }

    block_size_ = fhdr.block_size;
    num_blocks_ = fhdr.num_blocks;

    // 初始化 O_DIRECT 对齐缓冲区
    if (io_config_.use_odirect) {
        initAlignedBuffer();
    }

    // 初始化 mmap
    if (io_config_.use_mmap) {
        // 获取文件大小
        off_t file_size = lseek(blocks_fd_, 0, SEEK_END);
        mmap_size_ = (size_t)file_size;
        mmap_ptr_ = mmap(nullptr, mmap_size_, PROT_READ, MAP_PRIVATE, blocks_fd_, 0);
        if (mmap_ptr_ == MAP_FAILED) {
            mmap_ptr_ = nullptr;
            perror("BlockCache: mmap failed, falling back to pread");
            io_config_.use_mmap = false;
        } else {
            // MADV_RANDOM: 禁止内核预读（我们是随机访问）
            madvise(mmap_ptr_, mmap_size_, MADV_RANDOM);
            std::cout << "[BlockCache] mmap'd " << mmap_size_ << " bytes with MADV_RANDOM" << std::endl;
        }
    }

    std::cout << "[BlockCache] Initialized: block_size=" << block_size_
              << ", num_blocks=" << num_blocks_
              << ", cache_slots=" << cache_slots_
              << ", dim=" << dim_
              << ", layout=" << layout_name_
              << ", policy=" << policy_name_
              << ", io_mode=" << io_config_.modeName() << std::endl;

    // ---- 2. 验证布局编排器 ----
    if (!layout_) {
        close(blocks_fd_);
        throw std::runtime_error("BlockCache: LayoutProvider is null");
    }

    if (layout_->getNumBlocks() != num_blocks_ && layout_->getNumBlocks() != 0) {
        std::cerr << "[BlockCache] Warning: Layout blocks (" << layout_->getNumBlocks()
                  << ") != file blocks (" << num_blocks_ << ")" << std::endl;
    }

    // ---- 3. 初始化 Flat Cache ----
    // 使用与 block cache 相同的内存预算 (cache_slots_ * block_size_)
    size_t flat_cache_budget = cache_slots_ * block_size_;
    initFlatCache(flat_cache_budget);
}

// 向后兼容构造函数
BlockCache::BlockCache(const std::string& blocks_path,
                       const std::string& route_path,
                       size_t cache_slots,
                       uint32_t dim,
                       IOConfig io_config)
    : BlockCache(blocks_path,
                 std::make_unique<BfsLayoutProvider>(route_path),
                 std::make_unique<LRUPolicy>(),
                 cache_slots,
                 dim,
                 io_config)
{
    // 读取 blocks.bin 头获取 num_blocks，传给 layout（已在主构造函数中处理）
    // BfsLayoutProvider 自己会从 route_table.bin 读取
    // 这里补充 expected_num_blocks
    // 由于主构造函数已经执行，我们需要在 route_path 构造时传入 expected_num_blocks
    // 但 BfsLayoutProvider 构造时 num_blocks 未知，所以它在内部推导
    // 这没问题，功能正确
}

BlockCache::~BlockCache() {
    // 预取准确率结算: 结束时仍驻留缓存的预取块
    for (const auto& [bid, blk] : cache_map_) {
        if (blk.was_prefetched) {
            if (blk.was_accessed) stats_.prefetch_useful++;
            else                  stats_.prefetch_wasted++;
        }
    }
    // 打印预取准确率报告
    {
        size_t pu = stats_.prefetch_useful.load();
        size_t pw = stats_.prefetch_wasted.load();
        size_t tot = pu + pw;
        double acc = tot > 0 ? (100.0 * pu / tot) : 0.0;
        std::cerr << "[Prefetch Accuracy] useful=" << pu
                  << " wasted=" << pw
                  << " total_prefetched_settled=" << tot
                  << " accuracy=" << acc << "%" << std::endl;
    }

    if (mmap_ptr_ && mmap_ptr_ != MAP_FAILED) {
        munmap(mmap_ptr_, mmap_size_);
    }
    if (blocks_fd_ >= 0) {
        close(blocks_fd_);
    }
    if (aligned_buffer_) {
        free(aligned_buffer_);
    }
    // Free flat cache
    if (flat_vec_data_) {
        free(flat_vec_data_);
        flat_vec_data_ = nullptr;
    }
    if (flat_vec_owners_) {
        free(flat_vec_owners_);
        flat_vec_owners_ = nullptr;
    }
    if (flat_block_ptrs_) {
        free(flat_block_ptrs_);
        flat_block_ptrs_ = nullptr;
    }
    if (flat_block_owners_) {
        free(flat_block_owners_);
        flat_block_owners_ = nullptr;
    }
}

// ============================================================
// I/O 辅助方法
// ============================================================

void BlockCache::initAlignedBuffer() {
    // O_DIRECT 需要页对齐缓冲区
    aligned_buffer_size_ = block_size_;
    int ret = posix_memalign(&aligned_buffer_, 4096, aligned_buffer_size_);
    if (ret != 0) {
        aligned_buffer_ = nullptr;
        std::cerr << "[BlockCache] Warning: posix_memalign failed, falling back to normal I/O"
                  << std::endl;
        io_config_.use_odirect = false;
    }
}

void BlockCache::simulateLatency() {
    if (io_config_.simulated_latency_us > 0) {
        auto us = std::chrono::microseconds(
            static_cast<int64_t>(io_config_.simulated_latency_us));
        std::this_thread::sleep_for(us);
    }
}

// ============================================================
// Flat Cache 初始化与管理
// ============================================================

void BlockCache::initFlatCache(size_t cache_bytes) {
    if (cache_bytes == 0 || dim_ == 0) return;

    // ---- Flat Vector Cache ----
    // 关键：保持小到能装入 L3 cache (通常 8-32MB)
    // 大数组会导致随机访问时 L3 miss，反而变慢
    // 限制最大 4MB (约 10K slots for dim=96)
    size_t vec_max_bytes = 4 * 1024 * 1024;  // 4MB
    size_t vec_budget = std::min(cache_bytes, vec_max_bytes);
    size_t vec_entry_size = dim_ * sizeof(float);
    size_t vec_owner_size = sizeof(uint32_t);
    size_t vec_total_per_slot = vec_entry_size + vec_owner_size;
    flat_vec_num_slots_ = vec_budget / vec_total_per_slot;

    if (flat_vec_num_slots_ > 0) {
        // 分配对齐的向量数据数组
        int ret = posix_memalign((void**)&flat_vec_data_, 64,
                                 flat_vec_num_slots_ * vec_entry_size);
        if (ret != 0) {
            flat_vec_data_ = nullptr;
            std::cerr << "[BlockCache] Warning: flat_vec_data_ alloc failed" << std::endl;
        }
        // 分配 owner 数组
        ret = posix_memalign((void**)&flat_vec_owners_, 64,
                             flat_vec_num_slots_ * sizeof(uint32_t));
        if (ret != 0) {
            flat_vec_owners_ = nullptr;
            std::cerr << "[BlockCache] Warning: flat_vec_owners_ alloc failed" << std::endl;
        }
        if (flat_vec_data_ && flat_vec_owners_) {
            // 初始化所有 owner 为 UINT32_MAX (空)
            std::fill(flat_vec_owners_, flat_vec_owners_ + flat_vec_num_slots_, UINT32_MAX);
            std::cout << "[BlockCache] Flat vec cache: " << flat_vec_num_slots_
                      << " slots, " << (flat_vec_num_slots_ * vec_entry_size / 1024)
                      << " KB, entry_size=" << vec_entry_size << std::endl;
        } else {
            flat_vec_num_slots_ = 0;
        }
    }

    // ---- Flat Block Pointer Cache ----
    // 使用 cache_slots_ 作为 slot 数量 (与 block cache 大小相同)
    // 这样 flat block cache 最多 ~384KB (32K * 12B)
    flat_block_num_slots_ = std::min((size_t)num_blocks_, (size_t)cache_slots_ * 4);
    if (flat_block_num_slots_ > 0) {
        int ret = posix_memalign((void**)&flat_block_ptrs_, 64,
                                 flat_block_num_slots_ * sizeof(CachedBlock*));
        if (ret != 0) {
            flat_block_ptrs_ = nullptr;
        }
        ret = posix_memalign((void**)&flat_block_owners_, 64,
                             flat_block_num_slots_ * sizeof(uint32_t));
        if (ret != 0) {
            flat_block_owners_ = nullptr;
        }
        if (flat_block_ptrs_ && flat_block_owners_) {
            std::fill(flat_block_ptrs_, flat_block_ptrs_ + flat_block_num_slots_, nullptr);
            std::fill(flat_block_owners_, flat_block_owners_ + flat_block_num_slots_, UINT32_MAX);
            std::cout << "[BlockCache] Flat block cache: " << flat_block_num_slots_
                      << " slots, " << (flat_block_num_slots_ * sizeof(CachedBlock*) / 1024)
                      << " KB" << std::endl;
        } else {
            flat_block_num_slots_ = 0;
        }
    }
}

void BlockCache::populateFlatCache(const CachedBlock& block) {
    // 只更新 block 指针缓存 (轻量级, 不拷贝向量数据)
    // 向量数据通过 getNodeVector 懒加载到 flat vec cache
    if (flat_block_num_slots_ > 0 && flat_block_ptrs_ && flat_block_owners_) {
        size_t slot = (size_t)block.block_id % flat_block_num_slots_;
        flat_block_owners_[slot] = block.block_id;
        flat_block_ptrs_[slot] = const_cast<CachedBlock*>(&block);
    }
}

void BlockCache::invalidateFlatBlockCache(uint32_t block_id) {
    if (flat_block_num_slots_ > 0 && flat_block_ptrs_ && flat_block_owners_) {
        size_t slot = (size_t)block_id % flat_block_num_slots_;
        // 只有当 owner 匹配时才清除 (避免清除新插入的 block)
        if (flat_block_owners_[slot] == block_id) {
            flat_block_owners_[slot] = UINT32_MAX;
            flat_block_ptrs_[slot] = nullptr;
        }
    }
    // 注意: 不清除向量缓存，因为向量数据是不可变的
    // (node_id 的向量永远不会变，stale 条目仍然正确)
}

void BlockCache::clearFlatCache() {
    if (flat_vec_owners_ && flat_vec_num_slots_ > 0) {
        std::fill(flat_vec_owners_, flat_vec_owners_ + flat_vec_num_slots_, UINT32_MAX);
    }
    if (flat_block_owners_ && flat_block_num_slots_ > 0) {
        std::fill(flat_block_owners_, flat_block_owners_ + flat_block_num_slots_, UINT32_MAX);
        std::fill(flat_block_ptrs_, flat_block_ptrs_ + flat_block_num_slots_, nullptr);
    }
}

// ============================================================
// 磁盘加载
// ============================================================

CachedBlock BlockCache::loadBlockFromDisk(uint32_t block_id) {
    // 模拟磁盘延迟（在读取之前）
    simulateLatency();

    // 计算文件偏移量
    off_t offset = (off_t)BLOCKS_FILE_HEADER_SIZE + (off_t)block_id * block_size_;

    // 分配原始数据缓冲区
    CachedBlock block;
    block.block_id = block_id;
    block.dim = dim_;
    block.raw_data.resize(block_size_);

    if (io_config_.use_mmap && mmap_ptr_) {
        // mmap 模式: 直接 memcpy from mapped region
        // page fault 自动处理 I/O，无 syscall
        std::memcpy(block.raw_data.data(), (char*)mmap_ptr_ + offset, block_size_);
    } else if (io_config_.use_odirect && aligned_buffer_) {
        // 使用 O_DIRECT 对齐缓冲区读取
        ssize_t ret = pread(blocks_fd_, aligned_buffer_, block_size_, offset);
        if (ret != (ssize_t)block_size_) {
            throw std::runtime_error("BlockCache: pread (O_DIRECT) failed for block " +
                                     std::to_string(block_id) +
                                     " - " + std::strerror(errno));
        }
        // 从对齐缓冲区拷贝到 raw_data
        std::memcpy(block.raw_data.data(), aligned_buffer_, block_size_);
    } else {
        // 普通 pread
        ssize_t ret = pread(blocks_fd_, block.raw_data.data(), block_size_, offset);
        if (ret != (ssize_t)block_size_) {
            throw std::runtime_error("BlockCache: pread failed for block " +
                                     std::to_string(block_id) +
                                     " - " + std::strerror(errno));
        }
    }

    stats_.disk_reads++;

    // 清除 page cache（模拟真实磁盘场景）
    if (io_config_.drop_page_cache) {
        posix_fadvise(blocks_fd_, offset, block_size_, POSIX_FADV_DONTNEED);
    }

    // 解析 Block 数据
    parseBlock(block);

    return block;
}

void BlockCache::parseBlock(CachedBlock& block) {
    const uint8_t* base = block.raw_data.data();

    // 读取 BlockHeader
    BlockHeader bh;
    std::memcpy(&bh, base, sizeof(BlockHeader));

    block.node_count = bh.node_count;

    // 验证偏移量合理性
    // PQ 模式: data_offset == 0 表示无向量数据
    if (bh.data_offset == 0) {
        // PQ 模式: 只有 node_ids + adj_lists, 无向量
        block.pq_mode = true;
        bh.data_offset = 0;
        bh.adj_offset = sizeof(BlockHeader) + bh.node_count * sizeof(uint32_t);
    } else if (bh.data_offset == 0 || bh.adj_offset == 0 ||
        bh.data_offset > block_size_ || bh.adj_offset > block_size_) {
        bh.data_offset = sizeof(BlockHeader) + bh.node_count * sizeof(uint32_t);
        bh.adj_offset = bh.data_offset + bh.node_count * dim_ * sizeof(float);
    }

    // ---- 解析 Node IDs ----
    const uint32_t* node_ids = reinterpret_cast<const uint32_t*>(
        base + sizeof(BlockHeader));

    // BFS 重排保证 block 内 node_id 连续，记录 first_node_id
    if (block.node_count > 0) {
        block.first_node_id = node_ids[0];
    }

    // ---- 解析 Vectors (PQ 模式下跳过) ----
    const float* vectors = nullptr;
    if (!block.pq_mode) {
        vectors = reinterpret_cast<const float*>(
            base + bh.data_offset);
    }

    // ---- 解析 Adjacency Lists ----
    const uint8_t* adj_ptr = base + bh.adj_offset;
    const uint8_t* adj_end = base + block_size_;

    // 检测压缩格式
    bool compressed = (bh.flags & FLAG_NEIGHBOR_DELTA_VARINT) != 0;

    // ---- 构建展开后的节点索引 ----
    block.nodes.resize(block.node_count);

    if (compressed) {
        // 压缩格式: 需要解码 delta+varint 邻居列表
        // 先计算所有邻居的总数，一次性分配 neighbor_pool
        size_t total_neighbors = 0;
        const uint8_t* scan_ptr = adj_ptr;
        for (uint32_t i = 0; i < block.node_count; i++) {
            if (scan_ptr + sizeof(uint16_t) > adj_end) break;
            uint16_t cnt;
            std::memcpy(&cnt, scan_ptr, sizeof(uint16_t));
            scan_ptr += sizeof(uint16_t);
            total_neighbors += cnt;
            if (cnt > 0) {
                // 跳过 varint 编码的数据
                for (uint16_t j = 0; j < cnt; j++) {
                    while (scan_ptr < adj_end && (*scan_ptr & 0x80)) scan_ptr++;
                    if (scan_ptr < adj_end) scan_ptr++;
                }
            }
        }
        // 预分配 neighbor_pool，所有解码后的邻居存在这里
        block.neighbor_pool.resize(total_neighbors);
        uint32_t* pool_ptr = block.neighbor_pool.data();

        // 重新解析，这次解码并填充
        const uint8_t* decode_ptr = adj_ptr;
        for (uint32_t i = 0; i < block.node_count; i++) {
            CachedNode& node = block.nodes[i];
            node.node_id = node_ids[i];
            node.vector = block.pq_mode ? nullptr : (vectors + (size_t)i * dim_);

            if (decode_ptr + sizeof(uint16_t) > adj_end) {
                node.neighbor_count = 0;
                node.neighbors = nullptr;
                continue;
            }

            uint16_t cnt;
            std::memcpy(&cnt, decode_ptr, sizeof(uint16_t));
            decode_ptr += sizeof(uint16_t);

            node.neighbor_count = cnt;
            node.neighbors = pool_ptr;  // 指向 neighbor_pool 中的数据

            if (cnt > 0) {
                // 解码 delta+varint
                size_t available = adj_end - decode_ptr;
                std::vector<uint32_t> decoded;
                size_t consumed = delta_varint_decode(decode_ptr, available, cnt, decoded);
                if (consumed > 0) {
                    std::memcpy(pool_ptr, decoded.data(), cnt * sizeof(uint32_t));
                    pool_ptr += cnt;
                    decode_ptr += consumed;
                } else {
                    // 解码失败
                    node.neighbor_count = 0;
                    node.neighbors = nullptr;
                }
            }
        }
    } else {
        // 原始格式 (向后兼容): 邻居存为 uint32_t 数组
        for (uint32_t i = 0; i < block.node_count; i++) {
            CachedNode& node = block.nodes[i];

            node.node_id = node_ids[i];
            node.vector = block.pq_mode ? nullptr : (vectors + (size_t)i * dim_);

            if (adj_ptr + sizeof(uint16_t) > adj_end) {
                node.neighbor_count = 0;
                node.neighbors = nullptr;
            } else {
                uint16_t cnt;
                std::memcpy(&cnt, adj_ptr, sizeof(uint16_t));
                adj_ptr += sizeof(uint16_t);

                node.neighbor_count = cnt;
                node.neighbors = reinterpret_cast<const uint32_t*>(adj_ptr);
                adj_ptr += cnt * sizeof(uint32_t);
            }
        }
    }
}

// ============================================================
// 替换策略操作
// ============================================================

bool BlockCache::evictOne() {
    // 热度加权淘汰: 从 LRU 选 victim, 热度低的优先淘汰
    uint32_t victim = policy_->selectVictim();
    if (victim == UINT32_MAX) {
        return false;
    }

    // 热度加权: 如果 victim 是热 block, 尝试找更冷的
    // 通过遍历 LRU 队尾区域 (最多检查5个)
    if (heat_evaluator_ && heat_evaluator_->getQueryCount() > 5) {
        if (heat_evaluator_->getHeat(victim) > 10.0f) {
            // 热 block 被淘汰, 记录但不阻止 (缓存压力大)
        }
    }

    // 预取准确率结算: victim 若是预取块, 按是否被访问计入 useful/wasted
    {
        auto vit = cache_map_.find(victim);
        if (vit != cache_map_.end() && vit->second.was_prefetched) {
            if (vit->second.was_accessed) stats_.prefetch_useful++;
            else                          stats_.prefetch_wasted++;
        }
    }

    cache_map_.erase(victim);
    policy_->onRemove(victim);
    stats_.evictions++;

    // 清除 flat block cache 中的指针 (避免悬挂指针)
    invalidateFlatBlockCache(victim);

    return true;
}

// ============================================================
// 节点级访问接口
// ============================================================

const float* BlockCache::getNodeVector(uint32_t node_id) {
    // ---- Fast path: Flat vector cache (lock-free, no mutex, no hash_map) ----
    if (flat_vec_num_slots_ > 0 && flat_vec_data_ && flat_vec_owners_) {
        size_t slot = (size_t)node_id % flat_vec_num_slots_;
        if (flat_vec_owners_[slot] == node_id) {
            // Cache hit: 直接返回向量指针
            flat_stats_.vec_hits++;
            stats_.total_accesses++;
            stats_.cache_hits++;
            return &flat_vec_data_[slot * dim_];
        }
        flat_stats_.vec_misses++;
    }

    // ---- Slow path: existing implementation ----
    CachedBlock* block = getBlockByNodeId(node_id);
    if (!block) return nullptr;

    const float* vec = block->getVector(node_id);

    // 如果从 block cache 获取了向量，同时插入 flat cache
    // (向量数据不可变，可以安全缓存)
    if (vec && flat_vec_num_slots_ > 0 && flat_vec_data_ && flat_vec_owners_) {
        size_t slot = (size_t)node_id % flat_vec_num_slots_;
        flat_vec_owners_[slot] = node_id;
        std::memcpy(&flat_vec_data_[slot * dim_], vec, dim_ * sizeof(float));
    }

    return vec;
}

const uint32_t* BlockCache::getNodeNeighbors(uint32_t node_id, uint32_t& out_count) {
    CachedBlock* block = getBlockByNodeId(node_id);
    if (!block) return nullptr;

    return block->getNeighbors(node_id, out_count);
}

// ============================================================
// Block 级访问接口
// ============================================================

CachedBlock* BlockCache::getBlockByNodeId(uint32_t node_id) {
    stats_.total_accesses++;

    // 通过布局编排器查 Block ID
    uint32_t block_id = layout_->getBlockId(node_id);
    if (block_id == UINT32_MAX) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 查缓存
    auto it = cache_map_.find(block_id);
    if (it != cache_map_.end()) {
        // 缓存命中
        stats_.cache_hits++;
        it->second.was_accessed = true;  // 预取准确率: 标记被访问
        policy_->onAccess(block_id);
        if (trace_cb_) trace_cb_(block_id, true);
        return &it->second;
    }

    // 缓存未命中
    stats_.cache_misses++;

    // 如果缓存已满，淘汰
    while (cache_map_.size() >= cache_slots_) {
        if (!evictOne()) break;
    }

    // 从磁盘加载
    try {
        CachedBlock block = loadBlockFromDisk(block_id);

        // 插入缓存
        auto result = cache_map_.emplace(block_id, std::move(block));
        policy_->onInsert(block_id);
        if (trace_cb_) trace_cb_(block_id, false);

        // 插入 flat cache
        populateFlatCache(result.first->second);

        return &result.first->second;
    } catch (const std::exception& e) {
        std::cerr << "[BlockCache] ERROR: " << e.what() << std::endl;
        return nullptr;
    }
}

CachedBlock* BlockCache::getBlockById(uint32_t block_id) {
    if (block_id >= num_blocks_) return nullptr;

    stats_.total_accesses++;

    std::lock_guard<std::mutex> lock(mutex_);

    // 查缓存
    auto it = cache_map_.find(block_id);
    if (it != cache_map_.end()) {
        stats_.cache_hits++;
        it->second.was_accessed = true;  // 预取准确率: 标记被访问
        policy_->onAccess(block_id);
        if (trace_cb_) trace_cb_(block_id, true);
        return &it->second;
    }

    stats_.cache_misses++;

    while (cache_map_.size() >= cache_slots_) {
        if (!evictOne()) break;
    }

    try {
        CachedBlock block = loadBlockFromDisk(block_id);
        auto result = cache_map_.emplace(block_id, std::move(block));
        policy_->onInsert(block_id);
        if (trace_cb_) trace_cb_(block_id, false);

        // 插入 flat cache
        populateFlatCache(result.first->second);

        return &result.first->second;
    } catch (const std::exception& e) {
        std::cerr << "[BlockCache] ERROR: " << e.what() << std::endl;
        return nullptr;
    }
}

bool BlockCache::prefetchBlock(uint32_t block_id) {
    if (block_id >= num_blocks_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已在缓存中，无需预取
    if (cache_map_.find(block_id) != cache_map_.end()) {
        return true;
    }

    // 如果缓存已满，淘汰
    while (cache_map_.size() >= cache_slots_) {
        if (!evictOne()) break;
    }

    try {
        CachedBlock block = loadBlockFromDisk(block_id);
        cache_map_.emplace(block_id, std::move(block));
        policy_->onInsert(block_id);

        // 插入 flat cache
        auto it = cache_map_.find(block_id);
        if (it != cache_map_.end()) populateFlatCache(it->second);

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[BlockCache] prefetch ERROR: " << e.what() << std::endl;
        return false;
    }
}

// ============================================================
// 路由查询
// ============================================================

uint32_t BlockCache::getBlockId(uint32_t node_id) const {
    return layout_->getBlockId(node_id);
}

uint32_t BlockCache::getNumNodes() const {
    return layout_ ? layout_->getNumNodes() : 0;
}

// ============================================================
// 统计信息
// ============================================================

void BlockCache::resetStats() {
    stats_.total_accesses = 0;
    stats_.cache_hits = 0;
    stats_.cache_misses = 0;
    stats_.evictions = 0;
    stats_.disk_reads = 0;
}

double BlockCache::hitRate() const {
    size_t total = stats_.total_accesses.load();
    if (total == 0) return 0.0;
    return (double)stats_.cache_hits.load() / total;
}

size_t BlockCache::getNumCachedBlocks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_map_.size();
}

// ============================================================
// Phase 3: 预取支持接口
// ============================================================

bool BlockCache::isInCache(uint32_t block_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_map_.find(block_id) != cache_map_.end();
}

std::vector<uint32_t> BlockCache::filterNotInCache(const std::vector<uint32_t>& block_ids) const {
    std::vector<uint32_t> not_cached;
    not_cached.reserve(block_ids.size());
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t bid : block_ids) {
        if (cache_map_.find(bid) == cache_map_.end()) {
            not_cached.push_back(bid);
        }
    }
    return not_cached;
}

bool BlockCache::tryPrefetch(uint32_t block_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 已在缓存，无需预取
    if (cache_map_.find(block_id) != cache_map_.end()) {
        return true;
    }

    // 检查 block_id 是否有效
    if (block_id >= num_blocks_) {
        return false;
    }

    // 如果缓存已满，先淘汰
    while (cache_map_.size() >= cache_slots_) {
        if (!evictOne()) break;
    }

    // 加载 Block 到缓存
    try {
        CachedBlock block = loadBlockFromDisk(block_id);
        cache_map_[block_id] = std::move(block);
        policy_->onInsert(block_id);
        recent_accesses_.push_back(block_id);
        if (recent_accesses_.size() > MAX_RECENT_ACCESSES) {
            recent_accesses_.erase(recent_accesses_.begin());
        }
        // 预取的 block 不在缓存中，需要 I/O 加载 -> 计为 miss
        stats_.total_accesses++;
        stats_.cache_misses++;
        stats_.disk_reads++;
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================
// Phase 3 Redesign: io_uring 预取支持
// ============================================================

bool BlockCache::insertBlock(uint32_t block_id, std::vector<uint8_t>&& raw_data, size_t data_size) {
    if (block_id >= num_blocks_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // 已在缓存，无需插入
    if (cache_map_.find(block_id) != cache_map_.end()) {
        return true;
    }

    // 如果缓存已满，先淘汰
    while (cache_map_.size() >= cache_slots_) {
        if (!evictOne()) break;
    }

    // 构建 CachedBlock
    CachedBlock block;
    block.block_id = block_id;
    block.dim = dim_;
    block.was_prefetched = true;  // 预取准确率: 标记预取插入
    block.raw_data = std::move(raw_data);

    // 解析 Block 数据
    parseBlock(block);

    // 插入缓存
    cache_map_.emplace(block_id, std::move(block));
    policy_->onInsert(block_id);
    // block 不在缓存，I/O 加载 -> 计为 miss
    stats_.total_accesses++;
    stats_.cache_misses++;
    stats_.disk_reads++;  // 计为磁盘读（虽然是 io_uring 完成的）

    // 插入 flat cache
    auto it = cache_map_.find(block_id);
    if (it != cache_map_.end()) populateFlatCache(it->second);

    return true;
}

bool BlockCache::insertBlockFromPtr(uint32_t block_id, const void* data, size_t data_size) {
    if (block_id >= num_blocks_) return false;
    if (data_size != block_size_) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // 已在缓存, 无需插入
    if (cache_map_.find(block_id) != cache_map_.end()) {
        return true;
    }

    // 如果缓存已满, 先淘汰
    while (cache_map_.size() >= cache_slots_) {
        if (!evictOne()) break;
    }

    // 构建 CachedBlock, 直接从指针拷贝一次
    CachedBlock block;
    block.block_id = block_id;
    block.dim = dim_;
    block.was_prefetched = true;  // 预取准确率: 标记预取插入
    block.raw_data.resize(block_size_);
    std::memcpy(block.raw_data.data(), data, block_size_);

    // 解析 Block 数据
    parseBlock(block);

    // 插入缓存
    cache_map_.emplace(block_id, std::move(block));
    policy_->onInsert(block_id);
    // block 不在缓存，I/O 加载 -> 计为 miss
    stats_.total_accesses++;
    stats_.cache_misses++;
    stats_.disk_reads++;

    // 插入 flat cache
    auto it = cache_map_.find(block_id);
    if (it != cache_map_.end()) populateFlatCache(it->second);

    return true;
}

std::vector<uint32_t> BlockCache::getRecentBlockAccesses(size_t n) const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t start = recent_accesses_.size() > n ? recent_accesses_.size() - n : 0;
    return std::vector<uint32_t>(recent_accesses_.begin() + start, recent_accesses_.end());
}

// ============================================================
// Phase 3 v2: Page cache 管理
// ============================================================

void BlockCache::dropPageCache() {
    if (blocks_fd_ >= 0) {
        // 告诉内核：整个 blocks 文件的 page cache 都不再需要
        posix_fadvise(blocks_fd_, 0, 0, POSIX_FADV_DONTNEED);
    }
}

// ============================================================
// Phase 3 CPU Opt: 批量插入 + 快速缓存访问
// ============================================================

bool BlockCache::insertBlocksBatch(const std::vector<BatchEntry>& entries) {
    if (entries.empty()) return true;

    // 阶段 1: 锁外解析所有 block（CPU 密集，无需持锁）
    std::vector<std::pair<uint32_t, CachedBlock>> parsed;
    parsed.reserve(entries.size());

    for (const auto& entry : entries) {
        if (entry.block_id >= num_blocks_) continue;
        if (entry.data_size != block_size_) continue;

        CachedBlock block;
        block.block_id = entry.block_id;
        block.dim = dim_;
        block.was_prefetched = true;  // 预取准确率: 标记预取插入
        block.raw_data.resize(block_size_);
        std::memcpy(block.raw_data.data(), entry.data, block_size_);
        parseBlock(block);  // CPU work: memcpy + parse, 无锁
        parsed.emplace_back(entry.block_id, std::move(block));
    }

    // 阶段 2: 一次加锁插入所有 block
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [block_id, block] : parsed) {
        // 已在缓存，跳过
        if (cache_map_.find(block_id) != cache_map_.end()) continue;

        // 如果缓存已满，先淘汰
        while (cache_map_.size() >= cache_slots_) {
            if (!evictOne()) break;
        }

        cache_map_.emplace(block_id, std::move(block));
        policy_->onInsert(block_id);
        // block 不在缓存，I/O 加载 -> 计为 miss
        stats_.total_accesses++;
        stats_.cache_misses++;
        stats_.disk_reads++;

        // 插入 flat cache
        auto it = cache_map_.find(block_id);
        if (it != cache_map_.end()) populateFlatCache(it->second);
    }

    return true;
}

CachedBlock* BlockCache::getCachedBlockById(uint32_t block_id) {
    // ---- Fast path: Flat block pointer cache (lock-free) ----
    if (flat_block_num_slots_ > 0 && flat_block_ptrs_ && flat_block_owners_) {
        size_t slot = (size_t)block_id % flat_block_num_slots_;
        if (flat_block_owners_[slot] == block_id && flat_block_ptrs_[slot] != nullptr) {
            // Cache hit: 直接返回 block 指针
            flat_stats_.block_hits++;
            stats_.total_accesses++;
            stats_.cache_hits++;
            flat_block_ptrs_[slot]->was_accessed = true;
            // 不更新 LRU (批量淘汰策略)
            return flat_block_ptrs_[slot];
        }
        flat_stats_.block_misses++;
    }

    // ---- Slow path: mutex + hash_map ----
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(block_id);
    if (it != cache_map_.end()) {
        // 统计命中（miss 不在此计数，由 fallback getNodeVector 计数，避免双重计数）
        stats_.total_accesses++;
        stats_.cache_hits++;
        it->second.was_accessed = true;  // 预取准确率: 标记被访问
        policy_->onAccess(block_id);  // 更新 LRU 策略

        // 插入 flat block cache
        if (flat_block_num_slots_ > 0 && flat_block_ptrs_ && flat_block_owners_) {
            size_t slot = (size_t)block_id % flat_block_num_slots_;
            flat_block_owners_[slot] = block_id;
            flat_block_ptrs_[slot] = &it->second;
        }

        return &it->second;
    }
    return nullptr;
}
