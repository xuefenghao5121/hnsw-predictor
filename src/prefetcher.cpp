// prefetcher.cpp - 异步预取器实现
#include "prefetcher.h"
#include <iostream>

Prefetcher::Prefetcher(BlockCache* cache, size_t max_queue_size)
    : cache_(cache)
    , max_queue_size_(max_queue_size)
{
    worker_ = std::thread(&Prefetcher::workerLoop, this);
    std::cout << "[Prefetcher] Started (max_queue=" << max_queue_size << ")" << std::endl;
}

Prefetcher::~Prefetcher() {
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::cout << "[Prefetcher] Stopped" << std::endl;
}

void Prefetcher::submit(uint32_t block_id) {
    // 快速检查：已在缓存则跳过
    if (cache_->isInCache(block_id)) {
        stats_.prefetch_skipped++;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= max_queue_size_) {
            // 队列满，丢弃最旧的请求
            queue_.pop();
        }
        queue_.push(block_id);
    }
    stats_.prefetch_requested++;
    cv_.notify_one();
}

void Prefetcher::submitBatch(const std::vector<uint32_t>& block_ids) {
    for (uint32_t bid : block_ids) {
        submit(bid);
    }
}

void Prefetcher::flush() {
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty()) break;
        cv_.wait(lock);
    }
}

size_t Prefetcher::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void Prefetcher::resetStats() {
    stats_.prefetch_requested = 0;
    stats_.prefetch_skipped = 0;
    stats_.prefetch_loaded = 0;
    stats_.prefetch_failed = 0;
}

void Prefetcher::workerLoop() {
    while (true) {
        uint32_t block_id;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });

            if (stop_ && queue_.empty()) break;

            block_id = queue_.front();
            queue_.pop();
        }

        // 再次检查是否已在缓存（可能在排队期间被主线程加载）
        if (cache_->isInCache(block_id)) {
            stats_.prefetch_skipped++;
            continue;
        }

        // 异步加载 Block 到缓存
        // tryPrefetch 会加载 block 但不影响主线程的搜索结果
        if (cache_->tryPrefetch(block_id)) {
            stats_.prefetch_loaded++;
        } else {
            stats_.prefetch_failed++;
        }
    }
}
