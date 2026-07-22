# Phase 3 重设计方案：O_DIRECT + io_uring + 图引导预取

> 日期: 2026-07-23
> 状态: 设计阶段
> 前置: Phase 1 (BFS重排) ✅, Phase 2 (slim加载) ✅

---

## 一、问题诊断

### 1.1 当前实现 vs README 目标

| 指标 | README目标 | 当前实测(1M) | 差距 |
|------|-----------|-------------|------|
| 缓存命中率 | ≥ 85% | 33.8% (c64) | 远未达标 |
| P99延迟 | ≤ 2x全内存 (≈15ms) | 396ms (c64) | 26x |
| 内存压缩比 | ≥ 2x | 1x (1131=1131) | 0 |
| QPS | ≥ 60%全内存 (≈106) | 3.6 (c64) | 2% |

### 1.2 根因分析

**根因1: pread() 系统调用开销**
- 1M 数据集每次查询 ~1477 次 block 访问
- cache=64 时 ~970 次 miss，每次 pread() ~150us
- 纯 syscall 开销: 970 × 150us = 145ms（实际 275ms）
- 即使 page cache 100% 命中，syscall 本身就是瓶颈

**根因2: OS page cache 让内存节省为零**
- Block 文件 557MB 全部被 page cache 缓存
- 进程 RSS 1131MB（含 base_data 493MB 未释放）
- "磁盘模式" 变成了 "内存模式 + syscall 开销"，最差组合

**根因3: Markov 预测器无效**
- 一阶 Markov avg out-degree 191，top-k=3 覆盖率 1.6%
- 预取 skip=24203 >> load=12250，大部分预测的 block 已在 cache
- 预取线程开销（mutex + queue + pread）> 收益
- 预取本身也调 pread()，没有减少总 syscall 数

### 1.3 关键数据发现

**并发工作集分析**（来自 1M trace）：
- 每次查询访问 ~497 个 unique block（占总量 22%）
- 但**并发工作集只有 ~51 个 block**（滑动窗口=50）
- cache=64 完全可以容纳并发工作集
- 瓶颈不是 cache 大小，是**没有提前加载正确的 block**

---

## 二、新架构设计

### 2.1 架构对比

| 组件 | 旧方案 | 新方案 | 原因 |
|------|--------|--------|------|
| I/O 模型 | pread() + page cache | **O_DIRECT + io_uring** | 消除 page cache，真正省内存 |
| 预测器 | 一阶 Markov（块ID转移概率） | **图引导预取**（邻居block查表） | 准确率 ~100% vs 1.6% |
| 预取机制 | std::thread + pread | **io_uring 异步批量提交** | 1 syscall/N blocks vs N syscalls |
| 缓存淘汰 | LRU | LRU（保留） | 工作集 51 < cache 64，LRU 够用 |
| 内存管控 | 无（依赖 page cache） | **O_DIRECT 绕过 page cache** | 真实内存节省 |

### 2.2 I/O 层: O_DIRECT + io_uring

**O_DIRECT**:
- 打开 block 文件时使用 `O_DIRECT` 标志
- pread/io_uring 读取绕过 page cache，直接 DMA 到用户缓冲区
- 内存占用 = 显式 cache 大小，不再有 page cache 开销
- 要求：缓冲区 512 字节对齐，读大小 512 字节对齐

**io_uring**:
- Linux 5.1+ 异步 I/O 接口（内核 6.17 支持）
- 两个环形队列：SQ (提交队列) + CQ (完成队列)
- 批量提交：一次 `io_uring_submit()` 提交 N 个 SQE
- 每批开销 ~5us vs pread 每次开销 ~150us
- 支持 `IORING_OP_READ` 固定缓冲区读取（注册 buffer 避免每次映射）

**性能对比**:
```
旧方案: 10个block预取 = 10 × pread() = 10 × 150us = 1500us
新方案: 10个block预取 = 1 × io_uring_submit(10 SQE) + 等待CQE ≈ 60us
加速比: 25x
```

### 2.3 预测器: 图引导预取

**核心思想**: HNSW 搜索时，当前节点的邻居是已知的（graph 结构在内存中）。通过 route 表查出邻居所在的 block，提前预取。

**算法**:
```
searchLayer0(query, entry_point, ef):
    candidate_set = priority_queue({entry_point})
    
    while candidate_set not empty:
        curr = pop_nearest(candidate_set)
        curr_block = route_table[curr]
        
        # === 图引导预取 ===
        # 1. 收集邻居所在的所有 block
        prefetch_set = {}
        for neighbor in graph[curr].layer0_neighbors:
            neighbor_block = route_table[neighbor]
            if neighbor_block != curr_block 
               and neighbor_block not in cache:
                prefetch_set.add(neighbor_block)
        
        # 2. 批量提交 io_uring 预取
        if prefetch_set:
            io_uring_batch_submit(prefetch_set)
        
        # 3. 处理邻居（此时部分预取已完成）
        for neighbor in graph[curr].layer0_neighbors:
            neighbor_block = route_table[neighbor]
            if cache.contains(neighbor_block):
                vector = cache.get_vector(neighbor)    # 命中
            else:
                cache.load_block_sync(neighbor_block)  # 同步回退
                vector = cache.get_vector(neighbor)
            
            dist = distance(query, vector)
            if dist < worst_in_result_set or result_set.size < ef:
                candidate_set.push((dist, neighbor))
```

**为什么图引导比 Markov 准确得多**:
- Markov: P(next_block | current_block) — 同一个 block 可能跳到 191 个不同 block
- 图引导: neighbor ∈ graph[curr]，neighbor_block = route[neighbor] — 精确知道下一个要访问的 block
- 预测准确率接近 100%（搜索必然访问邻居，只是顺序不确定）

**BFS 聚类的作用**:
- BFS 重排后，拓扑相邻节点在同一 block
- 每个节点 ~16 个邻居 (M=16)，大部分在同 block
- 跨 block 邻居约 2-3 个/节点
- 每次查询 ~1700 节点 × 2-3 跨 block = ~4000 预取请求
- 去重后 ~200-400 unique block（但并发只需 ~51 个在 cache）

### 2.4 内存模型

**1M SIFT 内存对比**:

| 组件 | 全内存 | 新方案(cache=64) | 节省 |
|------|--------|-----------------|------|
| Layer-0 向量 | 488MB | 0（在 SSD） | 488MB |
| 上层向量 | 30MB | 30MB（常驻） | 0 |
| 图邻接表 | ~80MB | ~80MB（常驻） | 0 |
| BFS 表 | 8MB | 8MB（常驻） | 0 |
| Route 表 | 4MB | 4MB（常驻） | 0 |
| Block cache | N/A | 64 × 256KB = 16MB | N/A |
| **总计** | **~610MB** | **~138MB** | **4.4x** |

| 组件 | 全内存 | 新方案(cache=256) | 节省 |
|------|--------|------------------|------|
| **总计** | **~610MB** | **~198MB** | **3.1x** |

✅ 满足 ≥ 2x 内存压缩目标

### 2.5 预期性能估算

**假设条件**:
- NVMe SSD 读取 256KB: ~50us
- io_uring 批量提交 10 个 block: ~60us (并行 I/O)
- cache 命中: ~0.5us (内存访问)
- 每次查询: ~1700 节点, ~1477 block 访问, ~497 unique blocks
- 并发工作集: ~51 blocks (cache=64 足够)

**延迟估算**:

| 场景 | 命中率 | 计算 | Mean | vs 全内存(5.7ms) |
|------|--------|------|------|-----------------|
| 90% 命中 | 90% | 1477×(0.9×0.5us + 0.1×50us) | 8.1ms | 1.4x ✅ |
| 85% 命中 | 85% | 1477×(0.85×0.5us + 0.15×50us) | 11.8ms | 2.1x ⚠️ |
| 95% 命中 | 95% | 1477×(0.95×0.5us + 0.05×50us) | 4.4ms | 0.77x ✅ |

**关键假设**: 图引导预取 + io_uring 能达到 90%+ 命中率
- 并发工作集 51 < cache 64 ✅
- 预取准确率 ~100% ✅
- io_uring 能在搜索消费 block 之前完成加载:
  - 搜索每个 block 耗时: 5.7ms / 497 blocks ≈ 11.5ms/block
  - io_uring 加载 10 blocks: ~60us
  - 60us << 11.5ms，预取远快于消费 ✅

---

## 三、实现计划

### 3.1 需要修改的组件

| 组件 | 改动 | 工作量 |
|------|------|--------|
| `block_cache.h/cpp` | 添加 O_DIRECT 打开、对齐读取 | 中 |
| `prefetcher.h/cpp` | 重写为 io_uring 异步批量预取 | 大 |
| `disk_hnsw.h/cpp` | searchLayer0 集成图引导预取逻辑 | 中 |
| `predictor.h/cpp` | **删除**（图引导不需要训练模型） | 小 |
| `benchmark_prefetch.cpp` | 去掉 base_data 加载，修复 RSS 测量 | 小 |
| `Makefile` | 添加 -luring | 小 |
| `collect_traces.cpp` | **保留**（用于分析，但不再是必需步骤） | 无 |

### 3.2 分步实施

**Step 1: O_DIRECT + io_uring 基础设施**
- BlockCache: 使用 `O_DIRECT` 打开文件
- 实现 512 字节对齐的缓冲区分配
- 新 Prefetcher: 使用 `io_uring` 异步读取
- 验证: O_DIRECT 读取正确性

**Step 2: 图引导预取**
- DiskHNSW::searchLayer0 中集成预取逻辑
- 当前节点 → 邻居 block 查表 → io_uring 批量提交
- 同步回退: io_uring 未完成时 `pread()` (O_DIRECT) 等待
- 验证: recall@HNSW = 100%

**Step 3: Benchmark**
- E0: 全内存 (cache=num_blocks)
- E1: DiskHNSW O_DIRECT, cache=64, 无预取
- E2: DiskHNSW O_DIRECT, cache=64, 图引导预取
- E3: DiskHNSW O_DIRECT, cache=256, 无预取
- E4: DiskHNSW O_DIRECT, cache=256, 图引导预取
- 修复 RSS 测量（去掉 base_data）

**Step 4: 调优**
- 调整预取深度（1-hop vs 2-hop）
- 调整 io_uring 队列深度
- 调整 cache 大小
- 目标: 命中率 ≥ 85%, P99 ≤ 2x, 内存 ≥ 2x, QPS ≥ 60%

### 3.3 保留不变的部分

- ✅ BFS 重排（block 文件格式不变）
- ✅ Route 表（NodeID → BlockID 映射）
- ✅ Slim 加载（上层向量常驻内存）
- ✅ Block 文件格式（256KB block, header + nodes）
- ✅ LRU 淘汰策略
- ✅ 同步回退保证正确性

### 3.4 删除的部分

- ❌ MarkovPredictor（被图引导替代）
- ❌ train_markov（不需要训练）
- ❌ collect_traces（不再需要轨迹数据）
- ❌ pread() + page cache I/O 模式

---

## 四、风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| O_DIRECT 对齐问题 | 读取失败 | 使用 posix_memalign 分配对齐缓冲区 |
| io_uring 内核支持 | 编译失败 | 内核 6.17 支持，liburing-dev 安装 |
| 预取不如预期 | 命中率低 | 先实现 1-hop，再尝试 2-hop lookahead |
| NVMe I/O 延迟 | P99 超标 | 增大 cache 到 256，减少 miss |
| BFS 聚类不够好 | 跨 block 邻居多 | 尝试 1MB block size |

---

## 五、与 README 对照

| README 设计 | 新方案 | 状态 |
|------------|--------|------|
| BFS 重排 → 拓扑连续块 | 保留 Phase 1 | ✅ |
| 冷块在 SSD，热块在 DRAM | O_DIRECT 绕过 page cache | ✅ |
| io_uring 异步预取 | io_uring + 批量提交 | ✅ |
| 预测器预测下一个 Block ID | 图引导预取（更准确） | ✅ 改进 |
| GrASP/LSTM 预测器 | 延后（图引导先验证） | ⏸ 后续 |
| 内存减半 | 4.4x 压缩 (cache=64) | ✅ 超额 |
| P99 ≤ 2x 全内存 | 估算 1.4x (90%命中) | 待验证 |
| 缓存命中率 ≥ 85% | 估算 90%+ | 待验证 |
| QPS ≥ 60% 全内存 | 估算 70%+ | 待验证 |
