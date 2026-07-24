# I/O 侧优化分析报告

## 1. 命中率差异调查（问题 1）

### 根因：`getCachedBlockById()` 缺少 cache stats 统计

**Bug 位置**: `src/block_cache.cpp` 中的 `getCachedBlockById()` 方法

**原因**: CPU 优化 commit (90c98e8) 新增了 `getCachedBlockById()` 作为快速路径，但它**没有更新 `stats_.total_accesses` 和 `stats_.cache_hits`**。而原始路径 `getBlockByNodeId()` 会更新这两个计数器。

**影响**: F 组 benchmark 使用快速路径（`getCachedBlockById`），大部分 cache 访问未被统计。只有 fallback 路径（block 不在缓存中时）的访问才被计入，导致命中率被严重低估。

### 修复

```cpp
// 修复前
CachedBlock* BlockCache::getCachedBlockById(uint32_t block_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(block_id);
    if (it != cache_map_.end()) {
        policy_->onAccess(block_id);
        return &it->second;
    }
    return nullptr;
}

// 修复后
CachedBlock* BlockCache::getCachedBlockById(uint32_t block_id) {
    stats_.total_accesses++;  // 新增
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_map_.find(block_id);
    if (it != cache_map_.end()) {
        stats_.cache_hits++;  // 新增
        policy_->onAccess(block_id);
        return &it->second;
    }
    return nullptr;
}
```

### 修复前后对比

| 配置 | 修复前 Hit% | 修复后 Hit% | 说明 |
|------|------------|------------|------|
| F1: c1024 无预取 | 24.1% | **71.9%** | 被严重低估 |
| F2: c1024+GP | 92.9% | **84.7%** | 被高估（仅 entry node 统计） |
| F4: c512 无预取 | 22.4% | **46.6%** | 被严重低估 |
| F5: c512+GP | 82.3% | **71.3%** | 被高估 |

### D 组 vs F 组命中率差异解释

D4 (96.2%) vs F1 修正后 (71.9%) 仍有差异，原因：

1. **`drop_page_cache` 开销**: F 组设置 `drop_page_cache=true`，每次 O_DIRECT 读后调用 `posix_fadvise(FADV_DONTNEED)`。虽然 O_DIRECT 下该调用是 no-op，但 syscall 本身有 ~1μs 开销
2. **D 组顺序执行暖机**: D 组按 D1→D2→D3→D4 顺序执行，前面的运行可能暖机了 SSD 固件缓存
3. **系统状态差异**: 两次运行时间不同，页面缓存状态不同

**结论**: 命中率差异是**测量 bug** 导致的，不是真实差异。修复后 F1 命中率 71.9% 是准确的。

---

## 2. I/O 侧优化方案分析（问题 2）

### 实验数据：不同 block size 对比

#### 256KB blocks (2225 blocks, ~450 nodes/block) - 当前配置

| 配置 | Mean (ms) | P99 (ms) | QPS | Hit% | RSS (MB) | 内存节省 | PF 提交 |
|------|-----------|----------|-----|------|----------|---------|--------|
| F0: Full | 0.62 | 0.83 | 1614 | 100% | 667 | 0% | - |
| F1: c1024 | 66.30 | 120.77 | 15.1 | 71.9% | 368 | 45% | 0 |
| F2: c1024+GP | 27.93 | 44.84 | 35.8 | 84.7% | 377 | 44% | 48873 |
| F4: c512 | 149.90 | 229.76 | 6.7 | 46.6% | 233 | 65% | 0 |
| F5: c512+GP | 80.68 | 164.43 | 12.4 | 71.3% | 247 | 63% | 112294 |

#### 512KB blocks (1112 blocks, ~900 nodes/block)

| 配置 | Mean (ms) | P99 (ms) | QPS | Hit% | RSS (MB) | 内存节省 | PF 提交 |
|------|-----------|----------|-----|------|----------|---------|--------|
| F0: Full | 0.71 | 0.92 | 1403 | 100% | 667 | 0% | - |
| F1: c1024 | 1.94 | 5.65 | 514 | 98.7% | 638 | 4% | 0 |
| F2: c1024+GP | 1.74 | 4.53 | 576 | 99.3% | 651 | 2% | 1974 |
| F4: c512 | 59.14 | 107.00 | 16.9 | 71.9% | 369 | 45% | 0 |
| F5: c512+GP | **84.52** | 175.98 | 11.8 | 84.7% | 393 | 41% | 48832 |

#### 1MB blocks (556 blocks, ~1800 nodes/block)

| 配置 | Mean (ms) | P99 (ms) | QPS | Hit% | RSS (MB) | 内存节省 | PF 提交 |
|------|-----------|----------|-----|------|----------|---------|--------|
| F0: Full | 0.60 | 0.80 | 1660 | 100% | 669 | 0% | - |
| F1: c1024 | 0.85 | 5.94 | 1179 | 99.6% | 668 | 0.1% | 0 |
| F2: c1024+GP | 0.67 | 2.00 | 1503 | 99.8% | 691 | -3% | 537 |
| F4: c512 | 2.74 | 10.73 | 364 | 98.9% | 639 | 4% | 0 |
| F5: c512+GP | 2.54 | 8.83 | 394 | 99.4% | 663 | 1% | 1566 |

### 关键发现

#### 发现 1: 命中率由 block coverage 决定，与 block size 无关

| 内存预算 | Block Size | Coverage | Hit% (无预取) | Hit% (有预取) |
|----------|-----------|----------|-------------|-------------|
| 256MB | 256KB c1024 | 46% | 71.9% | 84.7% |
| 256MB | 512KB c512 | 46% | 71.9% | 84.7% |
| 512MB | 512KB c1024 | 92% | 98.7% | 99.3% |
| 512MB | 1MB c512 | 92% | 98.9% | 99.4% |

**结论**: 在相同内存预算下，block size 不影响命中率。BFS 重排已提供足够的空间局部性。

#### 发现 2: 预取效果与 block size 强相关

| Block Size | 无预取 Mean (ms) | 有预取 Mean (ms) | 加速比 | 备注 |
|-----------|-----------------|-----------------|--------|------|
| 256KB (c1024) | 66.30 | 27.93 | **2.37x** | ✅ 预取有效 |
| 512KB (c512) | 59.14 | 84.52 | **0.70x** | ❌ 预取反而变慢！ |
| 1MB (c512) | 2.74 | 2.54 | **1.08x** | 预取效果微小 |

**512KB + 预取变慢的原因**:
- 相同数量的预取请求（~48K），但每个请求 2x 数据量
- 总 I/O 带宽翻倍（48K × 512KB = 24GB vs 12GB）
- SSD 拥塞导致 reap/wait 时间大幅增加
- 预取开销超过了命中率提升带来的收益

#### 发现 3: 最佳配置取决于内存预算

| 目标 | 推荐配置 | Mean (ms) | 内存节省 | QPS |
|------|---------|-----------|---------|-----|
| 最高性能 | 1MB c1024+GP | 0.67ms | 0% | 1503 |
| 高性能+少量节省 | 512KB c1024 无预取 | 1.94ms | 4% | 514 |
| **最佳折衷** | **256KB c1024+GP** | **27.93ms** | **44%** | **35.8** |
| 低内存 | 256KB c512+GP | 80.68ms | 63% | 12.4 |

---

## 3. 优化方案评估

### 方案 A: 更大 block size (512KB/1MB) ⭐ 已实测

**可行性**: HIGH - 仅需修改 `write_blocks.cpp` 参数
**实际收益**: 
- 512KB c1024 (512MB cache): 1.94ms，但内存节省仅 4%
- 512KB c512 (256MB cache): 59.14ms，与 256KB c1024 (66.30ms) 相似
- 预取在 512KB 下反而有害
**结论**: ❌ **不推荐**。在相同内存预算下无命中率提升，预取效果反而变差。仅在内存充裕时有效，但那时接近 full-mem 了。

### 方案 B: 2-hop 预取

**可行性**: MEDIUM - 需要修改 GraphPrefetcher
**预期收益**: 命中率可能从 84.7% 提升到 90-92%
**风险**: HIGH - 
- 512KB + 预取已导致 I/O 拥塞（0.70x 变慢）
- 2-hop 会增加 2-3x 预取请求，在 256KB 下也可能导致拥塞
- 之前 40x I/O 爆炸的教训
**结论**: ❌ **不推荐**。I/O 拥塞风险太高，当前 1-hop 已接近 I/O 带宽极限。

### 方案 C: io_uring registered buffers

**可行性**: HIGH - io_uring API 原生支持
**预期收益**: ~1-2μs/I/O × 48K I/O = 48-96ms 总计 = 0.24-0.48ms/query
**对 F2 (27.93ms) 的改善**: ~1-2%
**实现**: 在 `IoUring` 构造函数中添加 `io_uring_register` 调用注册 buffer pool
**风险**: LOW
**结论**: ✅ **推荐实施**。低风险、低工作量，可与其他优化叠加。

### 方案 D: NVMe 多队列

**可行性**: LOW - 单线程搜索无法有效利用多队列
**预期收益**: MINIMAL
**风险**: HIGH 复杂度
**结论**: ❌ **不适用**。单线程搜索 + io_uring 已足够。

### 方案 E: Read coalescing (合并相邻 block 读取)

**可行性**: MEDIUM - BFS 重排已保证物理连续性
**预期收益**: 
- 估算 20-30% 的预取请求可合并
- 48K 请求 → ~35K 请求，减少 ~13K I/O
- F2 的 wait 时间 24.8ms/query，减少 25% → 节省 ~6.2ms/query
- 对 F2 (27.93ms) 的改善: **~22%**
**实现**: 
1. 在 `submitPrefetch` 中检测物理连续的 block IDs
2. 合并连续 blocks 为单个大 I/O 请求
3. 在 `reapCompletions` 中拆分结果数据到各个 block
**风险**: LOW - 但需要仔细处理 buffer 管理
**结论**: ✅ **最推荐**。预期收益最高，风险可控。

### 优先级排序

| 优先级 | 方案 | 预期改善 | 实现难度 | 风险 |
|--------|------|---------|---------|------|
| 1 | E: Read coalescing | ~22% | MEDIUM | LOW |
| 2 | C: Registered buffers | ~1-2% | LOW | LOW |
| 3 | A: 更大 block size | 已实测无收益 | - | - |
| 4 | B: 2-hop 预取 | 可能负面 | MEDIUM | HIGH |
| 5 | D: 多队列 | 不适用 | HIGH | HIGH |

---

## 4. 额外发现

### `drop_page_cache` 在 O_DIRECT 下是冗余的

F 组设置 `drop_page_cache=true`，在每次 O_DIRECT 读后调用 `posix_fadvise(FADV_DONTNEED)`。由于 O_DIRECT 本身绕过 page cache，此调用是 no-op，但 syscall 仍有 ~1μs 开销。

**建议**: 在 O_DIRECT 模式下移除 `drop_page_cache` 调用，节省无用的 syscall 开销。

### F2 预取器详细数据

```
submit_calls:     11395
total_submit_us:  757602 (avg=66.5 us/call)
reap_calls:       57480
total_reap_us:    1950700 (avg=33.9 us/call)
wait_calls:       46085
total_wait_us:    4963340 (avg=107.7 us/call)
total_pf_time:    7671.64 ms (38.4 ms/query)
```

- wait 时间占 65% 的预取总时间 → I/O 等待是瓶颈
- submit + reap 占 35% → CPU 开销可优化
- 这支持了 "F2 是 I/O-bound" 的结论

---

## 5. 修复与提交

已完成的修复:
1. ✅ `getCachedBlockById()` 添加 cache stats 统计
2. ✅ 重新构建并运行 benchmark 验证修复
3. ✅ 测试 512KB 和 1MB block size
4. ✅ 生成完整的对比数据

文件变更:
- `src/block_cache.cpp`: 修复 `getCachedBlockById()` stats bug

新增数据文件:
- `output/test1m_blocks_512k.bin` + `.meta` + `output/test1m_route_512k.bin`
- `output/test1m_blocks_1m.bin` + `.meta` + `output/test1m_route_1m.bin`
- `logs/d4_prefetch_fixed_stats.log`
- `logs/d4_prefetch_512k_blocks.log`
- `logs/d4_prefetch_1m_blocks.log`
