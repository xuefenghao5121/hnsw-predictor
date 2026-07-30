# P3 关键认知修正: cgroup 诚实性与真实部署模型

> 日期: 2026-07-30
> 触发: 项目目标对齐讨论, 5 个架构疑问分析
> 影响: 所有之前报告的性能数字需要重新验证

---

## D-039: cgroup page cache 诚实性修正 {#DEC-039}
<!-- ndf: kind=decision date=2026-07-30 affects=CHR-003,CHR-004,DEC-037,VER-037 source=deduced -->

**Context.** 之前的所有 benchmark 在 30GB RAM 的开发机上运行, cgroup 限制了进程匿名内存,
但系统 page cache 白嫖了 8GB+ 数据文件 (graph.bin 4.6GB + vecblocks 3.7GB)。
报告的 "RSS 1612MB / QPS 2340" 不反映真实受限内存部署。

**根本问题:**

1. **预热缓存白嫖**: 前一次 benchmark 运行缓存了全部数据文件, 后续运行在 cgroup 内
   访问时, 这些 page cache 页面可能不被计入 cgroup (首次读入发生在 cgroup 外)
2. **系统 RAM 充裕**: 即使 cgroup 限制 2GB, 系统 30GB RAM 提供充足的 page cache,
   内核不会积极回收。进程能 "免费" 访问 8GB 缓存数据
3. **真实 2GB RAM 服务器**: graph.bin (4.6GB) 和 vecblocks (3.7GB) 不可能全放 page cache,
   大量访问会触发真实磁盘 I/O

**Decision.**

1. **所有 benchmark MUST 在 `echo 3 > /proc/sys/vm/drop_caches` 后运行**
   - 确保无预热缓存, 模拟真实冷启动
2. **初始化文件 (graph.bin, PQ codes, bfs.bin) 加载后 MUST 调用 `posix_fadvise(DONTNEED)`**
   - 这些文件加载后不再需要, 释放 page cache 额度给运行时热数据
3. **cgroup MemoryMax 限制的是总内存 (匿名 + page cache)**
   - cgroup v2 已支持, 但需要干净启动才能诚实记账
4. **运行时 page cache 预算 = MemoryMax - 匿名内存**
   - 例: 2GB cgroup, 匿名 1.5GB -> page cache 预算 500MB
   - vecblocks 3.7GB 中只有 500MB (13%) 能被缓存, 其余走磁盘

**影响范围:**
- CHR-003 (SIFT1M SLA): QPS 数字可能下降, 需重新验证
- CHR-004 (演进路线): P2/P3 的 cgroup 目标需重新评估
- DEC-037 (cgroup 目标校准): 之前的 cgroup 可行性结论需修正
- VER-037 (P2 验证): 所有 "✅" 标记需重新确认

> rationale: 之前报告的 "2GB cgroup 下 QPS 2340" 实际使用了 8GB+ 的系统 page cache。
> 在真实 2GB RAM 部署中, QPS 可能降到 ~100-300。这不是优化退步, 而是之前测量不诚实。
> 修正后才能知道 DiskHNSW 在真实受限内存下的实际性能。

## D-040: 真实 I/O 模型分析 {#DEC-040}
<!-- ndf: kind=info level=may layer=L2 status=stable since=0.5 source=deduced -->

**Context.** 问题 3 的分析结论。需要区分 "开发机 I/O" 和 "真实部署 I/O"。

**开发机 (30GB RAM) I/O 模型:**
- vecblocks 3.7GB 完全在系统 page cache -> pread 命中内存, 不走磁盘
- CSR 文件 553MB 也在 page cache -> CSRCache miss 也是内存读
- PROFILE_TS: pread 602μs/query = 系统调用 + 内存拷贝, 不是磁盘 I/O
- 磁盘 I/O ≈ 0, 性能完全由 PQ 计算 (80%) 决定

**真实 2GB RAM 部署 I/O 模型:**

| 组件 | 大小 | page cache 预算 | 预期命中率 | miss I/O 延迟 |
|------|------|----------------|-----------|-------------|
| vecblocks | 3.7GB | ~500MB (13%) | 10-30% | 50μs/4KB (NVMe) |
| CSR pages | 553MB | 共享 500MB | 30-50% | 50μs/4KB |
| graph.bin | 4.6GB | 0 (加载后驱逐) | N/A | N/A |
| PQ codes | 304MB | 0 (在匿名内存) | N/A | N/A |

**每查询 I/O 估算 (2GB cgroup, 冷启动):**
- Fine Rerank: 300 候选 × 4KB × 70% miss × 50μs = 10.5ms
- CSR: 612 reads × 50% miss × 50μs = 15.3ms
- 总 I/O: ~26ms/query -> QPS ~38 (1T, 纯冷态)
- vs 开发机 QPS 590: **退化 15x**

**关键结论:**
1. 开发机上 I/O 不是瓶颈 (7%), 但真实部署中 I/O 是主要瓶颈 (>70%)
2. PQ 计算 (80% in 开发机) 在真实部署中占比降到 ~20%
3. **之前的优化 (VisitedList, PQ SIMD) 在真实部署中收益缩水** -- 它们优化的是非瓶颈
4. **真实部署需要的优化方向: I/O 减少 (批处理去重) + I/O 掩盖 (预取) + 缓存策略**

## D-041: 优化有效性的逻辑修正 {#DEC-041}
<!-- ndf: kind=info level=may layer=L1 status=stable since=0.5 source=deduced -->
<!-- refines=DEC-008,DEC-014 -->

**Context.** 问题 1 的重新回答。之前说 "内存节省 3.7x" 不诚实。

**真实的内存节省来源:**

| 组件 | 全内存方案 | DiskHNSW | 节省方式 | 真实性 |
|------|-----------|----------|---------|--------|
| 向量数据 | 3.83GB (常驻) | 304MB PQ codes (常驻) | PQ 压缩 12.6x | ✅ 真实 |
| 图邻接表 | 834MB (常驻) | 591MB CSR varint (常驻或磁盘) | 压缩 1.4x | ✅ 真实 |
| 上层向量 | ~1GB (常驻) | 228MB (常驻) | 分层卸载 | ✅ 真实 |
| 精排向量 | 0 (已在内存) | 3.7GB on disk + page cache | 按需 I/O | ⚠️ 部分真实 |
| **总计常驻** | ~5.7GB | ~1.1GB | **5.2x** | ✅ |
| **总计物理 (含 cache)** | ~5.7GB | 1.1GB + page cache | 取决于部署 | ⚠️ |

**修正:**
- "内存节省 3.7x" 应改为 "常驻匿名内存节省 5.2x, 但运行时还需要 page cache 预算"
- 在真实 2GB RAM 部署中, page cache 预算有限, 部分 vecblocks 访问走磁盘
- **优化有效的核心证明不变**: PQ 压缩 + 图常驻 + 按需精排, recall 95% 可达
- **但 QPS 在真实部署中会显著降低**, 需要重新测量

## D-042: 冷启动退化模型 {#DEC-042}
<!-- ndf: kind=info level=may layer=L2 status=stable since=0.5 source=deduced -->

**Context.** 问题 5 的分析。查询频繁变化时性能退化。

**退化分级模型 (2GB cgroup, drop_caches 后):**

| 查询模式 | CSR hit% | vecblocks hit% | I/O/query | QPS (1T) 估算 |
|----------|---------|---------------|-----------|-------------|
| 热态 (查询集中, 工作集 <500MB) | 68% | 80%+ | ~3ms | ~300 |
| 温态 (查询适度变化) | 40% | 40% | ~15ms | ~100 |
| 冷态 (查询分散, 覆盖全图) | 20% | 10% | ~26ms | ~38 |

**退化根因:**
1. CSR cache (256MB 进程内) 覆盖 46%, 查询分散时大量 miss -> 磁盘
2. vecblocks page cache 受 cgroup 限制 (~500MB), 覆盖 13%, 查询分散时几乎全 miss
3. PQ codes (304MB) 随机访问, 在 L3 (30MB) 外, 但在 RAM 内 (无需磁盘)

**与开发机的差异:**
- 开发机 (30GB RAM): 冷热差异 <1% (QPS 583 vs 580), 系统立即重新缓存
- 真实 2GB: 冷热差异可达 8x (QPS 300 vs 38), page cache 无法覆盖全部数据

**缓解策略 (按优先级):**
1. **增大 CSR cache**: 256MB -> 512MB, hit rate 46% -> 92% (代价: 256MB 匿名内存)
2. **查询聚类批处理**: 相似 query 分组, 利用 CSR/vecblocks 局部性
3. **预热高频区域**: 启动时预加载高频查询的 CSR pages 和 vecblocks pages
4. **自适应缓存**: 监控 hit rate, 动态调整 CSR cache vs vecblocks page cache 预算

## Q-006: 批处理在真实部署中的优先级 {#Q-006}
<!-- ndf: kind=open blocks=P3-ARCH-003 source=deduced date=2026-07-30 -->

**问题.** 问题 4 的分析。批处理在开发机上收益小 (I/O 非瓶颈), 但在真实部署中可能是关键优化。

**当前状态:**
- `batchSearchConcurrent`: 多线程独立查询, 无 I/O 去重
- `batchSearchEventDriven`: 查询间切换, 仅优化 I/O 利用率
- 无跨查询 page 去重, 无批量 PQ LUT, 无批量 Fine Rerank

**真实部署的批处理价值估算:**
- 200 query batch, 每 query 300 候选, 总 60000 page 访问
- 跨查询 page 去重: 60000 -> ~15000 unique pages (4x 去重)
- 批量 pread: NVMe QD32 并行, 15000 页 / 32 = 469 轮 × 50μs = 23ms
- vs 逐查询: 200 × 26ms = 5200ms
- **批处理吞吐: 200/0.023s = 8700 QPS vs 逐查询 38 QPS (228x)**

> 注: 228x 是理论上限, 实际受 PQ 计算和内存带宽限制。但即使 10x 提升也有巨大价值。

**决议条件:**
- 诚实 benchmark 后, 如果 1T QPS <100, 批处理优先级提升为 🔴
- 如果 1T QPS >300, 批处理优先级为 🟡 (优化而非必需)

## Q-007: 真实部署的 SLA 重新定义 {#Q-007}
<!-- ndf: kind=open blocks=CHR-003,CHR-004 source=deduced date=2026-07-30 -->

**问题.** 之前的 SLA (recall≥95%, QPS>500) 是在不诚实测量下定义的。
诚实测量后, SLA 可能需要调整。

**待定:**
- 2GB cgroup (drop_caches) 下 QPS 是多少? (估算 30-300)
- 如果 QPS <100, SLA 应该调到多少? 或换目标 (如批处理 QPS)?
- cgroup 目标是否应该调整? (2GB -> 4GB? 或 1GB?)
- 对比 hnswlib 在相同条件下的真实性能 (hnswlib 也需要 page cache)

**决议条件:** 诚实 benchmark 完成后, 基于实测数据重新定义 SLA。

---

## D-043: hnswlib 全量放开对比 — 两阶段搜索的固有开销 {#DEC-043}
<!-- ndf: kind=decision date=2026-07-30 affects=CHR-003,CHR-004,SLA-006 source=verified -->

**Context.** 之前只对比了"内存受限时谁能动"。新增全量放开 (2GB cgroup) 下与 hnswlib 的直接性能对比。

**实测数据 (SIFT1M, 2GB cgroup, drop_caches):**

| 配置 | hnswlib | DiskHNSW | 差距 |
|------|---------|----------|------|
| 1T ef=50 | 0.09ms QPS 11565 | 0.40ms QPS 2492 | **4.6x** |
| 4T ef=50 | QPS 11673 | QPS 5693 | 2.1x |
| hnswlib ef=100 | 0.16ms QPS 6392 (recall 98.3%) | - | 不可比 |

**DiskHNSW 0.40ms 时间分解 (PROFILE_TS + PROFILE_FINE):**

```
PhaseA (PQ 粗筛):    230μs (57%)  ← hnswlib 无此步骤
Fine Rerank:        190μs (38%)  ← hnswlib 无此步骤
  pread I/O:         189μs (page cache 命中, 非磁盘)
  L2 compute:         41μs
其他:                ~20μs (5%)
```

**Decision.**

1. **根因确认**: 4.6x 差距来自两阶段搜索的固有架构开销 — PQ 粗筛 + Fine Rerank 做了两遍距离计算, hnswlib 在一个 pass 里完成
2. **不是 I/O 问题**: 全量放开下 Fine Rerank 的 pread 全命中 page cache (189μs 是系统调用开销, 非磁盘)
3. **不是 PQ 精度问题**: recall 95.70% vs hnswlib 95.25%, PQ 粗筛精度足够
4. **是架构问题**: 两阶段分离导致重复遍历和额外计算

**影响:** SLA-006 新增全量放开目标, 要求优化到 hnswlib 70%+ QPS。

## D-044: 双维度 SLA — 内存受限 + 全量放开 {#DEC-044}
<!-- ndf: kind=decision date=2026-07-30 affects=CHR-003,CHR-004 source=verified -->

**Context.** 之前 SLA 只考虑内存受限场景。用户明确要求: "目标性能场景不止是 cgroup 限制下的 hnswlib, 还有全量放开下的 hnswlib"。

**Decision.** DiskHNSW 的 SLA MUST 同时满足两个维度:

### 维度 1: 内存受限 (已验证 ✅)

在 cgroup 限制 < hnswlib 需求时, DiskHNSW MUST 显著优于 hnswlib:

| 场景 | cgroup | DiskHNSW | hnswlib | 胜出 |
|------|--------|----------|---------|------|
| SIFT1M | 512MB | QPS 5247 | QPS 527 | **10x** |
| DEEP10M | 2GB | QPS 1762 | OOM | ∞ |
| DEEP10M 紧凑 | 1.5GB | QPS 327 | OOM | ∞ |

### 维度 2: 全量放开 (待优化 🔴)

在内存充裕时, DiskHNSW SHOULD 达到 hnswlib 70%+ QPS:

| 场景 | 当前 | 目标 | 差距 |
|------|------|------|------|
| SIFT1M 1T | 2492 (hnswlib 21%) | ≥ 8000 (70%+) | 需 3.2x |
| SIFT1M 4T | 5693 (hnswlib 49%) | ≥ 8000 (70%+) | 需 1.4x |

**rationale:** DiskHNSW 不能是"退而求其次"的方案。如果内存充裕时慢 5x,
用户没有理由选择它。70% 是"可接受的性能折衷"阈值 — 用 2x 内存节省换 30% 性能。

## D-045: 两阶段融合优化方向 {#DEC-045}
<!-- ndf: kind=decision date=2026-07-30 affects=SLA-006,Q-006 source=deduced -->

**Context.** DEC-043 确认 4.6x 差距来自两阶段分离。需要规划如何缩小差距。

**Decision.** 按以下优先级推进优化:

### 🔴 P1: PhaseA + FineRerank 融合 (预期 -40%)
**思路**: 边遍历边精排, 共享 candidate set, 消除重复遍历
```
当前: PhaseA(遍历100节点PQ) → 输出100候选 → FineRerank(100候选精确L2)
融合: 遍历中 → cache命中用精确L2 / miss用PQ → 到达ef时已精排完毕
```
本质: PQ_HYBRID 模式的极致版 — 尽可能用精确距离, miss 时 fallback PQ

### 🟡 P2: PQ SIMD 优化 (预期 -15%)
SIFT1M dsub=4 可用 AVX2 LUT, 当前实现未充分利用

### 🟡 P3: Fine Rerank 批量 pread (预期 -20%)
合并相邻 4KB 页为单次 8-16KB pread, 减少系统调用

### 🟢 P4: flat_vec_cache 增大 (预期 -10%)
32MB → 64MB, 更多候选命中缓存

### 合计预期: 0.40ms → 0.20ms, QPS 2492 → 5000+

**alternatives rejected:**
- 纯 O_DIRECT (绕过 page cache): 全量放开下 I/O 非瓶颈, 无收益
- 增大 PQ 码本 (M=64): 内存翻倍, 收益不确定
- 放弃 PQ 粗筛全量加载: 违背内存受限设计目标

## D-046: CSR cache 增大 — 实验否决 {#DEC-046}
<!-- ndf: kind=decision date=2026-07-30 affects=OPT-002 source=verified -->

**Context.** 猜想 CSR cache 256MB (覆盖率 46%) 增大到 512MB (覆盖率 92%) 能提高 hit rate。

**实验.** DEEP10M 2GB cgroup, CSR_CACHE_MB=512:
- hit_rate: 68.4% (与 256MB 完全相同)
- QPS: 1816 (vs 1762, 仅 +3%)
- misses: 38687 (与 256MB 完全相同)

**根因.** 200 query 的 unique CSR 工作集 = 38687 pages (151MB), 已在 256MB cache 内。
剩余 31.6% miss 是每个 query 访问新区域的**固有 cold miss**, 增大 cache 无法解决。

**Decision.** CSR cache 256MB 是最优配置, 不增大。
如要提高 CSR hit rate, 需改变访问模式 (查询聚类), 而非增大缓存。

## D-047: P1 批量 pread 合并 — 实验证实 {#DEC-047}
<!-- ndf: kind=decision date=2026-07-30 affects=OPT-001 source=verified -->

**实验.** Fine Rerank 中 60 个独立 pread(4KB) → 排序合并连续页为 ~10 个 pread(64KB)。

**结果:**

| 场景 | 优化前 | P1 后 | 提升 |
|------|--------|-------|------|
| SIFT1M 512MB 4T | 5247 | 6756 | +29% |
| DEEP10M 2GB 12T | 1762 | 1929 | +9% |
| DEEP10M 1.5GB 12T | 327 | 805 | +146% |

**Decision.** P1 保留, 默认启用 (FINE_PREAD=1 时自动生效)。

## D-048: P2 query 重排序 — 实验否决 {#DEC-048}
<!-- ndf: kind=decision date=2026-07-30 affects=OPT-003 source=verified -->

**实验.** 按 PQ top-1 节点 ID 排序 query, BFS 重排保证 ID 相近=空间相近。

**结果:**
- CSR hit_rate: 68.4% → 68.4% (完全不变)
- DEEP10M 1.5GB QPS: 805 → 625 (反而降低)
- 预计算开销: 10M 节点 ADC 扫描 × 200 query ~数十秒

**根因.** 200 query 的工作集 (38687 unique CSR pages = 151MB) 已经在 cache 内。
排序不会减少 cold miss — 每个 query 仍然要访问自己的新区域。
预计算开销远超缓存收益。

**Decision.** P2 否决, 保留代码 (QUERY_SORT=1) 但默认关闭。
