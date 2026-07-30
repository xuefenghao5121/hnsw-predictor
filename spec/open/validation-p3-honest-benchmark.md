# P3 诚实 Benchmark 验证报告

> 日期: 2026-07-30
> 条件: DEEP10M, drop_caches + cgroup v2 MemoryMax, T=12
> 前提: DEC-039 (cgroup 诚实性修正)

## 测试环境

- 机器: huawei-ThinkCentre-M960t (30GB RAM, NVMe SSD)
- 数据集: DEEP10M (10M, 96-dim)
- 每次测试前: `echo 3 > /proc/sys/vm/drop_caches` (彻底清空 page cache)
- 初始化后: `posix_fadvise(DONTNEED)` 驱逐 graph.bin/PQ codes/bfs 文件页
- CSR_ON_DISK=1, CSR_CACHE_MB=256, FINE_BUFFERED=1

## 诚实结果 vs 之前不诚实结果

| cgroup | Recall | QPS (诚实) | QPS (之前) | 退化 | RSS peak |
|--------|--------|-----------|-----------|------|----------|
| 2GB | 95.15% | **1762** | 2340 | -25% | 1401 MB |
| 1.5GB | 95.15% | **327** | 1895 | -83% | 1107 MB |
| 1.2GB | 95.15% | **252** | OOM | 新可行 | 1108 MB |
| 1.1GB | - | OOM | OOM | - | - |

## 关键发现

### 1. 之前数字的不诚实程度

1.5GB cgroup 下之前测出 QPS 1895, 诚实测量只有 327, **虚高 5.8x**。
根因: 30GB 系统内存的 page cache 白嫖了 8GB+ 数据文件。

### 2. ~1.5GB 是性能悬崖

- cgroup 1.2GB → QPS 252 (page cache 预算 ~100MB, 覆盖率 3%)
- cgroup 1.5GB → QPS 327 (page cache 预算 ~400MB, 覆盖率 11%)
- cgroup 2.0GB → QPS 1762 (page cache 预算 ~900MB, 覆盖率 24%)

1.5GB → 2GB 的 5.4x 跳跃说明: 工作集大小约 900MB, 低于此则 page cache 
无法有效缓存热数据, 大量 pread 变成真实磁盘 I/O。

### 3. Recall 完全不受 cgroup 影响

所有配置下 recall 一致 (95.15%), 因为算法正确性不依赖内存大小,
只是 QPS 受 I/O 延迟影响。

### 4. CSR 上磁盘的价值确认

- 1.2GB cgroup 现在可行 (之前 OOM)
- CSR 上磁盘节省了 553MB 匿名内存
- 初始化后 fadvise(DONTNEED) 释放了 graph/PQ 文件页, 给运行时热数据腾出空间

## 对 hnswlib 的对比 (诚实条件下)

hnswlib 需要: 全量向量 (3.83GB) + 图结构 (~834MB) = ~4.7GB 常驻内存
- 2GB cgroup: OOM (无法运行)
- 4GB cgroup: 可能运行但 RSS 接近极限

DiskHNSW 在 2GB cgroup 下: QPS 1762, recall 95.15%
**DiskHNSW 在 hnswlib 无法运行的内存配置下仍然可用。**

## 下一步优化方向

基于诚实数据, 优化优先级调整为:

1. 🔴 **批处理 I/O 去重** (Q-006): 1.5GB 下 QPS 327, 批处理可能提升 3-5x
2. 🔴 **CSR cache 增大**: 256MB → 512MB, hit rate 46% → 92%
3. 🟡 **查询聚类**: 减小工作集, 提高 page cache 命中率
4. 🟡 **I/O 预取**: 1-hop 预取掩盖磁盘延迟
