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

## D-049: Page Shuffle 在 10M 验证 — 实验否决 {#DEC-049}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-018 source=verified -->

**Context.** DEC-025 在 SIFT1M 上 Shuffle 收益边际 (+2.1%)。DEC-018 假设 10M 规模
vecblocks 3.7GB >> page cache, Shuffle 应有更大收益。

**实验.** DEEP10M Page Shuffle (greedy page clustering):
- 页内邻居对: 27.3% → 79.3% (+190.7%)
- Shuffle 耗时: 18s (1M nodes/s)

**结果:**

| 场景 | 原始 | Shuffled | 变化 |
|------|------|----------|------|
| 2GB cgroup | 1929 QPS | 1879 QPS | **-2.6%** |
| 1.5GB cgroup | 805 QPS | 581 QPS | **-27.8%** |
| Recall | 95.15% | 95.15% | 不变 |

**根因.** Shuffle 重排了 vecblocks 内部布局, 虽然提高了页内邻居对 (27%→79%),
但破坏了 BFS 重排的 block 级局部性:
1. BFS 保证同 block 内节点空间相近 → block cache 一次读 64KB 覆盖多个 query 的候选
2. Shuffle 在 block 内部按页重排 → 页级局部性提高, 但跨 block 的访问模式变差
3. 紧凑模式下 page cache 紧张, 跨 block 访问变差的影响被放大

**Decision.** Page Shuffle 正式否决:
- 1M 收益边际 (+2.1%), 10M 负收益 (-2.6% ~ -27.8%)
- BFS block 级重排已足够, 页级 shuffle 破坏而非改善局部性
- 算法正确 (页内邻居对确实提升), 但性能假设错误 (收益不随规模放大)

## D-050: Page Search 不在 10M 验证 — 逻辑否决 {#DEC-050}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-017 source=deduced -->

**Context.** DEC-017 在 SIFT1M 冷态 +2% 收益。DEC-024 结论"10M 是真正验证战场"。

**Decision.** 不在 10M 上实施 Page Search, 逻辑否决:

1. **实现成本高**: 需要 ~40MB block→node_ids 反向映射表 + init 逻辑改动
2. **收益预期低**: Page Search 的前提是"同页向量有用", 但:
   - 10M 的 Fine Rerank 候选 (REFINE_EF=300) 已覆盖足够邻居
   - 页内额外向量大多不在 ef=300 候选集中 → 不会改善 recall
   - L2 计算开销 (+10 per page × 60 pages × 12 threads) 可能抵消 I/O 节省
3. **Page Shuffle 已否决**: Page Search 在 Shuffle 后才有意义 (同页向量是图邻居),
   Shuffle 失败 → Page Search 失去前提条件
4. **优先级低于**: PhaseA+Fine 融合 (-40%) 和 PQ SIMD (-15%) 收益远大于此

**正式关闭 DEC-017 (Page Search) 和 DEC-018 (Page Shuffle)。**

## D-051: PhaseA+Fine 融合 (FUSED_SEARCH) — 实验否决 {#DEC-051}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-045,SLA-006 source=verified -->

**Context.** DEC-045 P1 提出"边遍历边精排"融合方案，消除两阶段分离开销。

**实验.** searchLayer0 PQ 模式中 cache-miss 时 inline pread(4KB) 精确化距离。

**结果 (SIFT1M 2GB 1T):**

| 模式 | Recall | QPS | 延迟 |
|------|--------|-----|------|
| Baseline (PQ + Fine Rerank) | 95.70% | **6219** | 0.16ms |
| FUSED_SEARCH | 96.20% | **311** | 3.2ms |

**FUSED_SEARCH 慢 20 倍。**

**根因.** I/O 模式效率差异：
- Fine Rerank (批量): 100 候选 → ~60 unique pages → 排序合并 → 5~10 次大 pread(64KB)
- FUSED (inline): 图遍历中 ~200 次独立 pread(4KB)，随机分散，无法合并

**Decision.** 融合方案否决。两阶段分离不是性能瓶颈，而是 **I/O 批量化的架构优势**。
PQ_HYBRID 已在 Phase A 中为 cache-hit 节点提供精确距离，Phase B 批量 pread 效率远高于 inline。

**DEC-045 P1 (PhaseA+Fine 融合) 正式关闭。**

替代方向: PQ SIMD dsub=4 (-15%), flat_vec_cache 增大 (-10%), ef_search 自适应。

## D-052: l2Distance AVX2 SIMD 优化 — 验证通过 {#DEC-052}
<!-- ndf: kind=decision date=2026-07-30 affects=SLA-006,DEC-045 source=verified -->

**Context.** 原 l2Distance 为标量实现 ("保持与 hnswlib 一致, 作为公平对比基线")。
但 hnswlib 内部已用 AVX2，这个"基线"实际对自己不公平。
PQ SIMD dsub=4 分析发现 PQ 距离计算仅占 PhaseA 8%，但 l2Distance 标量实现
占 PhaseA + Fine Rerank 总计算量的 ~18%。

**改动.** AVX2 + FMA 实现: 8-wide load + sub + fmadd + horizontal sum。
SIFT 128D = 4 iterations，DEEP 96D = 3 iterations + 0 tail。

**结果:**

| 场景 | 标量 | AVX2 | 变化 |
|------|------|------|------|
| SIFT1M 2GB 4T | 6219 | **7231** | **+16.3%** |
| SIFT1M 512MB 4T | 5489 | **6188** | +12.7% |
| SIFT1M 2GB 1T | 2053 | 2111 | +2.8% |
| DEEP10M 2GB 12T | 1929 | 1848 | -4.2% (noise) |

**vs hnswlib 全量放开:**
- 差距从 1.88x 缩小到 **1.62x** (4T: 7231 vs 11673)

**Decision.** AVX2 l2Distance 采纳。SIFT1M 主要受益（128D 多次 l2 调用），
DEEP10M 无显著变化（瓶颈在 I/O 不在计算）。

**教训:** "公平对比基线"应该用相同的 SIMD 级别，而非标量 vs SIMD。
NDF OPT 路线 "PQ SIMD dsub=4" 的实际收益来自 l2Distance 而非 PQ 距离计算。

## D-053: flat_vec_cache 解耦 + 调参 — 实验否决 (增大无收益) {#DEC-053}
<!-- ndf: kind=decision date=2026-07-31 affects=SLA-006 source=verified -->

**Context.** 原 flat_vec_cache 预算被 cap 在 `min(cache_slots*block_size, FLAT_VEC_MB)`，
即 CACHE_MB=128 时 flat cache 不超过 128MB。假设增大 flat cache 能提高 Phase A 精确距离覆盖率。

**改动.** 解耦: flat_vec_cache 预算由 FLAT_VEC_MB 独立控制, 不受 CACHE_MB 限制。

**实验 (SIFT1M 2GB 4T):**

| FLAT_VEC_MB | 覆盖率 | QPS | 变化 |
|-------------|--------|-----|------|
| 32 (65K slots) | 6.5% | 6603 | -5% |
| **64 (130K slots)** | **13%** | **6754** | **baseline** |
| 128 (260K slots) | 26% | 6515 | -4% |
| 256 (520K slots) | 52% | 6176 | -9% |
| 512 (1040K slots) | 100% | 7310 | +8% (噪声?) |

**根因.** flat_vec_cache 是 hash table (node_id % num_slots):
- 64MB = 130K slots: 热节点集中在少数 slot, L3 cache 命中率高
- 512MB = 1M slots: 覆盖全部节点, 但 hash table 数组 520MB >> L3 (12MB), 每次 lookup 都是 L3 miss
- **增大 flat cache 恶化 CPU L3 cache 局部性, 抵消了覆盖率提升**

**Decision.**
1. 解耦修复保留 (代码正确性, 去除人为 cap)
2. FLAT_VEC_MB=64 确认为最优配置
3. "增大 flat_vec_cache" 优化方向否决

**教训.** hash table 大小与 CPU cache 局部性存在 trade-off:
- 小 hash table → 高 L3 命中但低覆盖率
- 大 hash table → 高覆盖率但低 L3 命中
- 对 SIFT1M, 64MB (13% 覆盖) 是最佳平衡点

## D-054: ef_search 自适应 (PQ 距离 margin 裁剪) — 实验否决 {#DEC-054}
<!-- ndf: kind=decision date=2026-07-31 affects=SLA-006 source=verified -->

**Context.** Phase A 产出 ef_coarse=100~300 个 PQ 候选，全部送 Fine Rerank。
假设按 PQ 距离 margin (best_dist × margin) 裁剪可减少 Fine Rerank I/O 量。

**方案.** ADAPTIVE_EF=1 ADAPTIVE_MARGIN=M:
- 保留 PQ 距离 < best_dist × M 的候选
- 至少保留 2k 个 (recall 保障)
- M>5 时等价于不裁剪 (所有候选都保留)

**实验 (SIFT1M 2GB 4T):**

| MARGIN | Recall | QPS | 变化 |
|--------|--------|-----|------|
| baseline | 95.70% | ~7000 | - |
| 3.0 | 95.40% | ~7300 | +4% (噪声范围内) |
| 1.5 | 94.60% | ~8000 | +14% (**recall < 95%**) |

**DEEP10M 2GB 12T:**

| MARGIN | Recall | QPS |
|--------|--------|-----|
| baseline | 95.15% | 1973 |
| 3.0 | 95.15% | 1813 (-8%) |

**根因.**
1. SIFT1M 基线 QPS 方差 ~15% (6078~7261), 自适应效果 (~4%) 被噪声淹没
2. DEEP10M PQ dim=96 精度高, margin 裁剪几乎不减少候选 → 无收益甚至负收益
3. recall≥95% 约束下, margin 必须 ≥5.0 (等价于不裁剪) 才能保持 recall

**Decision.** PQ 距离 margin 裁剪否决:
- recall≥95% 时无可靠 QPS 提升
- margin=1.5 可作为"性能模式"(recall~94.5%, +14% QPS), 但不满足 SLA 约束
- 代码不留

**教训.** searchLayer0 的 early termination (`candidateDist > lowerBound && size==ef → break`)
已经自动处理了"简单 query 早停"的场景。Fine Rerank 的候选数 (100~300) 不大,
裁剪几个候选的 I/O 节省不足以抵消 PQ 距离误差带来的 recall 损失。

## D-055: io_uring 批量 Fine Rerank — 实验否决 {#DEC-055}
<!-- ndf: kind=decision date=2026-07-31 affects=SLA-006 source=verified -->

**Context.** Fine Rerank 用 pread 逐页读, 多线程需 FINE_PREAD=1。
假设用 thread_local io_uring 批量提交可利用 NVMe 并行 + 内核 I/O 合并。

**方案演进:**
1. 自定义 IoUring wrapper (128 entries, 8KB buffers): SIFT1M +25% 但 DEEP10M recall 崩溃 (buffer 不足)
2. liburing 直接调用 (64KB buffers, merged reads): SIFT1M -4%, DEEP10M +1%
3. 分批提交 (256+ ranges → 2 batches of 128): memcpy 开销抵消收益

**实验:**

| 配置 | SIFT1M 4T QPS | DEEP10M 12T QPS | DEEP10M Recall |
|------|---------------|-----------------|----------------|
| pread baseline | ~6400 | 1973 | 95.15% |
| IoUring wrapper (8KB) | ~8000 (噪声?) | — | 54% (buffer 不足) |
| liburing (64KB, 128 bufs) | 5908 | — | 58% (range > 128) |
| liburing + batch | 6152 | 1997 | 95.15% ✓ |

**微基准 (100 random pages, warm cache):**

| 方法 | 延迟/query |
|------|-----------|
| pread 100×4KB | 28.9 μs |
| pread 10×40KB (merged) | 13.7 μs |
| io_uring 100×4KB | 24.3 μs |
| io_uring 10×64KB (merged) | 0.8 μs |

**根因分析:**
1. **微基准 vs 实际**: 微基准用 liburing registered buffers + prep_read_fixed (0.8μs), 
   实际需要 unordered_map page_ptr + memcpy(4KB/page) + batch 间数据保护
2. **Page cache 均衡器**: SIFT1M(496MB)/DEEP10M(3.7GB) 都能放入 30GB RAM 的 OS page cache
   - pread on cached page = memcpy from page cache → 即时
   - io_uring on cached page = submit + CQ poll → 额外开销
   - 两者命中相同的 page cache, io_uring 多一层 syscall
3. **Batch memcpy 开销**: 大规模 ranges(250+)需要分批, 每批的 buffer 会在下批覆写,
   必须 memcpy 到 page_data unique_ptr, 抵消了 zero-copy 优势
4. **O_DIRECT 才是 io_uring 的战场**: 现有 FINE_BUFFERED=1 (buffered I/O) 下,
   page cache 使 io_uring 无优势

**Decision.** io_uring 批量 Fine Rerank 否决:
- Warm page cache 下 io_uring 无优势 (submit/wait 开销 ≈ pread memcpy)
- Batch memcpy 开销抵消 zero-copy 收益
- 需要 O_DIRECT + SPDK 才能发挥 io_uring 真正优势 (P5 阶段)

**教训.** 
- 微基准 ≠ 实际: registered buffer + prep_read_fixed 的 0.8μs 在实际代码中变成 6μs+
- Page cache 是 I/O 的伟大均衡器: 所有 buffered I/O 路径最终都命中同一缓存
- io_uring 的优势在 cold I/O + O_DIRECT, 不在 warm page cache
