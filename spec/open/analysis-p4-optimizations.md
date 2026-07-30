# P4 优化分析总结 (2026-07-31 定稿)

## 分析背景

P4 阶段目标: 缩小全量放开场景下 DiskHNSW 与 hnswlib 的 QPS 差距。
起始差距: 2.05x (DEC-047 前) → 最终差距: **1.65x** (DEC-052 后)。

## 优化决策链

```
DEC-047 P1 pread 合并 ──────── ✅ +9~146% (SIFT1M/DEEP10M)
  │
DEC-052 AVX2 l2Distance ────── ✅ +16% (SIFT1M 4T)
  │
DEC-051 PhaseA+Fine 融合 ───── ❌ inline I/O 20x 慢
  │
DEC-053 flat_vec_cache 解耦 ── ❌ L3 局部性恶化
  │
DEC-054 ef_search 自适应 ───── ❌ PQ margin recall≥95% 无效
  │
DEC-055 io_uring Fine Rerank ─ ❌ page cache 均衡器
```

## 失败实验的共同教训

### 教训 1: Page Cache 是 I/O 优化的前提
所有 buffered I/O 优化在 page cache warm 时无效。
30GB RAM 的开发机上 SIFT1M(496MB)/DEEP10M(3.7GB) 全部在 page cache 中。
**推论**: 只有 page cache miss 的场景 (100M+, O_DIRECT, 紧凑 cgroup) 才能验证 I/O 优化。

### 教训 2: CPU Cache 层次决定 hash table 性能
flat_vec_cache 增大看似提高覆盖率, 但 hash table 数组超过 L3 cache (~12MB) 后
lookup 延迟急剧增加。64MB 最优是因为热部分恰好 fit L3。

### 教训 3: Early Termination 已隐式优化了简单 query
searchLayer0 的 BFS + priority queue 在候选距离超过 lowerBound 时自动 break,
不需要外层 ef_search 自适应。

## 微基准数据 (供未来参考)

### io_uring vs pread (warm page cache, 100 pages/query)
| 方法 | 延迟 |
|------|------|
| pread 100×4KB (逐页) | 28.9 μs |
| pread 10×40KB (合并) | 13.7 μs |
| io_uring 100×4KB (逐页) | 24.3 μs |
| io_uring 10×64KB (合并, registered) | **0.8 μs** |

io_uring + registered buffers + merged reads 理论快 17x。
实际无法复现因为: (1) unordered_map overhead, (2) batch memcpy, (3) page cache 命中。

## 有效改动的 ROI

| 改动 | 代码量 | SIFT1M 收益 | DEEP10M 收益 | 难度 |
|------|--------|-------------|-------------|------|
| P1 pread 合并 | ~30 行 | +29% | +9% (2GB) / +146% (1.5GB) | 🟢 低 |
| AVX2 l2Distance | ~15 行 | +16% | 未测 | 🟢 低 |
| 合计 | ~45 行 | **+50%** 累计 | **+9~146%** | — |
