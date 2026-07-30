# Optimization Roadmap — 实验结果与待验证项

> 更新: 2026-07-30 (DEC-052 AVX2 l2Distance 验证通过)
> 状态: 20 个 idea 已验证, 10 个待实现
> 代码状态: P1 pread merge + AVX2 l2Distance (DEC-047 + DEC-052)

---

## 全量放开差距进展

```
hnswlib 4T: 11673 QPS (固定基线)

DiskHNSW 4T 演进:
  初始标量:     5693 QPS  → 差距 2.05x
  +P1 pread合并: 6219 QPS  → 差距 1.88x
  +AVX2 l2Dist:  7231 QPS  → 差距 1.62x  ← 当前
  目标 (70%):   8171 QPS  → 差距 1.43x
```

## 已验证 Idea 总览

### ✅ 采纳 (有正收益, 代码已合入)

| ID | 优化 | 收益 | 决策 |
|----|------|------|------|
| DEC-034 | VisitedList uint32→uint8 | 2x QPS (10M) | ✅ |
| DEC-035 | FINE_PREAD 多线程路径 | recall 70%→95% | ✅ |
| DEC-036 | PQ dsub=3 SIMD | +5% QPS | ✅ |
| DEC-037 | cgroup 目标 1GB→2GB | 解除内存瓶颈 | ✅ |
| DEC-047 | P1 批量 pread 合并 | +9%~146% QPS | ✅ |
| DEC-052 | **AVX2 l2Distance** | **+16% QPS (SIFT1M)** | ✅ |

### ❌ 否决 (无收益或负收益, 代码未合入)

| ID | 优化 | 结果 | 根因 |
|----|------|------|------|
| DEC-019 | Dynamic Width | PQ 搜索不收敛 | 架构特性 |
| DEC-038 | 图裁剪 10M | 0% QPS gain | CSR 非瓶颈 |
| DEC-046 | CSR cache 增大 256→512MB | hit_rate 不变 | 工作集已在 cache 内 |
| DEC-048 | P2 query 重排序 | QPS 反降 | 预计算开销 > 缓存收益 |
| DEC-049 | Page Shuffle @10M | -2.6%~-27.8% | 破坏 BFS block 局部性 |
| DEC-050 | Page Search | 逻辑否决 | Shuffle 前提失败 |
| DEC-051 | PhaseA+Fine 融合 | 慢 20 倍 | inline I/O 分散, 批量 I/O 是架构优势 |

### 已关闭系列

| 系列 | 结论 |
|------|------|
| DEC-017~019 Page Search/Shuffle/DynamicWidth | ❌ BFS block 级重排已足够 |
| DEC-046~048 CSR cache/Query sort | ❌ 工作集已在 cache, 排序无法减少 cold miss |
| DEC-051 PhaseA+Fine 融合 | ❌ 两阶段分离是 I/O 批量化优势, 非瓶颈 |

> **核心认知: 两阶段搜索的分离不是缺陷, 而是 I/O 批量化的架构优势。**
> **l2Distance 标量→AVX2 是最低成本的优化 (19 行改动, +16%)。**

---

## 待实现 Idea (按优先级)

### 🔴 优先级 1: 全量放开性能优化 (SLA-006)

> 当前差距: 1.62x (4T: 7231 vs 11673), 距 70% 目标还差 940 QPS
> 已完成: P1 pread(+19%) + AVX2 l2(+16%), 还需 ~13% 提升

| # | 优化 | 预期 | 难度 | 状态 |
|---|------|------|------|------|
| 1 | **flat_vec_cache 增大** 32→64MB | +5~8% | 🟢 低 | 未开始 |
| 2 | **ef_search 自适应** | +5~10% | 🟡 中 | 未开始 |
| 3 | **buildPqDistTable 4-centroid AVX2** | +2~3% | 🟡 中 | 未开始 |

### 🟡 优先级 2: 紧凑部署优化 (1.5GB cgroup)

| # | 优化 | 预期 | 难度 | 状态 |
|---|------|------|------|------|
| 4 | **io_uring 批量 Fine Rerank** (Q-006) | 1.5x @1.5GB | 🔴 高 | 未开始 |
| 5 | **1-hop CSR 预取** (P3-4) | 掩盖磁盘延迟 | 🟡 中 | 未开始 |

### 🟢 优先级 3: 内存压缩 (P3 100M 前置)

| # | 优化 | 效果 | 难度 | 状态 |
|---|------|------|------|------|
| 6 | **PQ codes mmap** (P3-5) | 减少 304MB 匿名 | 🟡 中 | 未开始 |
| 7 | **上层向量 PQ 编码** (P3-6) | 减少 228MB 匿名 (10M) | 🟡 中 | 未开始 |
| 8 | **路由表内存优化** (P3-8) | 减少 ~40MB | 🟢 低 | 未开始 |

### ⚪ 优先级 4: 长期探索 (P4-P5)

| # | 优化 | 效果 | 难度 | 状态 |
|---|------|------|------|------|
| 9 | **100M 数据集** (P3-7) | 验证规模上限 | 🔴 高 | 未开始 |
| 10 | **SPDK 用户态 I/O** (DEC-027) | 2-3x I/O 带宽 | 🔴 高 | P4 探索 |

---

## 已关闭的 Open Question

| ID | 问题 | 结论 |
|----|------|------|
| Q-001 | 图遍历规模上限 | 10M 可行 ✅, 100M 需关注 CSR 4.7GB 上磁盘 |
| Q-003 | 数据集规模选择 | SIFT1M + DEEP10M ✅ |
| Q-004 | PQ M 值选择 | M=32 ✅ |
| Q-005 | 路由表方向 | dual-route-table ✅ |
| Q-006 | 批处理优先级 | P1 pread 合并已验证 ✅, io_uring 待做 |
| Q-007 | SLA 重新定义 | SLA-006 双维度 ✅ |
