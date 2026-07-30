# P3 诚实 Benchmark 验证报告: SIFT1M

> 日期: 2026-07-30
> 条件: SIFT1M (128D, 1M 向量), drop_caches + cgroup v2 MemoryMax
> 前提: DEC-039 (cgroup 诚实性修正)

## 测试环境

- 机器: huawei-ThinkCentre-M960t (30GB RAM, NVMe SSD)
- 数据集: SIFT1M (128D, 1,000,000 向量, 496MB base)
- 每次测试前: `echo 3 > /proc/sys/vm/drop_caches` + evict_cache 所有数据文件
- 初始化后: `posix_fadvise(DONTNEED)` 驱逐 graph.bin/PQ codes/bfs 文件页
- 配置: TWO_STAGE=1 PQ_HYBRID=1 FINE_RERANK=1 FINE_BUFFERED=1 REFINE_EF=100

## 诚实结果 vs 之前不诚实结果

### 4 线程 (4T)

| cgroup | Recall | QPS (诚实) | QPS (之前) | 退化 | RSS peak |
|--------|--------|-----------|-----------|------|----------|
| 512MB | 95.70% | **5247** | 5808 | -10% | 275 MB |
| 384MB | 95.70% | **3744** | - | - | 271 MB |
| 256MB | 95.70% | **423** | - | - | 233 MB |

### 单线程 (1T)

| cgroup | Recall | QPS (诚实) | QPS (之前) | 退化 |
|--------|--------|-----------|-----------|------|
| 512MB | 95.70% | **2334** | 2780 | -16% |

## 关键发现

### 1. SIFT1M 受 page cache 白嫖影响较小

- 512MB cgroup 4T: QPS 仅降 10% (5808 → 5247)
- 对比 DEEP10M 2GB: QPS 降 25% (2340 → 1762)
- 对比 DEEP10M 1.5GB: QPS 降 83% (1895 → 327)

**原因**: SIFT1M 总数据文件仅 ~1.6GB (vs DEEP10M ~13GB)。
512MB cgroup 下匿名内存 ~240MB, page cache 预算 ~270MB。
SIFT1M 的 vecblocks (497MB) 虽然不能全放, 但 200 query 的工作集 (~50MB) 远小于预算。
200 query 访问的 unique 4KB 页面 ~12000 个 = 48MB, page cache 270MB 绰绰有余。

### 2. 256MB 是性能悬崖

- 384MB → QPS 3744 (page cache 预算 ~120MB, 工作集 48MB, 足够)
- 256MB → QPS 423 (page cache 预算 ~30MB, 工作集 48MB, 不够!)
- 384→256 的 8.8x 跳跃: 工作集不再全部在 page cache, 大量真实磁盘 I/O

### 3. Recall 在所有配置下一致 (95.70%)

算法正确性不依赖内存大小, 只影响速度。

### 4. 性能悬崖远低于 hnswlib 限制

- hnswlib: 726MB RSS, OOM@512MB cgroup
- DiskHNSW: 512MB cgroup QPS 5247, 384MB cgroup QPS 3744
- **DiskHNSW 在 hnswlib 完全无法运行的内存配置下仍有可用 QPS**

## 与 DEEP10M 的对比分析

| 维度 | SIFT1M | DEEP10M |
|------|--------|---------|
| 数据文件总量 | 1.6GB | 13GB |
| 512MB cgroup 影响 | -10% | N/A (未测) |
| 性能悬崖点 | ~256MB | ~1.5GB |
| 悬崖前后 QPS 比 | 8.8x | 5.4x |
| 核心瓶颈 (悬崖下) | vecblocks I/O | vecblocks + CSR I/O |
| page cache 白嫖程度 | 轻微 (~10%) | 严重 (~25-83%) |

**结论**: SIFT1M 的之前的数字 (QPS 5808) **基本可信** (诚实 5247, 仅差 10%)。
DEEP10M 的之前数字**严重不诚实** (虚高 25-83%)。

## 修正后的 SLA (CHR-003)

| 指标 | 值 (诚实) | 条件 | 状态 |
|------|----------|------|------|
| Recall@10 | ≥ 95% | 512MB cgroup, drop_caches | ✅ 95.70% |
| QPS (4T) | ≥ 5000 | 512MB cgroup, drop_caches | ✅ 5247 |
| QPS (1T) | ≥ 2000 | 512MB cgroup, drop_caches | ✅ 2334 |
| RSS | ≤ 300MB | 512MB cgroup | ✅ 275MB |
| 内存节省 | ≥ 2.5x | vs hnswlib 726MB | ✅ 2.6x (726/275) |
