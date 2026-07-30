# 性能验证报告 2026-07-29 (P2 DEEP10M 初步)

> 验证日期: 2026-07-29
> 关联决策: DEC-018, DEC-022, DEC-023, DEC-026
> 关联文件: output/deep10m_vecblocks_64k.bin, output/pqco_deep10m_M32.bin

## 测试环境

- 数据集: DEEP10M (96D, 9.99M 向量)
- 无 cgroup（系统 page cache ~25GB 可用）
- 线程数: 1 (io_uring 路径)
- 参数: K=10, EF=50, NQ=200, REFINE_EF=200, TWO_STAGE=1, CACHE_MB=128, FLAT_VEC_MB=64
- 通用: FINE_RERANK=1, FINE_BUFFERED=1, PQ M=32 (dsub=3)
- 冷 I/O: EVICT_PAGE_CACHE=1 (每 query 后驱逐 vecblocks page cache)
- Page Search: PAGE_SEARCH=1

## 页聚类质量

| 指标 | 原始 | Shuffled | 提升 |
|------|------|----------|------|
| 页内邻居对 | 27.3% | 79.3% | +190.7% |
| 算法耗时 | - | 17,493ms | - |

## 实测结果

| 测试 | Recall | Mean | QPS | RSS |
|------|--------|------|-----|-----|
| A: 热态基线 | 94.20% | 13.35ms | 74.9 | 2480 MB |
| B: 冷态基线 | 94.20% | 14.32ms | 69.8 | 2480 MB |
| C: 冷态+PageSearch | 94.25% | 14.45ms | 69.2 | 2484 MB |
| D: 冷态+Shuffle | 94.20% | 14.41ms | 69.4 | 2480 MB |
| E: 冷态+Shuffle+PS | 94.20% | 14.41ms | 69.4 | 2485 MB |
| F: 热态+Shuffle+PS | 94.20% | 13.09ms | 76.4 | 2485 MB |

## SIFT1M vs DEEP10M 瓶颈对比

| 指标 | SIFT1M | DEEP10M |
|------|--------|---------|
| 热态 QPS | 2038 | 74.9 |
| 冷态 QPS | 803 | 69.8 |
| I/O 延迟增量 | +0.76ms (60%) | +0.97ms (7%) |
| Page Shuffle QPS gain | +2.1% | ~0% |
| Page Search recall gain | +0.5pp | +0.05pp |
| 主导瓶颈 | I/O (60%) | PQ 计算 (~80%) |

## 关键发现

### 1. 瓶颈从 I/O 转移到 PQ 计算 ⭐

在 SIFT1M 规模，query 时间分布为：PQ 10% + 图遍历 30% + I/O 60%。
在 DEEP10M 规模，分布变为：**PQ 80% + 图遍历 13% + I/O 7%**。

PQ ADC 距离计算随 M=32 和 10M 节点数增长为 O(N×M×Dsub)，而 I/O 成本相对稳定。

### 2. Page Shuffle + Page Search 在 10M 规模无效

- Page Shuffle: I/O 占比仅 7%，页局部性提升 190% 也难有可观测收益
- Page Search: 10 向量/页已比 SIFT 的 8 向量/页有更高利用率，额外扫描无增益
- 这些技术**优化了不存在的瓶颈**

### 3. P2 目标需要重新校准

| 原目标 | 实际 | 差距 |
|--------|------|------|
| recall ≥95% | 94.25% | -0.75pp |
| QPS >500 (1T) | 74.9 | 7x 不足 |
| RSS ≤1GB | 2.48GB | 2.5x 超出 |

**根因**：
- PQ 计算在 10M 规模成为计算瓶颈（非 I/O 瓶颈）
- CSR 邻接表随节点数线性增长（47MB → 1.2GB），1GB cgroup 物理上不可行
- 图遍历和 PQ 计算的常数时间在 10M 规模无法隐藏

### 4. 与 HELMSMAN 论文的印证

论文的洞察得到了验证：**不同规模的瓶颈不同**。
- HELMSMAN 在 100B 规模面对的是 I/O 瓶颈（串行图遍历）
- DiskHNSW 在 10M 规模面对的是 PQ 计算瓶颈
- 两者都需要"正确识别瓶颈 + 对症下药"

## 结论

1. **P2 的 I/O 优化策略（Page Shuffle, Page Search）价值被高估**：在 10M DEEP10M 下 I/O 非瓶颈
2. **P2 的真正优化目标应是 PQ 计算加速**：SIMD 优化、量化压缩、近似距离
3. **P2 的内存目标需放松**：CSR 邻接表 1.2GB 不可压缩，1GB cgroup 不切实际
4. **Page Shuffle 算法正确性已验证**：页共置率 79.3%，但需在 I/O 是瓶颈的规模（100M+）才能体现价值
