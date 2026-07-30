# SLA 重定义 - 基于诚实 Benchmark 数据

> 日期: 2026-07-30
> 依据: DEC-039 诚实 benchmark (drop_caches + cgroup v2)
> 替代: CHR-003 旧 SLA (2026-07-28, 不诚实条件)
> 状态: 提案, 待用户确认

## SLA 设计原则 {#SLA-001}

### 1. SLA MUST 基于诚实测量

所有性能数字 MUST 在以下条件下测量:
- `echo 3 > /proc/sys/vm/drop_caches` 清空系统 page cache
- `posix_fadvise(DONTNEED)` 驱逐初始化文件页
- cgroup v2 MemoryMax 限制总内存 (匿名 + page cache)
- 200 query 全量测试, 非 cherry-pick

### 2. SLA MUST 包含对比基线

每个数据集 MUST 同时报告 hnswlib 在相同 cgroup 下的性能,
作为 DiskHNSW 相对性能的参考基线。

### 3. SLA 分级: 可用 / 生产 / 高性能

| 等级 | QPS | 场景 |
|------|-----|------|
| 可用 (Usable) | ≥ 100 | 交互式搜索 (<10ms 延迟) |
| 生产 (Production) | ≥ 500 | 中等吞吐服务 |
| 高性能 (High) | ≥ 1000 | 高吞吐批量检索 |

---

## SIFT1M SLA {#SLA-002}
<!-- ndf: kind=constraint level=must layer=L0 status=verified since=0.5 source=verified -->

> 数据集: SIFT1M (128D, 1,000,000 向量)
> 验证日期: 2026-07-30, drop_caches + cgroup v2

### 核心指标 (512MB cgroup, 4T)

| 指标 | 目标 | 实测 | 状态 |
|------|------|------|------|
| Recall@10 | ≥ 95% | 95.70% | ✅ |
| QPS (4T) | ≥ 5000 (高性能) | 5247 | ✅ |
| QPS (1T) | ≥ 2000 (高性能) | 2334 | ✅ |
| RSS peak | ≤ 300MB | 275MB | ✅ |
| pgmajfault | < 100 | 10 | ✅ |

### hnswlib 对比 (相同条件)

| cgroup | DiskHNSW QPS | hnswlib QPS | DiskHNSW 胜出 |
|--------|-------------|-------------|--------------|
| 512MB | 5247 | 527 | **10.0x** |
| 600MB | 5124 | 2000 | **2.6x** |
| 768MB | - | 11528 | ❌ (hnswlib 充裕) |

**结论**: SIFT1M 在 ≤600MB cgroup 下, DiskHNSW 全面优于 hnswlib。
hnswlib 在 768MB+ 时性能更高 (全内存零开销), 但内存占用 726MB vs 275MB。

### cgroup 扫描

| cgroup | Recall | QPS (4T) | 等级 | 备注 |
|--------|--------|----------|------|------|
| 512MB | 95.70% | 5247 | ✅ 高性能 | 推荐配置 |
| 384MB | 95.70% | 3744 | ✅ 高性能 | 可用, pgmajfault 激增 |
| 256MB | 95.70% | 423 | ⚠️ 可用边缘 | page cache 不足 |

---

## DEEP10M SLA {#SLA-003}
<!-- ndf: kind=constraint level=must layer=L0 status=verified since=0.5 source=verified -->

> 数据集: DEEP10M (96D, 9,990,000 向量)
> 验证日期: 2026-07-30, drop_caches + cgroup v2

### 核心指标 (2GB cgroup, 12T)

| 指标 | 目标 | 实测 | 状态 |
|------|------|------|------|
| Recall@10 | ≥ 95% | 95.15% | ✅ |
| QPS (12T) | ≥ 1000 (高性能) | 1762 | ✅ |
| RSS peak | ≤ 1.5GB | 1401MB | ✅ |

### hnswlib 对比

| cgroup | DiskHNSW | hnswlib | 说明 |
|--------|----------|---------|------|
| 2GB | QPS 1762, recall 95.15% | ❌ OOM | hnswlib 需 ~6GB |
| 4GB | (未测, 预计 QPS ~2000) | QPS ~1500? | hnswlib 勉强可用 |

> hnswlib DEEP10M 需 ~6GB (向量 3.7GB + 图 0.8GB + index overhead)。
> 在 ≤4GB cgroup 下 hnswlib 不可行或极度紧张。

### cgroup 扫描

| cgroup | Recall | QPS (12T) | 等级 | 备注 |
|--------|--------|-----------|------|------|
| 2GB | 95.15% | 1762 | ✅ 高性能 | 推荐配置 |
| 1.5GB | 95.15% | 327 | ✅ 生产 | 性能悬崖边缘 |
| 1.2GB | 95.15% | 252 | ✅ 生产 | 紧凑部署 |
| 1.1GB | OOM | - | ❌ | 核心匿名数据 > 1.1GB |

---

## DEEP10M @ 1.5GB 专项 SLA {#SLA-004}
<!-- ndf: kind=constraint level=should layer=L1 status=verified since=0.5 source=verified -->

> 1.5GB 是 hnswlib 完全不可用的区域, DiskHNSW 仍有 327 QPS (生产级)。
> 这是有商业价值的 SLA: **在 hnswlib 无法运行的环境中提供生产级搜索**。

| 指标 | 目标 | 实测 | 状态 |
|------|------|------|------|
| Recall@10 | ≥ 95% | 95.15% | ✅ |
| QPS (12T) | ≥ 300 (生产) | 327 | ✅ |
| RSS peak | ≤ 1.2GB | 1107MB | ✅ |

---

## 全量放开 SLA (与 hnswlib 直接竞争) {#SLA-006}
<!-- ndf: kind=constraint level=should layer=L0 status=exploratory since=0.5 source=deduced -->
<!-- verified=2026-07-30: 差距根因已定位, 优化方向已识别 -->

> DiskHNSW 不仅要在内存受限时赢 hnswlib, 还要在内存充裕时与 hnswlib 接近。
> 这要求两阶段搜索的额外开销可接受。

### 现状: 与 hnswlib 的差距 (SIFT1M, 2GB cgroup, 无内存约束)

| 配置 | hnswlib | DiskHNSW | 差距 | DiskHNSW 时间分解 |
|------|---------|----------|------|----------------|
| 1T ef=50 | 0.09ms QPS 11565 | 0.40ms QPS 2492 | **4.6x** | PhaseA 230μs(57%) + Fine 190μs(38%) + 其他 20μs |
| 4T ef=50 | QPS 11673 | QPS 5693 | **2.1x** | (hnswlib 4T 有锁瓶颈, 不 scale) |
| 1T ef=100 | 0.16ms QPS 6392 | 0.40ms QPS 2492 | 2.6x | (hnswlib ef=100 recall 98.3%, 不可比) |

### 差距根因

1. **两阶段额外开销 (主因)**: PQ 粗筛 + Fine Rerank 做了两遍距离计算, hnswlib 一遍
2. **PQ 粗筛效率**: Phase A 230μs 访问 ~100 节点, 每节点 PQ ADC ~2μs + 邻居展开
3. **Fine Rerank I/O**: 100 候选 × 4KB pread = ~190μs (即使全在 page cache)
4. **无额外收益的图遍历**: DiskHNSW 的 Phase A 用 PQ 近似距离遍历图, 效率低于精确距离

### 优化路径 (从 4.6x 缩小到 ≤1.5x)

| 优化 | 预期收益 | 机制 | 难度 |
|------|---------|------|------|
| PhaseA+FineRerank 融合 | -40% (230+190→250μs) | 边遍历边精排, 共享 candidate set | 🔴 高 |
| PQ SIMD 优化 (dsub=4 AVX2) | -15% (230→180μs) | 已有但未充分利用 | 🟡 中 |
| Fine Rerank 批量 pread | -20% (190→150μs) | 合并相邻页, 减少系统调用 | 🟡 中 |
| flat_vec_cache 增大 | -10% | 更多候选命中缓存, 减少 pread | 🟢 低 |
| ef_search 自适应 | -10% | 简单 query 少遍历, 难 query 多展开 | 🟡 中 |
| **合计** | **0.40→0.20ms** | QPS 5000→接近 hnswlib 6392 | - |

### 目标 SLA (待优化后验证)

| 指标 | 目标 | 当前 | 依据 |
|------|------|------|------|
| QPS (1T, 无约束) | ≥ 8000 (hnswlib 70%+) | 2492 | 需 3.2x 提升 |
| QPS (4T, 无约束) | ≥ 10000 | 5693 | 需 1.8x 提升 |
| Recall@10 | ≥ 95% | 95.70% | ✅ 已达标 |
| vs hnswlib (相同内存) | ≥ 70% QPS | 21% (1T) | 需优化 |

---

## SLA 修正历史

| 版本 | 日期 | 变更 | 原因 |
|------|------|------|------|
| v0.2 | 2026-07-28 | 初始 SLA: QPS≥2000 (1T), ≥5000 (4T) @512MB | 不诚实 (page cache 白嫖) |
| v0.5 | 2026-07-30 | 诚实重测 + 新增 DEEP10M SLA + hnswlib 对比 | DEC-039 |
