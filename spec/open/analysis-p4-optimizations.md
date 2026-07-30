# P4 优化分析: 批处理 I/O 去重 / CSR cache / 查询聚类

> 日期: 2026-07-30
> 类型: 开放问题分析
> 依据: DEC-039 诚实 benchmark, DEC-043~045 性能差距分析
> 关联: Q-006 (批处理优先级), CHR-003 维度 2 (全量放开目标)

---

## OPT-001: 批处理 I/O 去重 {#OPT-001}
<!-- ndf: kind=open blocks=BEH-007,SLA-006 source=verified date=2026-07-30 -->

### 问题

当前 `batchSearchConcurrent` 是朴素的多线程独立查询:
```cpp
while (true) {
    size_t i = next_idx.fetch_add(1);
    auto res = searchKnn(&queries[i * dim], k);  // 独立查询, 无跨 query 优化
}
```

每查询 Fine Rerank 读取 ~60 unique 4KB 页 (SIFT1M) / ~200 页 (DEEP10M)。
200 query 串行 = 200 × 60 = 12000 次独立的 pread, 无去重。

### 分析

**SIFT1M (512MB cgroup):**
- 200 query × 60 unique pages = 12000 page reads
- BFS 重排保证局部性, 但不同 query 访问不同区域
- 跨 query page 共享率: ~20% (相邻 query 有部分重叠)
- 去重后: 12000 × 80% = 9600 unique pages = 37.5MB
- 去重收益不大: 37.5MB < 278MB page cache 预算, 全量放开下 I/O 非瓶颈

**DEEP10M (2GB cgroup):**
- 200 query × 200 pages (REFINE_EF=300) = 40000 page reads
- 跨 query page 共享率: ~15% (查询更分散)
- 去重后: 40000 × 85% = 34000 unique pages = 133MB
- 但 vecblocks 3.7GB, page cache 预算 ~900MB
- 去重收益: 133MB vs 900MB 预算, 单批不显著
- **但**: 多批次累积后, 热页逐渐沉淀, 减少冷 miss

**DEEP10M (1.5GB cgroup, 紧凑):**
- page cache 预算仅 ~400MB
- 每批 200 query 的 133MB unique pages 中, 大部分是新页 (cold)
- 去重 + 批量 pread 合并 → 减少系统调用 + 利用 NVMe 并行
- **这是批处理收益最大的场景**

### 方案

**阶段 1: 批量 pread 合并 (低难度, 中收益)**
```
当前: 60 个独立 pread(4KB) → 60 次系统调用
优化: 排序 + 合并相邻页 → pread(64KB) → ~10 次系统调用
预期: -30% Fine Rerank 时间 (系统调用开销减少)
```

**阶段 2: 跨 query page 去重 (中难度, 中收益)**
```
当前: query1 读 page A, query2 也读 page A → 2 次 pread
优化: batch 内收集所有 pages_needed, 去重后统一 pread
预期: -15% I/O 量 (20% 跨 query 共享率)
```

**阶段 3: io_uring 批量提交 (高难度, 高收益)**
```
优化: 所有 unique pages 通过 io_uring 批量提交, 内核调度合并
NVMe QD32 → 32 个 4KB 读并行, 掩盖延迟
预期: 在 1.5GB cgroup 下 QPS 327 → 500+ (1.5x)
```

### 结论

| 场景 | 收益 | 优先级 |
|------|------|--------|
| SIFT1M 全量放开 | 低 (I/O 非瓶颈) | 🟢 |
| DEEP10M 2GB | 中 (-15% I/O) | 🟡 |
| DEEP10M 1.5GB 紧凑 | 高 (1.5x QPS) | 🔴 |
| 全量放开缩小差距 | 中 (-20% Fine 时间) | 🟡 |

**决策: 阶段 1 (批量 pread 合并) 优先实施, 对所有场景都有收益且难度低。**

---

## OPT-002: CSR cache 增大 {#OPT-002}
<!-- ndf: kind=open blocks=CON-MEM-010 source=verified date=2026-07-30 -->

### 问题

DEEP10M CSR cache 256MB (覆盖率 46.3%), hit_rate 68.4%。
直觉上增大 cache 提高覆盖率 → 减少 pread miss。

### 实验结果

| CSR cache | 覆盖率 | hit_rate | QPS (12T) | RSS |
|-----------|--------|----------|-----------|-----|
| 128MB | 23.1% | (未测) | - | - |
| **256MB** | **46.3%** | **68.4%** | **1762** | 1401MB |
| **512MB** | **92.6%** | **68.4%** | **1816** | 1401MB |

**关键发现: cache 翻倍, hit_rate 完全不变 (68.4%), QPS 仅 +3%。**

### 根因分析

```
200 query, 每查询 612 次 CSR page 访问:
  cache miss (首次访问): 38687 pages = 151MB ← unique 工作集
  cache hit (重复访问): 83737 pages = 已缓存的页面被再次访问
```

**38687 unique pages (151MB) 已经能放进 256MB cache。**
增大 cache 到 512MB 没有新的页面可以填入 — 剩余的 31.6% miss 是**每个 query 访问新区域的固有 cold miss**。

```
query 1 访问 page A,B,C,...  → 全 miss (冷)
query 2 访问 page A,D,E,...  → A 命中, D,E miss
query 3 访问 page F,G,H,...  → 全新区域, 全 miss
...
```

200 个 query 分散在 10M 向量空间中, 覆盖的 CSR 页面范围很广。
但每个 query 的 612 次访问中, 大部分是**对同几个热点页的重复访问** (图遍历反复回到 hub 节点)。

### 结论

**CSR cache 256MB 已是最优, 增大无收益。** hit_rate 68.4% 是当前访问模式的固有限制。

如果要进一步提高 CSR hit rate, 需要改变访问模式 (查询聚类 OPT-003),
而非增大缓存。

**决策: 不增大 CSR cache。256MB 是最优配置。**

---

## OPT-003: 查询聚类 {#OPT-003}
<!-- ndf: kind=open blocks=OPT-001,OPT-002 source=deduced date=2026-07-30 -->

### 问题

200 个随机 query 分散在向量空间中, 每个 query 的图遍历访问不同区域的 CSR pages 和 vecblocks pages。
如果将**相似的 query 聚类后批量处理**, 可以:

1. 提高跨 query 的 page 共享率 (相似 query 访问相似区域)
2. 减少 CSR cache miss (热页在不同 query 间复用)
3. 减少 vecblocks pread (相同 page 只读一次)

### 方案

**阶段 1: Query 重排序 (零开销)**
```
当前: query 按 file order 处理 (随机)
优化: 按 query 的第一级路由 (PQ 粗筛 top-1 的 block) 排序
     相似 query 排在一起, 连续访问相同区域
预期: CSR hit rate +10%, vecblocks page 共享 +15%
开销: 排序 O(N log N), 几乎为零
```

**阶段 2: Query 分组 + 组内去重 (低开销)**
```
优化: 用 PQ 粗筛把 query 分组 (top-1 candidate 相近的 query 一组)
     组内共享 CSR/vecblocks page cache
     组间清空或保留 cache (取决于内存预算)
预期: 在 OPT-001 批处理基础上再提升 20-30%
```

**阶段 3: Query-aware 预取 (中难度)**
```
优化: 分析一组 query 的图遍历路径, 预测共同访问的 pages
     提前批量预取, 与 PQ 计算重叠
预期: 掩盖 I/O 延迟, 在 1.5GB 紧凑模式下收益最大
```

### 预期收益估算

| 场景 | 当前 | +OPT-001 (批量pread) | +OPT-003 (聚类) | 合计 |
|------|------|---------------------|----------------|------|
| SIFT1M 512MB | 5247 | +5% | +5% | ~5800 |
| DEEP10M 2GB | 1762 | +10% | +15% | ~2200 |
| DEEP10M 1.5GB | 327 | +30% | +30% | ~550 |

### 结论

**查询聚类的收益取决于 query 分布。** 如果 query 高度分散 (如真实搜索流量),
聚类收益有限; 如果 query 有明显聚类结构 (如推荐系统的相似 item 检索), 收益大。

**决策: 阶段 1 (query 重排序) 零开销, 应立即实施。阶段 2/3 视批处理效果再定。**

---

## 优先级排序

综合三个优化的分析:

| 优先级 | 优化 | 难度 | 收益 (1.5GB) | 收益 (全量放开) | 实施顺序 |
|--------|------|------|-------------|---------------|---------|
| 🔴 P1 | OPT-001 阶段1: 批量 pread 合并 | 🟢 低 | +15% | +5% | 立即 |
| 🔴 P2 | OPT-003 阶段1: query 重排序 | 🟢 低 | +10% | +5% | 立即 |
| 🟡 P3 | OPT-001 阶段2: 跨query去重 | 🟡 中 | +15% | +5% | P1后 |
| 🟡 P4 | OPT-001 阶段3: io_uring 批量 | 🔴 高 | +30% | +5% | P3后 |
| ❌ | OPT-002: CSR cache 增大 | - | **0%** | **0%** | **不做** |
| 🟡 P5 | OPT-003 阶段2: query 分组 | 🟡 中 | +20% | +10% | P2后 |

**CSR cache 增大 (OPT-002) 已通过实验否决, 不再考虑。**
