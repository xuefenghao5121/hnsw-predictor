// prefetcher.h - 异步预取器：后台线程预取预测的 Block
//
// 设计要点：
// 1. 后台线程从队列取 Block ID，异步加载到 BlockCache
// 2. 主线程搜索时检查缓存（可能已被预取命中）
// 3. 降级：未命中时主线程同步加载
#pragma once

#include "block_cache.h"
#include <cstdint>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

class Prefetcher {
public:
    explicit Prefetcher(BlockCache* cache, size_t max_queue_size = 64);
    ~Prefetcher();

    // 提交预取请求（非阻塞）
    void submit(uint32_t block_id);
    void submitBatch(const std::vector<uint32_t>& block_ids);

    // 等待所有待处理预取完成
    void flush();

    // 统计
    struct Stats {
        std::atomic<size_t> prefetch_requested{0};   // 总请求数
        std::atomic<size_t> prefetch_skipped{0};      // 已在缓存，跳过
        std::atomic<size_t> prefetch_loaded{0};       // 成功加载
        std::atomic<size_t> prefetch_failed{0};       // 加载失败
    };

    const Stats& getStats() const { return stats_; }
    size_t pendingCount() const;

    // 重置统计
    void resetStats();

private:
    BlockCache* cache_;
    std::queue<uint32_t> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    size_t max_queue_size_;
    Stats stats_;

    void workerLoop();
};
