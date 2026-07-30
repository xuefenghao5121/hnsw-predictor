# P3 验证: CSR 分页加载 (DEEP10M 初步验证)

> 验证日期: 2026-07-30
> 关联: P3-ARCH-001, P3-ARCH-002, P3-ARCH-003, P3-BEH-001
> 状态: 在 10M 规模验证架构可行性, 100M 规模待数据集

## 实现内容

1. **CSRCache** (`include/csr_cache.h`): LRU 页缓存 + pread 4KB 分页
   - shared_mutex 线程安全 (shared_lock 读命中, unique_lock 写入/淘汰)
   - slot-based LRU 淘汰
   - 双检查避免重复加载

2. **CSR 磁盘模式** (`disk_hnsw.cpp`): `CSR_ON_DISK=1` 环境变量
   - buildInMemoryAdjacency 后, compact_ 写入 /tmp 文件
   - 释放 553MB 内存 (DEEP10M), byte_offsets_ (38MB) 常驻
   - decodeCsrNeighbors 双路径: 内存 compact_ / CSRCache pread

## DEEP10M 测试结果

### 基本对比 (无 cgroup, T=1)

| 配置 | Recall | QPS | RSS init | RSS peak | CSR hit% |
|------|--------|-----|----------|----------|----------|
| CSR 常驻 (P2) | 95.15% | 590 | 2422 MB | 2484 MB | - |
| CSR 磁盘 256MB | 95.15% | 556 | 1869 MB | 1930 MB | 68.4% |

### cgroup 对比 (T=12)

| 配置 | Recall | QPS | RSS peak | 状态 |
|------|--------|-----|----------|------|
| P2: CSR 常驻, 2GB cgroup | 95.15% | 2340 | 1612 MB | ✅ |
| P3: CSR 磁盘, 2GB cgroup | 95.15% | 2210 | 1401 MB | ✅ RSS -211MB |
| P3: CSR 磁盘, 1.5GB cgroup | 95.15% | 1895 | 1229 MB | ✅ (P2 OOM!) |
| P3: CSR 磁盘, 1.5GB, 128MB cache | 95.15% | 62 | 1286 MB | ⚠️ cache 太小 |

### CSR 缓存大小扫描 (T=1, 无 cgroup)

| CSR_CACHE_MB | Recall | QPS | Hit Rate | Cached Pages |
|-------------|--------|-----|----------|-------------|
| 64 | 95.15% | 149 | 33.6% | 16384/141622 |
| 128 | 95.15% | 84 | 40.2% | 32768/141622 |
| 256 | 95.15% | 556 | 68.4% | 38687/141622 |
| 512 | 95.15% | 567 | 68.4% | 38687/141622 |

> 256MB 是甜点: hit rate 68.4%, 再大无收益 (working set ~151MB)
> 128MB 异常慢: LRU 淘汰开销 + 缓存颠簸

## 关键发现

1. **Recall 完全一致**: CSR 上磁盘不影响搜索结果, 95.15% 不变
2. **QPS 仅降 5.6%**: 2210 vs 2340 (T=12, 2GB cgroup)
3. **RSS 省 553MB**: compact_ 释放, byte_offsets_ (38MB) 常驻
4. **1.5GB cgroup 从 OOM 变为可用**: P2 不可行, P3 可行
5. **BFS 局部性有限**: hit rate 68.4% (非 80%+), BFS 重排在 4KB 页粒度不够集中
6. **working set ~151MB**: 256MB cache 已饱和, 更大无收益

## 100M 规模推演

| 指标 | 10M (实测) | 100M (推演) |
|------|-----------|------------|
| CSR compact | 553 MB | 4.9 GB |
| byte_offsets | 38 MB | 400 MB |
| CSR cache (256MB) | 46% coverage | 5% coverage |
| 预期 hit rate | 68.4% | ~20-30% (需更大 cache) |
| 预期 QPS 影响 | -5.6% | -30-50% (cache miss 增多) |

> 100M 规模需要 CSR_CACHE_MB=1-2GB 才能维持 60%+ hit rate
