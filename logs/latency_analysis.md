# c1024+GP 9x Latency Gap Analysis

## 测试环境
- 数据集: 1M SIFT vectors, dim=128
- Block: 2225 blocks × 256KB = 557MB
- Cache: 1024 slots = 256MB (全量需 557MB)
- Full-mem: 1.32 ms/query
- c1024+GP: 10.51 ms/query (7.94x slower)
- Gap: 9.19 ms/query

## GraphPrefetcher 统计 (200 queries)

| 指标 | 总量 | 每查询 | 每次调用 |
|------|------|--------|----------|
| submit() 调用 | 5167 | 25.8 | 38.3 us |
| reapCompletions() 调用 | 30584 | 152.9 | 25.3 us |
| waitForCompletions() 调用 | 13118 | 65.6 | 137.3 us |
| 提交的 block | 13982 | 69.9 | - |
| 跳过的 block (已在缓存) | 284587 | 1422.9 | - |

## 瓶颈分析 (按影响排序)

### 瓶颈 1: waitForCompletions 阻塞 - 占 9.0 ms/query (98% of gap)

**问题**: `waitForCompletions(100ms)` 等待 **所有** inflight 的 io_uring 请求完成，而不是只等待当前需要的 block。

```cpp
// 当前实现 - 等待 ALL inflight
while (ring_.inflight() > 0) {
    ring_.waitCompletion();  // 阻塞 syscall
    reapCompletions();
}
```

每次候选节点展开时：
1. submitPrefetch(邻居 blocks) → 提交 2-3 个 block
2. 处理 in-cache 邻居 (几微秒)
3. waitForCompletions → **立即阻塞**等待所有 I/O 完成

I/O 提交和等待之间几乎没有计算重叠。65.6 次等待调用/查询，每次 137us。

**根因**: Pipeline 深度太浅。应该先提交所有候选的预取，再做计算，最后才等待。

### 瓶颈 2: reapCompletions 调用过于频繁 - 占 3.87 ms/query

152.9 次 reap 调用/查询，每次 25.3us。reapCompletions 内部会加锁处理完成的 block。

### 瓶颈 3: 256KB memcpy + 内存分配 - 估算 1-2 ms/query

每个完成的 block：
```cpp
std::vector<uint8_t> raw_data(block_size_);  // 256KB 分配
std::memcpy(raw_data.data(), buf, block_size_);  // 256KB 拷贝
cache_->insertBlock(block_id, std::move(raw_data), block_size_);
```

70 blocks/query × 256KB = 17.5 MB 的分配+拷贝 per query。

### 瓶颈 4: mutex 锁竞争

每个操作都加全局锁：
- `isInCache()` - 1423 次/query (检查跳过的 block)
- `getNodeVector()` / `getNodeNeighbors()` - 每次访问节点
- `insertBlock()` - 70 次/query

估算 3000+ 次锁获取 per query。

### 瓶颈 5: submit() syscall 频率 - 0.99 ms/query

25.8 次 io_uring_enter syscall/query，每次 38us。虽然有 batching (2.71 SQEs/submit)，但调用频率仍然高。

## 优化建议 (按预期收益排序)

### 优化 1: 选择性等待 (预期收益: 5-7 ms)
```cpp
// 改为只等待需要的 block
void waitForBlock(uint32_t block_id, uint64_t max_wait_us) {
    if (!pending_requests_.count(block_id)) return;  // 已完成或未提交
    while (pending_requests_.count(block_id)) {
        ring_.waitCompletion();
        reapCompletions();
    }
}
```
在 pending neighbors 循环中，只等待当前需要的 block，而不是所有 inflight。

### 优化 2: 加深 Pipeline (预期收益: 3-5 ms)
当前：submit → 立即 wait
改进：收集所有候选的预取请求 → 批量提交 → 处理所有 in-cache → 批量 wait

### 优化 3: 零拷贝 insertBlock (预期收益: 1-2 ms)
```cpp
// 让 BlockCache 直接持有 io_uring buffer 的数据
// 避免 256KB vector 分配 + memcpy
bool insertBlock(uint32_t block_id, void* data, size_t size);
```

### 优化 4: 减少 reap 频率 (预期收益: 1-2 ms)
- reapCompletions 只在 inflight > 阈值 或 需要等待时调用
- 不要每次候选展开都 reap

### 优化 5: 细粒度锁 (预期收益: 0.5-1 ms)
- 分离读锁和写锁 (RWLock)
- 或使用 concurrent_hash_map

### 优化 6: 批量提交 (预期收益: 0.5 ms)
- 收集多个候选的预取请求，一次性 submit
- 减少从 25.8 到 ~5 次 submit/query
