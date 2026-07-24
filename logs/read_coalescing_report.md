# Read Coalescing + Registered Buffers 实现报告

## 日期: 2026-07-23

## 1. 实现概述

### 1.1 Read Coalescing
在 `GraphPrefetcher::submitPrefetch` 中检测连续的 block ID run，将 N 个连续 block 合并为一个大的 I/O 请求（N × block_size 字节），在 `reapCompletions` 中将大 buffer 拆分为多个 block 插入 BlockCache。

### 1.2 Registered Buffers (Bonus)
在 `IoUring::setBufferSize` 中注册 buffer pool 到内核（`IORING_REGISTER_BUFFERS`），使用 `IORING_OP_READ_FIXED` 替代 `IORING_OP_READ`，跳过内核的 per-I/O buffer validation。

## 2. 文件变更

| 文件 | 变更 |
|------|------|
| `src/io_uring_wrapper.h` | 添加 `submitReadPtr()`、`hasSqeAvailable()`、`registerBuffers()`，修改 `submitRead()` 使用 `READ_FIXED` |
| `src/graph_prefetcher.h` | 添加 `PendingRequest` 结构体、coalescing stats、`pending_block_ids_` |
| `src/graph_prefetcher.cpp` | 重写 `submitPrefetch`（连续 run 检测+合并）、`reapCompletions`（拆分大 buffer）、`waitForBlock(s)`（使用 `pending_block_ids_`）|
| `src/benchmark_d4_prefetch.cpp` | 添加 coalescing stats 打印 |

## 3. 实现细节

### 3.1 PendingRequest 统一结构
```cpp
struct PendingRequest {
    int buf_idx = -1;              // buffer pool index (single), -1 for coalesced
    void* coalesced_buf = nullptr;  // dynamic buffer (coalesced)
    size_t coalesced_size = 0;      // total buffer size (coalesced)
    uint32_t first_block_id = 0;    // first block_id
    std::vector<uint32_t> block_ids; // all block_ids in request
};
```

### 3.2 Coalescing 流程
1. `submitPrefetch`: 过滤已缓存/已 pending 的 block → 排序 → 扫描连续 run
2. run_length == 1: 使用 buffer pool 单 block 读取（原路径）
3. run_length > 1: `posix_memalign` 分配 N×block_size 的大 buffer，`submitReadPtr` 提交一个 I/O
4. `reapCompletions`: 检查 `buf_idx` 判断类型，coalesced 完成时拆分大 buffer 为多个 `BatchEntry`，批量插入 BlockCache

### 3.3 Registered Buffers
- `setBufferSize` 后自动调用 `registerBuffers()`
- 注册成功 → `submitRead` 使用 `IORING_OP_READ_FIXED`
- 注册失败 → 回退到 `IORING_OP_READ`（graceful degradation）

## 4. Benchmark 结果

### 最终运行数据 (logs/d4_prefetch_coalescing_final.log)

| Config | Mean(ms) | P99(ms) | QPS | Hit% | RSS(MB) | R@HNSW |
|--------|----------|---------|-----|------|---------|--------|
| F0: Full | 0.66 | 0.91 | 1509 | 100% | 667 | 100% |
| F1: c1024 | 68.66 | 121.65 | 14.6 | 71.9% | 368 | 100% |
| F2: c1024+gp | 31.00 | 51.07 | 32.3 | 84.7% | 403 | 100% |
| F4: c512 | 148.70 | 228.75 | 6.7 | 46.6% | 233 | 100% |
| F5: c512+gp | 83.77 | 139.14 | 11.9 | 71.3% | 271 | 100% |

### Coalescing 统计 (F2)

| 指标 | 值 |
|------|-----|
| single_requests | 48473 |
| coalesced_requests | 200 |
| coalesced_blocks | 400 |
| max_coalesce_run | 2 |
| blocks_in_coalesce | 0.82% |
| io_reduction | 0.41% |
| actual_io_count | 48673 (vs 48873 without) |
| registered_bufs | enabled |

### Prefetcher 详细统计 (F2)

| 指标 | 值 |
|------|-----|
| submit_calls | 11395 |
| total_submit_us | 1.16M (avg=102 us/call) |
| reap_calls | 50875 |
| total_reap_us | 3.26M (avg=64 us/call) |
| wait_calls | 39480 |
| total_wait_us | 5.06M (avg=128 us/call) |
| total_pf_time | 9.47ms (per query) |

## 5. 关键发现

### 5.1 Coalescing 率极低 (0.82%)
- 48873 个预取 block 中，仅 200 对连续 block 被合并
- 最大 run length = 2（从未出现 3+ 连续 block）
- I/O 请求仅减少 0.41%（48873→48673）

### 5.2 根因分析
BFS 重排虽然将拓扑相邻的节点放在同一 block 内，但 HNSW 搜索时的访问模式导致预取的 block ID 不连续：
1. **HNSW 图边跨度大**：HNSW 的 skip-list 结构意味着边跨越多个 BFS 层级，邻居 block ID 分散
2. **每次 submitPrefetch 调用 block 数量少**：每次只提交当前节点邻居的 block IDs（~5-15 个 unique blocks），连续概率低
3. **256KB block size**：2225 个 block，每个 block ~450 个节点，邻居分散在多个 block 中

### 5.3 Cross-call 尝试
尝试了跨调用累积（deferred queue + threshold=16），但：
- `waitForBlocks` 在每次 candidate 处理后被调用（cache miss 时），导致 queue 被频繁 flush
- 延迟提交丢失了 I/O 与计算的 overlap，F5 从 80ms 恶化到 102ms
- 放弃 deferred 方案，保留 immediate submission

### 5.4 Registered Buffers
- 成功注册，无错误
- 预期 ~1-2% 改善，在 benchmark 噪声范围内不可见
- 实现正确，无副作用

## 6. 正确性验证

- ✅ Recall@HNSW = 100% for all configs
- ✅ 0 prefetch failures
- ✅ 48873 submitted = 48873 completed (no lost I/O)
- ✅ Build clean (no new warnings beyond pre-existing)

## 7. 结论

Read coalescing 基础设施已完整实现并验证正确。但在当前工作负载（HNSW + BFS 重排 + 256KB block）下，连续 block 访问率仅 0.82%，性能收益可忽略。

如果要提高 coalescing 率，需要：
1. **修改搜索算法**：批量处理多个 candidate 的邻居，一次性提交更多 block IDs
2. **使用更大的 block size**：减少 block 总数，增加同 block 命中率（但之前实验显示大 block 预取反而变慢）
3. **改变 block 布局**：按访问模式而非 BFS 顺序排列 block

Registered buffers 已实现并生效，预期 ~1-2% 改善（在噪声范围内）。
