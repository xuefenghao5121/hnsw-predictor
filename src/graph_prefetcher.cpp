// graph_prefetcher.cpp - 图引导 io_uring 异步预取器实现

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

int GraphPrefetcher::submitPrefetch(const std::vector<uint32_t>& block_ids, bool auto_submit) {
    if (block_ids.empty()) return 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    int submitted = 0;
    for (uint32_t block_id : block_ids) {
        // 检查是否已在缓存中
        if (cache_->isInCache(block_id)) {
            stats_.prefetch_skipped++;
            continue;
        }

        // 检查是否已有 pending 请求
        if (pending_requests_.count(block_id)) {
            stats_.prefetch_skipped++;
            continue;
        }

        // 分配对齐缓冲区
        int buf_idx = ring_.allocBuffer();
        if (buf_idx < 0) {
            // 缓冲区池耗尽，先回收一些
            reapCompletions();
            buf_idx = ring_.allocBuffer();
            if (buf_idx < 0) {
                // 仍然没有，跳过这个 block
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
    // submit() 内部会检查 pending SQEs, 无 pending 时返回 0
    int ret = ring_.submit();
    if (ret > 0) {
        stats_.submit_calls++;
    }
}

int GraphPrefetcher::reapCompletions() {
    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<IoUring::CqeResult> results;
    results.reserve(32);

    int count = ring_.reapCompletions(results);
    stats_.reap_calls++;

    for (const auto& cqe : results) {
        processCompletion(cqe.user_data, cqe.res);
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
    while (ring_.inflight() > 0) {
        if (max_wait_us > 0) {
            auto elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::high_resolution_clock::now() - t0).count();
            if (elapsed >= max_wait_us) {
                break;  // 超时
            }
        }

        ring_.waitCompletion();
        stats_.wait_calls++;
        reapCompletions();
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
}

bool GraphPrefetcher::waitForBlock(uint32_t block_id) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // 快速检查：已在缓存中（之前已 reap 并插入）
    if (cache_->isInCache(block_id)) {
        return true;
    }

    // 检查是否已完成但未在缓存（不应该发生，因为 processCompletion 会插入缓存）
    // 如果不在 pending_requests_ 中，说明该 block 没有被提交预取
    if (pending_requests_.find((uint64_t)block_id) == pending_requests_.end()) {
        // 没有提交预取，直接返回 false
        return false;
    }

    // 等待该 block 完成：循环 wait+reap 直到该 block 出现在缓存中
    while (true) {
        // 非阻塞 reap 一次
        reapCompletions();

        if (cache_->isInCache(block_id)) {
            auto t1 = std::chrono::high_resolution_clock::now();
            stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            return true;
        }

        // 检查是否仍在 pending（可能已 reap 但失败了）
        if (pending_requests_.find((uint64_t)block_id) == pending_requests_.end()) {
            // 不在 pending 了但也不在缓存 -> 读取失败
            auto t1 = std::chrono::high_resolution_clock::now();
            stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
            return false;
        }

        // 还有 inflight，等待至少一个完成
        if (ring_.inflight() > 0) {
            ring_.waitCompletion();
            stats_.wait_calls++;
        } else {
            // 没有 inflight 但 block 仍在 pending？不应该发生
            break;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats_.total_wait_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
    return cache_->isInCache(block_id);
}

void GraphPrefetcher::waitForBlocks(const std::set<uint32_t>& needed_blocks) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // 筛选出仍在 pending 中的 block
    std::set<uint32_t> pending;
    for (uint32_t id : needed_blocks) {
        if (!cache_->isInCache(id) && pending_requests_.count((uint64_t)id)) {
            pending.insert(id);
        }
    }

    if (pending.empty()) {
        return;
    }

    // 循环: reap -> 检查哪些完成了 -> wait -> 重复
    while (!pending.empty()) {
        // 非阻塞 reap
        reapCompletions();

        // 移除已完成的 block
        for (auto it = pending.begin(); it != pending.end();) {
            if (cache_->isInCache(*it) || !pending_requests_.count((uint64_t)*it)) {
                it = pending.erase(it);
            } else {
                ++it;
            }
        }

        if (pending.empty()) break;

        // 等待至少一个 completion
        if (ring_.inflight() > 0) {
            ring_.waitCompletion();
            stats_.wait_calls++;
        } else {
            // 没有 inflight 但还有 pending? 不应该发生
            break;
        }
    }

    auto t1 = std::chrono::high_resolution_clock::now();
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
