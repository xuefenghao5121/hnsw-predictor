// graph_prefetcher.cpp - 图引导 io_uring 异步预取器实现
//
// 线程安全版本: 所有公共方法通过 std::mutex 串行化
// 关键设计: ring_.waitCompletion() 不持有锁，允许其他线程并发 submit

#include "graph_prefetcher.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <algorithm>

GraphPrefetcher::GraphPrefetcher(BlockCache* cache, unsigned ring_size, bool use_odirect)
    : cache_(cache)
    , ring_(ring_size)
    , use_odirect_(use_odirect)
{
    block_size_ = cache_->getBlockSizeBytes();
    blocks_fd_ = cache_->getBlocksFd();
    header_size_ = BlockCache::getHeaderSize();

    // 设置 io_uring 缓冲区池
    ring_.setBufferSize(block_size_);

    std::cout << "[GraphPrefetcher] Initialized: ring_size=" << ring_size
              << ", block_size=" << block_size_
              << ", odirect=" << (use_odirect_ ? "yes" : "no")
              << ", fd=" << blocks_fd_ << std::endl;
}

GraphPrefetcher::~GraphPrefetcher() {
    // 时效性报告: 搜索需要 block 时它处于何种状态
    Stats s = getStats();
    size_t t = s.need_timely;
    size_t f = s.need_inflight;
    size_t n = s.need_not_prefetched;
    size_t tot = t + f + n;
    if (tot > 0) {
        std::cerr << "[Prefetch Timeliness] need_total=" << tot
                  << " timely=" << t << "(" << (100.0*t/tot) << "%)"
                  << " inflight=" << f << "(" << (100.0*f/tot) << "%)"
                  << " not_prefetched=" << n << "(" << (100.0*n/tot) << "%)"
                  << " | wait_calls=" << s.wait_calls
                  << " total_wait_us=" << s.total_wait_us
                  << std::endl;
    }
}

int GraphPrefetcher::submitPrefetch(const std::vector<uint32_t>& block_ids, bool auto_submit) {
    if (block_ids.empty()) return 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);

    // 优化：批量检查缓存，一次加锁替代 N 次加锁
    // 先过滤掉已在缓存中的 block
    std::vector<uint32_t> to_submit = cache_->filterNotInCache(block_ids);

    int submitted = 0;
    for (uint32_t block_id : to_submit) {
        // 再次检查 pending（可能在上一批 submit 中已提交）
        if (pending_requests_.count(block_id)) {
            stats_.prefetch_skipped++;
            continue;
        }

        // 分配对齐缓冲区
        int buf_idx = ring_.allocBuffer();
        if (buf_idx < 0) {
            // 缓冲区池耗尽，先回收一些（内部不加锁）
            reapCompletionsUnlocked();
            buf_idx = ring_.allocBuffer();
            if (buf_idx < 0) {
                // 仍然没有，回退到同步加载（不静默跳过）
                cache_->getBlockById(block_id);
                stats_.prefetch_skipped++;
                continue;
            }
        }

        // 计算文件偏移
        off_t offset = (off_t)header_size_ + (off_t)block_id * block_size_;

        // 提交 io_uring 读请求
        int ret = ring_.submitRead(blocks_fd_, offset, block_size_, buf_idx, (uint64_t)block_id);
        if (ret < 0) {
            ring_.freeBuffer(buf_idx);
            stats_.prefetch_failed++;
            continue;
        }

        pending_requests_[(uint64_t)block_id] = buf_idx;
        submitted++;
        stats_.prefetch_submitted++;
    }

    // 统计跳过的 block（已在缓存中的）
    stats_.prefetch_skipped += block_ids.size() - to_submit.size();

    if (submitted > 0 && auto_submit) {
        // 批量提交到内核
        ring_.submit();
        stats_.submit_calls++;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.total_submit_us += std::chrono::duration<double, std::micro>(t1 - t0).count();

    return submitted;
}

void GraphPrefetcher::flushSubmits() {
    std::lock_guard<std::mutex> lock(mutex_);
    // submit() 内部会检查 pending SQEs, 无 pending 时返回 0
    int ret = ring_.submit();
    if (ret > 0) {
        stats_.submit_calls++;
    }
}

int GraphPrefetcher::reapCompletions() {
    std::lock_guard<std::mutex> lock(mutex_);
    return reapCompletionsUnlocked();
}

int GraphPrefetcher::reapCompletionsUnlocked() {
    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<IoUring::CqeResult> results;
    results.reserve(32);

    int count = ring_.reapCompletions(results);
    stats_.reap_calls++;

    // 收集所有完成的 block，批量插入 BlockCache
    // 优化：parseBlock 在锁外完成，一次加锁插入所有 block
    std::vector<BlockCache::BatchEntry> batch_entries;
    std::vector<int> used_buf_idxs;  // 记录使用的 buffer idx，后续释放

    for (const auto& cqe : results) {
        uint32_t block_id = (uint32_t)cqe.user_data;

        auto it = pending_requests_.find(cqe.user_data);
        if (it == pending_requests_.end()) {
            continue;
        }

        int buf_idx = it->second;
        pending_requests_.erase(it);

        if (cqe.res < 0 || (size_t)cqe.res < block_size_) {
            ring_.freeBuffer(buf_idx);
            stats_.prefetch_failed++;
            continue;
        }

        // 收集到批量插入列表
        void* buf = ring_.getBuffer(buf_idx);
        batch_entries.push_back({block_id, buf, block_size_});
        used_buf_idxs.push_back(buf_idx);

        stats_.prefetch_completed++;
    }

    // 批量插入：锁外 parse + 一次加锁 insert
    // 注意: 调用 BlockCache 时持有 prefetcher mutex, 锁序: prefetcher -> cache
    // BlockCache 不会回调 prefetcher, 无死锁风险
    if (!batch_entries.empty()) {
        cache_->insertBlocksBatch(batch_entries);
    }

    // 批量插入完成后，释放所有 io_uring 缓冲区
    for (int idx : used_buf_idxs) {
        ring_.freeBuffer(idx);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.total_reap_us += std::chrono::duration<double, std::micro>(t1 - t0).count();

    return count;
}

void GraphPrefetcher::waitForCompletions(uint64_t max_wait_us) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // 先非阻塞回收
    reapCompletions();

    // 如果还有未完成的，等待
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ring_.inflight() == 0) break;
        }

        if (max_wait_us > 0) {
            auto elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::high_resolution_clock::now() - t0).count();
            if (elapsed >= max_wait_us) {
                break;  // 超时
            }
        }

        // 等待 completion（不持有锁，允许其他线程 submit）
        ring_.waitCompletion();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.wait_calls++;
        }
        reapCompletions();
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
}

bool GraphPrefetcher::waitForBlock(uint32_t block_id) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // 快速检查：已在缓存中（之前已 reap 并插入）
    if (cache_->isInCache(block_id)) {
        return true;
    }

    // 检查是否有 pending 请求
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_requests_.find((uint64_t)block_id) == pending_requests_.end()) {
            // 没有提交预取，直接返回 false
            return false;
        }
    }

    // 等待该 block 完成：循环 wait+reap 直到该 block 出现在缓存中
    while (true) {
        // 非阻塞 reap 一次
        reapCompletions();

        if (cache_->isInCache(block_id)) {
            auto t1 = std::chrono::high_resolution_clock::now();
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            return true;
        }

        // 检查是否仍在 pending（可能已 reap 但失败了）
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_requests_.find((uint64_t)block_id) == pending_requests_.end()) {
                // 不在 pending 了但也不在缓存 -> 读取失败
                auto t1 = std::chrono::high_resolution_clock::now();
                stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
                return false;
            }
            if (ring_.inflight() == 0) {
                // 没有 inflight 但 block 仍在 pending？不应该发生
                break;
            }
        }

        // 等待至少一个 completion（不持有锁）
        ring_.waitCompletion();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.wait_calls++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
    return cache_->isInCache(block_id);
}

void GraphPrefetcher::waitForBlocks(const std::set<uint32_t>& needed_blocks) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // 筛选出仍在 pending 中的 block
    std::set<uint32_t> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t id : needed_blocks) {
            bool in_cache = cache_->isInCache(id);
            bool in_flight = pending_requests_.count((uint64_t)id) > 0;
            // 时效性分类（搜索需要此 block 时的状态）
            if (in_cache) {
                stats_.need_timely++;           // 预取已到位, 完全藏住
            } else if (in_flight) {
                stats_.need_inflight++;         // 预取在途, 得等
                pending.insert(id);
            } else {
                stats_.need_not_prefetched++;   // 未预取, 纯同步 miss
            }
        }
    }

    if (pending.empty()) {
        return;
    }

    // 循环: reap -> 检查哪些完成了 -> wait -> 重复
    while (!pending.empty()) {
        // 非阻塞 reap
        reapCompletions();

        // 检查哪些已完成
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = pending.begin(); it != pending.end();) {
                if (cache_->isInCache(*it) || !pending_requests_.count((uint64_t)*it)) {
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }

            if (pending.empty()) break;

            if (ring_.inflight() == 0) {
                // 没有 inflight 但还有 pending? 不应该发生
                break;
            }
        }

        // 等待至少一个 completion（不持有锁，允许其他线程并发 submit/reap）
        ring_.waitCompletion();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.wait_calls++;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
}

void GraphPrefetcher::processCompletion(uint64_t user_data, int32_t res) {
    uint32_t block_id = (uint32_t)user_data;

    auto it = pending_requests_.find(user_data);
    if (it == pending_requests_.end()) {
        // 没有找到对应请求（不应该发生）
        return;
    }

    int buf_idx = it->second;
    pending_requests_.erase(it);

    if (res < 0 || (size_t)res < block_size_) {
        // 读取失败
        ring_.freeBuffer(buf_idx);
        stats_.prefetch_failed++;
        return;
    }

    // 零拷贝优化: 直接从 io_uring 对齐缓冲区插入 BlockCache
    // 避免了临时 vector<uint8_t> 的分配 + memcpy (Opt 3)
    void* buf = ring_.getBuffer(buf_idx);
    cache_->insertBlockFromPtr(block_id, buf, block_size_);

    // 释放缓冲区回池
    ring_.freeBuffer(buf_idx);

    stats_.prefetch_completed++;
}

// ---- 线程安全的统计/状态接口 ----

unsigned GraphPrefetcher::inflight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_.inflight();
}

GraphPrefetcher::Stats GraphPrefetcher::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;  // 返回拷贝，避免外部访问期间数据竞争
}

void GraphPrefetcher::resetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
}
