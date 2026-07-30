# P4 优化路线图 (2026-07-31 最终版)

## 当前状态

### SIFT1M 4T (2GB cgroup)
- **Recall**: 95.70% ✅
- **QPS**: 7067 (hnswlib 11673, gap **1.65x**, 达到 hnswlib 60%)
- **RSS**: 291MB

### DEEP10M 12T (2GB cgroup)
- **Recall**: 95.15% ✅
- **QPS**: 1973 (hnswlib OOM@2GB, 无基线)
- **RSS**: 1441MB

### 有效代码改动 (P4)
| 改动 | DEC | 收益 | 状态 |
|------|-----|------|------|
| P1 批量 pread 合并 | DEC-047 | +9~146% | ✅ 保留 |
| AVX2 l2Distance FMA | DEC-052 | +16% | ✅ 保留 |

### 否决实验 (P4)
| 实验 | DEC | 结果 | 根因 |
|------|-----|------|------|
| PhaseA+Fine 融合 | DEC-051 | ❌ -20% | inline I/O 20x 慢于 batch |
| flat_vec_cache 解耦 | DEC-053 | ❌ 0% | L3 局部性恶化抵消覆盖率提升 |
| ef_search 自适应 | DEC-054 | ❌ 0% | PQ margin 裁剪在 recall≥95% 无效 |
| io_uring Fine Rerank | DEC-055 | ❌ 0% | page cache 均衡器效应 |

## 关键技术认知

### Page Cache 均衡器效应 (DEC-055 核心发现)
- 30GB RAM → 所有 vecblocks 在 OS page cache 中
- buffered I/O 下 pread 和 io_uring 命中同一缓存
- 微基准: io_uring merged reads 0.8μs vs pread 13.7μs (理论 17x)
- 实际: io_uring submit/wait 开销 ≈ pread memcpy, 无净收益
- **结论**: io_uring/O_DIRECT 优势仅在 page cache miss 时体现

### L3 Cache 局部性 (DEC-053 发现)
- flat_vec_cache hash table: 64MB 最优 (130K slots, 13% 覆盖)
- 增大到 256MB: QPS -9% (hash table >> L3 cache, lookup 变慢)
- 小 hash table → 高 L3 命中但低覆盖率; 大 hash table → 反之
- SIFT1M 最佳平衡点: 64MB

### PQ Margin 自适应失效 (DEC-054 发现)
- searchLayer0 的 early termination 已隐式处理简单 query 早停
- Fine Rerank 候选数 (100~300) 不大, 裁剪几个的 I/O 节省不足
- recall≥95% 约束下 margin 必须 ≥5.0 (等价于不裁剪)

## 下一步方向

### P3: 100M 规模验证 (推荐下一步)
- page cache 不再 cover 全部数据 → I/O 优化有意义
- CSR 内存成为新瓶颈 (4.7GB varint)
- 需要分级存储策略

### P5: 硬件路线 (长期)
- **O_DIRECT + io_uring**: 绕过 page cache, 释放 io_uring 潜力
  - DEC-055 微基准已证明理论 17x 优势
  - 需要 buffer 对齐 + I/O 合并策略调整
- **SPDK**: 用户态 NVMe 驱动, 零拷贝 I/O
- **GPU**: PQ distance 批量计算 offload
- **PMEM**: 持久内存作为分级存储层
