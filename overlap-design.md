# HNSW-Predictor 异步 I/O 改造方案（基于 DiskANN++ 思路）

> 设计日期: 2026-07-24 | 状态: 待审核
> 基于: DiskANN++ PageSearch (arXiv:2310.00402) + 当前 HNSW-Predictor 代码

---

## 一、问题回顾

### 当前性能瓶颈

| 配置 | QPS | Recall | 说明 |
|------|-----|--------|------|
| F0: Full-mem | 1155 | 100% | 基准 |
| F2: c1024+GP (blocking) | 102 | 100% | **当前最优 recall 100% 方案** |
| F2-batch-4 (non-blocking) | **163.6** | 96.25% ⚠️ | I/O overlap 生效但 recall 崩了 |

**核心矛盾**：非阻塞搜索 QPS 163.6（比阻塞版快 60%），但 recall 只有 96.25%。修复 recall 后 overlap 消失，QPS 退回 102。

### 之前非阻塞方案失败的原因

```
当前候选: C1 (dist=0.5, block miss)  ← 最近，但 block 不在缓存
下一个:   C2 (dist=0.8, block hit)   ← 更远，但 block 在缓存

非阻塞做法: 跳过 C1，展开 C2 → lowerBound 被抬高 → C1 的邻居被错误剪枝 → recall 下降
```

**根因**：弹出 C1 后继续弹出 C2 改变了 best-first 展开顺序，C1 的子图从未被探索。

---

## 二、DiskANN++ 的启发

### DiskANN++ PageSearch 核心思路

```
1. 取 beam_width 个候选 → 提交异步 I/O
2. I/O 等待期间 → 从 PageHeap 弹出已缓存页面的顶点做展开
3. I/O 完成 → 停止 page expansion → 处理刚读取的候选
```

### 为什么不破坏 recall

**Page expansion 是"额外"工作，不替代主搜索循环**：
- 主搜索循环仍然严格 best-first
- Page expansion 只从**已缓存**页面中找有用候选，加入 candidate_set
- 被等待的候选**不会被跳过**，I/O 完成后照常展开
- lowerBound 只可能**降低**（找到更近的候选），不可能错误抬高

### 与我们之前非阻塞方案的关键区别

| | 我们的 F2-batch-4 非阻塞 | DiskANN++ PageSearch |
|--|------------------------|---------------------|
| block miss 时 | **弹出**最近候选，继续处理下一个 | **不弹出**，提交异步 I/O |
| I/O 等待期间 | 处理 candidate_set 中的其他候选 | 从**已缓存页面**做额外展开 |
| lowerBound 变化 | **可能升高**（展开了更远的候选） | **只可能降低**（找到更近的候选） |
| 被等待的候选 | **被跳过**（已弹出，deferred 处理） | **保留**，I/O 完成后展开 |
| recall | 96.25% ⚠️ | 100% ✅ |

---

## 三、改造方案设计

### 3.1 核心思路：I/O 等待期间的 Block Expansion

```
当前 searchLayer0:
  候选 C (block miss) → 同步等待 I/O → 展开 C 的邻居
                        ↑ CPU 空闲

改造后 searchLayer0WithOverlap:
  候选 C (block miss) → 提交异步 I/O → Block Expansion → I/O 完成 → 展开 C 的邻居
                        ↑ CPU 不空闲
```

**Block Expansion 做什么**：从 candidate_set 中找其他 block 在缓存的候选，展开它们的邻居。这些候选比 C 更远，但它们的邻居可能有用的，加入 candidate_set 供后续使用。

### 3.2 当前代码的两个阻塞点

分析 `searchLayer0()` 函数（disk_hnsw.cpp:195-420），有两个同步 I/O 等待：

**阻塞点 1：候选自身 block miss（line 224）**
```cpp
CachedBlock* candidateBlock = cache_->getCachedBlockById(curr_block_id);
if (!candidateBlock) {
    // 回退到 getNodeVector → 同步磁盘 I/O → 阻塞！
    const uint32_t* neighbors = cache_->getNodeNeighbors(candidateId, neighborCount);
    ...
}
```

**阻塞点 2：邻居 block miss（line 374）**
```cpp
// 收集 pending_neighbors (block 不在缓存的邻居)
// 然后同步等待所有 block
graph_prefetcher_->waitForBlocks(needed_blocks);  // 阻塞！
```

### 3.3 改造方案

#### 改造点 A：候选 block miss 时的 Block Expansion

```
原流程:
  pop C → getCachedBlockById → null → getNodeVector (同步I/O) → 展开邻居

新流程:
  pop C → getCachedBlockById → null
    → submitPrefetch(C's block, async)
    → while not I/O_complete:
        peek candidate_set.top() = C2
        if C2's block in cache:
            pop C2 → 展开 C2 的邻居 → 加入 candidate_set
        else:
            break  // 下一个也 miss，无法展开
    → wait I/O complete
    → 展开 C 的邻居
```

#### 改造点 B：邻居 block miss 时的 Block Expansion

```
原流程:
  展开 C 的邻居 → 分为 in-cache 和 pending
  → 处理 in-cache 邻居
  → waitForBlocks(pending)  // 阻塞等待所有
  → 处理 pending 邻居

新流程:
  展开 C 的邻居 → 分为 in-cache 和 pending
  → 处理 in-cache 邻居
  → submitPrefetch(pending blocks, async)
  → while not all_complete:
      reapCompletions()  // 非阻塞检查
      process ready pending neighbors
      if none ready:
        peek candidate_set.top() = C2
        if C2's block in cache:
            pop C2 → 展开 C2 的邻居  // Block Expansion
        else:
            waitForAnyBlock(pending)  // 阻塞等一个
  → 处理 remaining pending 邻居
```

### 3.4 伪代码

```cpp
searchLayer0WithOverlap(entry, query, ef, visited) {
    // ... 初始化（与现有代码相同）...

    while (!candidate_set.empty()) {
        auto [dist, nodeId] = candidate_set.top();
        if (dist > lowerBound && top_candidates.size() == ef) break;
        candidate_set.pop();

        uint32_t blockId = getBlockIdFast(nodeId);
        CachedBlock* block = cache_->getCachedBlockById(blockId);

        if (!block) {
            // ★ 改造点 A: 候选 block miss → 异步 I/O + Block Expansion
            if (graph_prefetcher_) {
                graph_prefetcher_->submitPrefetch({blockId}, false);  // async

                // Block Expansion: 展开其他 in-cache 候选
                while (true) {
                    graph_prefetcher_->reapCompletions();  // 非阻塞
                    block = cache_->getCachedBlockById(blockId);
                    if (block) break;  // I/O 完成！

                    if (candidate_set.empty()) {
                        graph_prefetcher_->waitForBlock(blockId);  // 无候选可展开，阻塞等
                        block = cache_->getCachedBlockById(blockId);
                        break;
                    }

                    auto [d2, nodeId2] = candidate_set.top();
                    if (d2 > lowerBound && top_candidates.size() == ef) {
                        // 搜索即将终止，等 I/O 即可
                        graph_prefetcher_->waitForBlock(blockId);
                        block = cache_->getCachedBlockById(blockId);
                        break;
                    }

                    uint32_t blockId2 = getBlockIdFast(nodeId2);
                    CachedBlock* block2 = cache_->getCachedBlockById(blockId2);
                    if (!block2) {
                        // 下一个候选也 miss，等 I/O
                        graph_prefetcher_->waitForBlock(blockId);
                        block = cache_->getCachedBlockById(blockId);
                        break;
                    }

                    // ★ 展开 in-cache 候选（Block Expansion）
                    candidate_set.pop();
                    expandNeighbors(nodeId2, block2, query,
                                   candidate_set, top_candidates,
                                   lowerBound, visited);
                    // 注意: nodeId 仍在候选流中（已被 pop），
                    // 但它的邻居会在 I/O 完成后展开
                }

                if (!block) continue;
            } else {
                // 无预取器，回退同步加载
                block = ... (getNodeVector 路径)
            }
        }

        // 展开原始候选的邻居
        expandNeighbors(nodeId, block, query,
                       candidate_set, top_candidates,
                       lowerBound, visited);

        // ★ 改造点 B: 邻居 block miss 时也做 Block Expansion
        // (在 expandNeighbors 内部处理 pending_neighbors 时)
    }
}
```

### 3.5 expandNeighbors 的改造

```cpp
expandNeighbors(nodeId, block, query, candidate_set, top_candidates,
                lowerBound, visited) {
    // 1. 获取邻居列表（从 block）
    neighbors = block->getNeighbors(nodeId, count);

    // 2. 提交邻居 block 预取 (fire-and-forget, 不等待)
    if (graph_prefetcher_) {
        graph_prefetcher_->submitPrefetch(neighbor_blocks, false);  // async
    }

    // 3. 处理 in-cache 邻居（与现有代码相同）
    for (neighbor : neighbors) {
        if (visited) continue;
        markVisited(neighbor);
        nBlock = getCachedBlockById(neighbor_block);
        if (nBlock) {
            dist = l2Distance(query, nBlock->getVector(neighbor));
            addToCandidateSet(...);
        } else {
            pending.push_back(neighbor);
        }
    }

    // 4. ★ 改造: pending 邻居不再同步等待
    if (!pending.empty() && graph_prefetcher_) {
        while (true) {
            graph_prefetcher_->reapCompletions();

            // 处理已就绪的 pending 邻居
            bool any_ready = false;
            for (auto& pn : pending) {
                CachedBlock* pb = cache_->getCachedBlockById(pn.blockId);
                if (pb && !pn.processed) {
                    pn.processed = true;
                    any_ready = true;
                    dist = l2Distance(query, pb->getVector(pn.neighborId));
                    addToCandidateSet(...);
                }
            }

            // 检查是否全部完成
            if (all processed) break;

            // ★ Block Expansion: 等待期间展开 in-cache 候选
            if (!any_ready && !candidate_set.empty()) {
                auto [d2, nodeId2] = candidate_set.top();
                if (d2 <= lowerBound || top_candidates.size() < ef) {
                    uint32_t blockId2 = getBlockIdFast(nodeId2);
                    CachedBlock* block2 = cache_->getCachedBlockById(blockId2);
                    if (block2) {
                        candidate_set.pop();
                        // 递归展开（但限制深度避免无限递归）
                        expandNeighborsShallow(nodeId2, block2, ...);
                    } else {
                        graph_prefetcher_->waitForAnyBlock(pending_blocks);
                    }
                } else {
                    graph_prefetcher_->waitForAnyBlock(pending_blocks);
                }
            }
        }
    }
}
```

---

## 四、Recall 100% 保证分析（★★★）

### 4.1 为什么能保持 recall 100%

**核心不变量：主搜索循环仍然严格 best-first**

1. **候选弹出顺序不变**：candidate C 被 pop 后，如果 block miss，立即提交异步 I/O。I/O 完成后 C 的邻居照常展开。C 不会被跳过。

2. **Block Expansion 只添加候选**：从 in-cache 候选 C2 展开邻居，邻居被加入 candidate_set。这不移除任何已有候选。

3. **lowerBound 只可能降低**：
   - Block expansion 展开的是比 C 更远的候选（C 是最近的）
   - 这些候选的邻居如果比当前 lowerBound 更近，会加入 top_candidates
   - 加入更近的候选 → lowerBound 降低或不变 → **不可能升高**
   - lowerBound 降低 = 搜索更选择性 = 我们有更好的结果 = 正确行为

4. **邻居不会丢失**：Block expansion 中展开的候选会 markVisited 其邻居。当原始候选 C 最终展开时，已 visited 的邻居会被跳过——但它们已经被处理过了（距离已计算，符合条件的已加入 candidate_set）。这等价于通过不同路径到达同一节点。

### 4.2 与之前非阻塞方案的对比

| 方面 | F2-batch-4 非阻塞 | 本方案 (Block Expansion) |
|------|-------------------|------------------------|
| C 的 block miss 时 | C 被 pop，deferred 处理 | C 被 pop，但 I/O 立即提交 |
| I/O 等待期间 | 处理 candidate_set 中的其他候选 | 处理 candidate_set 中的 **in-cache** 候选 |
| C 的邻居 | 被 deferred，可能被剪枝 | I/O 完成后**照常展开** |
| lowerBound | **可能升高**（展开更远候选的邻居填满 top_candidates） | **只可能降低**（Block expansion 找到更近候选） |
| visited 标记 | deferred 邻居未标记 → 重复处理 | Block expansion 标记 visited → C 展开时跳过已处理 |
| recall | 96.25% ⚠️ | **预期 100%** ✅ |

### 4.3 边界情况分析

**情况 1：Block expansion 展开了 C 的某个邻居 N**
- N 被 markVisited
- C 展开时发现 N 已 visited → 跳过
- 但 N 的距离已经被计算并加入 candidate_set（如果足够近）
- ✅ 没有信息丢失

**情况 2：Block expansion 降低了 lowerBound**
- C 的某些远邻居原本能加入 candidate_set（dist < old_lowerBound）
- 现在 dist > new_lowerBound → 被跳过
- 但这意味着我们已有 ef 个比它更近的结果 → **正确剪枝**
- ✅ 不影响 recall

**情况 3：I/O 期间 candidate_set 为空**
- 没有 in-cache 候选可展开
- 直接 waitBlock(blockId) 阻塞等待
- ✅ 等价于当前阻塞行为

**情况 4：连续多个候选 block miss**
- 第一个 miss → 提交 I/O → block expansion
- Block expansion 中遇到的候选也 miss → 不展开，直接 waitBlock
- ✅ 回退到阻塞等待，不会死锁

---

## 五、预期性能

### 5.1 overlap 机会估算

当前 F2 (c1024+GP) 的数据：
- 命中率 96.3%，I/O miss 比例 3.7%（13982/298569）
- 每次 I/O 等待约 1-2ms（SSD 随机读 256KB）
- 总 I/O 等待时间：13982 × 1.5ms ≈ 21 秒（200 查询）
- 总搜索时间：200 / 102 QPS ≈ 1.96 秒

等等，这不对。13982 次 I/O 是预取 miss，不是搜索阻塞次数。搜索阻塞发生在 `waitForBlocks` 时，而大部分预取在 `waitForBlocks` 之前已完成。

实际阻塞更可能发生在：
- `getCachedBlockById` 返回 null（候选 block 被淘汰）
- `waitForBlocks(pending_neighbors)` 时部分 block 未就绪

保守估计：每查询约 5-10 次 block miss 需要等待 I/O，每次 1-2ms。
- 每查询 I/O 等待：5-20ms
- Block expansion 可填充的时间：5-20ms
- CPU 在 5ms 内可展开约 1000 个候选（每个 ~5μs）

### 5.2 预期 QPS 提升

- **保守估计**：overlap 填充 30% 的 I/O 等待 → QPS 提升 10-15% → ~115-120 QPS
- **乐观估计**：overlap 填充 60% 的 I/O 等待 → QPS 提升 25-30% → ~130-135 QPS
- **不会达到 F2-batch-4 的 163.6 QPS**（那个方案破坏了搜索顺序，overlap 空间更大）

### 5.3 性能上限分析

Block expansion 的效果受限于：
1. **candidate_set 中 in-cache 候选的数量**：命中率 96% 时，大多数候选在 cache，可展开的很多
2. **I/O 等待时长**：SSD 延迟决定 overlap 窗口大小
3. **Block expansion 的有用性**：展开的候选是否找到更近的邻居（取决于图结构）

---

## 六、实现计划

### Phase 1: searchLayer0WithOverlap 函数（改造点 A）

**修改文件**：`src/disk_hnsw.cpp`

1. 新增 `searchLayer0WithOverlap()` 函数（基于现有 `searchLayer0`）
2. 修改候选 block miss 路径：
   - `getCachedBlockById` 返回 null 时
   - 提交异步 `submitPrefetch({blockId}, false)`
   - 进入 Block Expansion 循环
3. Block Expansion 循环：
   - `reapCompletions()` 检查 I/O 完成
   - 从 candidate_set 弹出 in-cache 候选并展开
   - I/O 完成或无候选可展开时退出

**预估工作量**：~80 行新代码

### Phase 2: expandNeighborsWithOverlap 函数（改造点 B）

**修改文件**：`src/disk_hnsw.cpp`

1. 抽取 `expandNeighbors()` 为独立函数（当前是内联代码）
2. 修改 pending_neighbors 处理：
   - 提交异步预取
   - 循环：reap + 处理 ready + block expansion
   - 替代 `waitForBlocks(needed_blocks)` 同步等待

**预估工作量**：~60 行修改

### Phase 3: Benchmark 验证

**修改文件**：`src/benchmark_overlap.cpp`

1. 新增配置 F2-overlap: c1024+GP + overlap search
2. 与 F2-single (blocking) 和 F2-batch-4 (non-blocking) 对比
3. 验证 recall 100%
4. 测量 QPS 提升

**预估工作量**：~30 行修改

### Phase 4: 参数调优

- Block expansion 深度限制（避免过度展开）
- I/O 批量提交策略
- candidate_set 大小监控

---

## 七、风险与缓解

### 风险 1: Block Expansion 过度展开导致 CPU 开销

**问题**：I/O 等待期间展开太多候选，CPU 成为瓶颈
**缓解**：限制 Block Expansion 展开的候选数量（如最多 50 个），超过后直接 waitBlock

### 风险 2: io_uring ring 满导致提交失败

**问题**：inflight 请求数过多，submitPrefetch 失败
**缓解**：在 Block Expansion 循环中检查 inflight()，如果接近 ring_size，先 reapCompletions 释放

### 风险 3: Block Expansion 展开了原始候选的邻居

**问题**：展开 C2 时发现 C 的邻居 N 也在 C2 的邻居列表中，N 被 markVisited
**分析**：这不是问题。N 的距离已被计算并加入 candidate_set（如果足够近）。C 展开时跳过 N 是正确行为——N 已通过另一条路径被发现。

### 风险 4: expandNeighbors 递归调用导致栈溢出

**问题**：改造点 B 中 Block Expansion 调用 expandNeighbors，可能递归
**缓解**：Block Expansion 中调用的 expandNeighbors 使用浅层版本（不递归做 Block Expansion），或加深度参数限制

---

## 八、与 DiskANN++ 的差异

| 方面 | DiskANN++ | 本方案 |
|------|-----------|--------|
| 图结构 | Vamana (flat) | HNSW (hierarchical) |
| I/O 粒度 | 4KB page | 256KB block |
| 缓存 | PageHeap (页级，预计算距离) | BlockCache (block 级，按需计算) |
| overlap 来源 | 从已缓存页面展开 | 从 candidate_set 中的 in-cache 候选展开 |
| 布局优化 | 同构映射 (Star Packing) | BFS 重排（已实现） |
| PQ 使用 | 是（PQ 距离过滤） | 否（全精度 L2 距离） |
| beam_width | 是（批量 I/O） | 否（单候选 I/O + block expansion） |

**我们的优势**：
- BFS 重排已实现（命中率 87-96%）
- io_uring 异步 I/O 已实现
- BlockCache + GraphPrefetcher 基础设施完善

**我们的劣势**：
- 没有 PQ，Block Expansion 需要全精度距离计算（更贵）
- block 粒度大（256KB vs 4KB），单次 I/O 更慢

---

## 九、代码位置参考

- `searchLayer0`: disk_hnsw.cpp:195-420（要改造的主函数）
- `searchLayer0NonBlocking`: disk_hnsw.cpp:443-640（之前的失败尝试，参考）
- `GraphPrefetcher`: graph_prefetcher.h（submitPrefetch/reapCompletions/waitForBlock API）
- `BlockCache::getCachedBlockById`: block_cache.h（非阻塞缓存查询）
- `benchmark_overlap.cpp`: benchmark 文件（新增 F2-overlap 配置）
