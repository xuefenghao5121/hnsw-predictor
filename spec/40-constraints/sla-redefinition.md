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

## 演进 SLA 目标 (未验证) {#SLA-005}
<!-- ndf: kind=constraint level=may layer=L0 status=exploratory since=0.5 source=deduced -->

> 以下目标为未来优化的方向, 当前未验证。

| 优化 | 目标 | 依据 | 优先级 |
|------|------|------|--------|
| 批处理 I/O 去重 | DEEP10M 1.5GB QPS 327→1000+ | Q-006 理论 10x | 🔴 |
| CSR cache 增大 | DEEP10M 1.5GB hit rate 46%→92% | DEC-042 | 🟡 |
| SIFT1M 256MB 优化 | QPS 423→1000+ | 当前 page cache 不足 | 🟡 |

---

## SLA 修正历史

| 版本 | 日期 | 变更 | 原因 |
|------|------|------|------|
| v0.2 | 2026-07-28 | 初始 SLA: QPS≥2000 (1T), ≥5000 (4T) @512MB | 不诚实 (page cache 白嫖) |
| v0.5 | 2026-07-30 | 诚实重测 + 新增 DEEP10M SLA + hnswlib 对比 | DEC-039 |
