// graph_prefetcher.h - 图引导 io_uring 异步预取器
//
// 核心思想:
//   HNSW 搜索时，当前节点的邻居是已知的（图结构在内存中）。
//   通过 route 表查出邻居所在的 block，提前用 io_uring 异步预取。
//
// 替代旧版 MarkovPredictor + Prefetcher (std::thread + pread)
// 优势:
//   1. 预测准确率 ~100%（图结构精确知道下一个 block）
//   2. io_uring 批量提交: 1 syscall / N blocks (vs N syscalls)
//   3. O_DIRECT 绕过 page cache, 真实省内存
//
// 设计文档: phase3-redesign.md

#pragma once

#include "block_cache.h"
#include "io_uring_wrapper.h"
#include "common.h"

#include <cstdint>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <memory>

class GraphPrefetcher {
public:
    struct Stats {
        size_t prefetch_submitted = 0;   // 提交的 io_uring 请求数
        size_t prefetch_completed = 0;   // 成功完成的预取数
        size_t prefetch_failed = 0;      // 失败的预取数
        size_t prefetch_skipped = 0;     // 已在缓存，跳过
        size_t submit_calls = 0;         // submit() 调用次数
        size_t reap_calls = 0;           // reapCompletions 调用次数
        size_t wait_calls = 0;           // waitForCompletions 调用次数
        double total_submit_us = 0;      // submit 总耗时
        double total_reap_us = 0;        // reap 总耗时
        double total_wait_us = 0;        // wait 总耗时
    };

    // 构造函数
    // cache:      BlockCache 指针（不拥有）
    // ring_size:  io_uring SQ 大小（默认 128）
    // use_odirect: 是否使用 O_DIRECT（需 cache 的 fd 也是 O_DIRECT 打开的）
    GraphPrefetcher(BlockCache* cache, unsigned ring_size = 128, bool use_odirect = true);
    ~GraphPrefetcher() = default;

    // 非拷贝
    GraphPrefetcher(const GraphPrefetcher&) = delete;
    GraphPrefetcher& operator=(const GraphPrefetcher&) = delete;

    // ---- 核心接口 ----

    // 提交一批 block 预取请求
    // block_ids: 需要预取的 block ID 列表
    // 自动过滤已在缓存中的 block
    // auto_submit: 是否自动调用 io_uring_enter 提交到内核 (false=延迟提交, 需后续调用 flushSubmits)
    // 返回实际提交的请求数
    int submitPrefetch(const std::vector<uint32_t>& block_ids, bool auto_submit = true);

    // 刷新延迟提交的预取请求到内核 (Opt 5: 批量提交)
    void flushSubmits();

    // 非阻塞地回收已完成的预取，将数据插入 BlockCache
    // 返回回收的完成数
    int reapCompletions();

    // 等待所有未完成的预取完成，并插入 BlockCache
    // 最多等待 max_wait_us 微秒，0 表示非阻塞
    void waitForCompletions(uint64_t max_wait_us = 0);

    // 等待特定 block 的预取完成（选择性等待）
    // 只等待该 block 对应的 CQE，不等无关 I/O
    // 返回 true 表示 block 已在缓存中可用
    bool waitForBlock(uint32_t block_id);

    // 批量等待多个 block 完成 (Opt 2: 深流水线 - 推迟等待)
    // 只等待 needed_blocks 中的 block, 不等无关 I/O
    // 比逐个 waitForBlock 更高效: 一次 wait+reap 可能完成多个 block
    void waitForBlocks(const std::set<uint32_t>& needed_blocks);

    // 等待任意一个 needed_blocks 中的 block 完成 (非阻塞搜索用)
    // 一次 wait+reap, 返回后可能有 0~N 个 block 就绪
    // 不保证所有 needed_blocks 都完成, 只保证至少完成一个
    void waitForAnyBlock(const std::set<uint32_t>& needed_blocks);

    // 获取当前未完成的请求数
    unsigned inflight() const { return ring_.inflight(); }

    // 统计
    const Stats& getStats() const { return stats_; }
    void resetStats() { stats_ = Stats{}; }

private:
    BlockCache* cache_;
    IoUring ring_;
    bool use_odirect_;

    // block_size from cache
    uint32_t block_size_;
    int blocks_fd_;
    size_t header_size_;

    // Buffer tracking: user_data -> buffer_idx
    // user_data = block_id (we pack block_id into 64-bit user_data)
    std::unordered_map<uint64_t, int> pending_requests_;

    Stats stats_;

    // 内部方法
    int allocBufferForBlock(uint32_t block_id);
    void processCompletion(uint64_t user_data, int32_t res);

    // 已完成但未插入缓存的 block_ids
    // (waitForBlock 检查时使用)
    std::unordered_set<uint32_t> completed_blocks_;
};
