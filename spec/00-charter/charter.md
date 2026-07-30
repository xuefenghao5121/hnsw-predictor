# Charter - 系统目标与范围

## 系统目标 {#CHR-001}
<!-- ndf: kind=arch level=L0 layer=L0 status=stable since=0.1 source=observed -->

DiskHNSW MUST 在 cgroup 内存限额(≥512MB)下,使用磁盘驻留向量数据,实现与全内存 HNSW 可比的向量搜索召回率(≥95%),同时将常驻内存控制在限额内。

**核心业务实体(从代码强行归纳):**

1. **HNSW 图结构** (`GraphStructure` - `common.h:113-136`): 分层图,包含节点层级、向量、标签、邻接表,搜索时通过贪心下降 + best-first 遍历
2. **BlockCache** (`BlockCache` - `block_cache.h:116-430`): LRU 缓存的磁盘块管理器,按需加载 64KB/256KB 块,可插拔布局和替换策略
3. **PQ 编码** (`PQParams` + `pq_codes_` - `disk_hnsw.h:37-43`): Product Quantization 将 128 维向量压缩为 32 字节,用于 Phase A 零 I/O 粗筛
4. **两层搜索**: Phase A (PQ ADC 粗筛) + Phase B (精确 L2 精排),精排通过 4KB 页粒度 I/O 读取候选向量
5. **数据 Pipeline**: build_index → extract_graph → bfs_reorder → write_blocks → write_blocks_veconly → train_pq → gen_gt
6. **io_uring 异步 I/O** (`IoUring` - `io_uring_wrapper.h:57-363`): 内核异步 I/O,批量提交,O_DIRECT 对齐

## Scope {#CHR-002}
<!-- ndf: kind=arch level=L0 layer=L0 status=stable since=0.1 source=observed -->

- **IN**: 向量搜索(L2 距离)、PQ 训练与编码、BFS 重排优化块局部性、CSR 邻接表压缩、图引导预取、多线程并发搜索
- **IN**: cgroup 内存受限部署、数据 pipeline 工具链(build_index/extract_graph/bfs_reorder/write_blocks/write_blocks_veconly/gen_route/verify)
- **IN**: 可插拔缓存架构(LayoutProvider 接口 + ReplacementPolicy 接口)
- **OUT**: 增量插入/删除(当前只读搜索,无 insert/delete API)
- **OUT**: 多租户 QoS、分布式部署、持久内存支持
- **OUT**: GPU 加速(规划在 P5 阶段)

## Non-Goals {#CHR-003}
<!-- ndf: kind=info layer=L0 status=stable since=0.1 source=observed -->

- **不是**通用向量数据库:无 CRUD、无持久化日志、无 WAL
- **不是**实时插入系统:索引构建是离线 batch 操作
- **不是**跨平台方案:依赖 Linux 5.1+ (io_uring)、x86 AVX2、C++17

---

<!-- ════════════════════════════════════════════════════════════════ -->
<!-- INTENT- 条款:基于 README、代码结构和性能数据的推断性规范   -->
<!-- source=deduced 表示条款源自全局理解,非从单一代码实体提取 -->
<!-- ════════════════════════════════════════════════════════════════ -->
<!-- INTENT-CHR-001 和 INTENT-CHR-002 已删除（裁决：文档意淫，无部署/用户数据支撑） -->
<!-- ════════════════════════════════════════════════════════════════ -->

## 关键性能承诺 {#CHR-003}
<!-- ndf: kind=constraint level=must layer=L0 status=stable since=0.5 source=verified -->
<!-- verified=2026-07-30: drop_caches + cgroup v2 诚实测量 -->
<!-- see: spec/40-constraints/sla-redefinition.md -->

> 完整 SLA 定义 (含 hnswlib 对比、cgroup 扫描、分级目标): `spec/40-constraints/sla-redefinition.md`

### SIFT1M (512MB cgroup)

| 指标 | 目标 | 实测 (诚实) | 状态 |
|------|------|-----------|------|
| Recall@10 | ≥ 95% | 95.70% | ✅ |
| QPS (4T) | ≥ 5000 | 5247 | ✅ |
| QPS (1T) | ≥ 2000 | 2334 | ✅ |
| RSS | ≤ 300MB | 275MB | ✅ |
| vs hnswlib | ≥ 2x QPS | **10x** (@512MB) | ✅ |

### DEEP10M (2GB cgroup)

| 指标 | 目标 | 实测 (诚实) | 状态 |
|------|------|-----------|------|
| Recall@10 | ≥ 95% | 95.15% | ✅ |
| QPS (12T) | ≥ 1000 | 1762 | ✅ |
| RSS | ≤ 1.5GB | 1401MB | ✅ |
| hnswlib | 不可行 | OOM@2GB (需 6GB) | ✅ |

### DEEP10M 紧凑模式 (1.5GB cgroup)

| 指标 | 目标 | 实测 (诚实) | 状态 |
|------|------|-----------|------|
| Recall@10 | ≥ 95% | 95.15% | ✅ |
| QPS (12T) | ≥ 300 | 327 | ✅ |
| hnswlib | 不可行 | OOM | ✅ |

> rationale: 95% recall 是生产可接受的最低召回率阈值;
> 2000 QPS 是单线程交互式搜索的可用阈值(<0.5ms 延迟)。

## 演进路线意图（探索性设想） {#CHR-004}
<!-- ndf: kind=arch level=may layer=L0 status=exploratory since=0.2 source=deduced -->

> ⚠️ **探索性设想标签**：本条款无设计文档或代码支撑，仅为方向性思考。
> P0-P2 已完成并验证；P3-P5 为未落地的规划构想，不构成规范性承诺。

DiskHNSW 的设计意图是**从 1M 验证走向 100M 生产**：

- **P0-P1（已完成 ✅）**：1M 规模下验证内存卸载 + 压缩 + 图裁剪的可行性与边界
- **P2（已完成 ✅, 2026-07-30）**：10M 规模验证。DEEP10M 95.15% recall / **1762 QPS** (12T, drop_caches 诚实测量) / 2GB cgroup。
  hnswlib 需 ~6GB OOM@2GB，DiskHNSW 在 hnswlib 无法运行的内存配置下仍可用。
  ⚠️ DEC-039: 之前 QPS 2340 不诚实（page cache 白嫖），诚实测量 1762 (-25%)。
  1.5GB cgroup 下从 1895 降到 327 (-83%)，~1.5GB 是性能悬崖。
  瓶颈从 PQ 计算转移到磁盘 I/O（真实部署），VisitedList 优化在开发机有效但真实部署收益缩水。
- **P3（实现中, 2026-07-30）**：CSR 分页加载已实现。
  CSR 上磁盘 + CSRCache LRU 分页缓存, 节省 553MB 匿名内存 (DEEP10M)。
  初始化后 posix_fadvise(DONTNEED) 驱逐 graph/PQ/bfs 文件页。
  诚实 benchmark (drop_caches) 已验证 SIFT1M 和 DEEP10M。
  详见 `spec/open/validation-p3-honest-benchmark.md` 和 `spec/open/memory-architecture.md`。
- **P4-P5（探索性构想）**：分级存储、硬件亲和（NUMA/SPDK/GPU/PMEM）。这些方向当前
  无代码或设计支撑，仅为探索性路线设想

每个阶段的核心验证：**"给定内存预算 M，DiskHNSW 能跑多大规模的向量搜索？"**

**双维度目标**：
1. **内存受限维度**：给定 cgroup 限额, DiskHNSW MUST 显著优于 hnswlib (当前已验证 ✅)
2. **全量放开维度**：内存充裕时, DiskHNSW SHOULD 达到 hnswlib 70%+ QPS (当前 21%, 需优化)
   - 差距根因: 两阶段搜索的额外开销 (PQ 粗筛 57% + Fine Rerank 38%)
   - 优化路径: PhaseA+Fine 融合 / PQ SIMD / 批量 pread / flat_vec_cache 增大
   - 详见 `spec/40-constraints/sla-redefinition.md` SLA-006

> rationale: 1M 规模下宿主机 page cache 能装下全部 496MB 向量数据，
> 掩盖了磁盘 I/O 优化的真实价值。10M 规模验证了瓶颈转移 (I/O -> PQ 计算)。
> 100M 规模 CSR 内存将成为新瓶颈，需要新的架构决策。
> 全量放开场景要求 DiskHNSW 不是"退而求其次"的方案, 而是通用高性能方案。

## 设计约束(推断) {#CHR-005}
<!-- ndf: kind=constraint level=should layer=L0 status=stable since=0.2 source=deduced -->

除 [[CHR-004]] 的硬约束外,以下软约束指导设计决策:

1. ~~**零外部运行时依赖**~~（已删除：裁决--伪约束，非主动设计，是 hnswlib header-only 特性的副产物）
2. **Linux 优先**:不追求跨平台,利用 io_uring、O_DIRECT、cgroup v2 等 Linux 特有能力
3. **C++17 而非 C++20**:保持与主流编译器的兼容性,避免 C++20 coroutine/modules 的编译器差异
4. **离线索引构建**:搜索是在线路径,索引构建是离线 batch。不追求在线插入性能
5. **可测量性**:每个优化 SHOULD 有 benchmark 数据支撑。未达预期的优化（如 `FINE_MERGE`、`SPEC_PREFETCH`）默认关闭并记录原因。这是理想目标而非强制纪律--代码中无机制阻止无 benchmark 的合入

## 设计约束 {#CHR-004}
<!-- ndf: kind=constraint level=must layer=L0 status=stable since=0.1 source=observed -->
<!-- updated=2026-07-30: DEC-044 双维度 SLA -->

1. 常驻内存 MUST ≤ cgroup MemoryMax(典型 512MB)
2. 搜索召回率 MUST ≥ 95% (Recall@10)
3. 所有数据准备步骤 MUST 用同一套 base 数据(graph/PQ/GT 共享 node id 空间)
4. vecblocks 与 route table MUST 配套生成,不可跨版本混用
5. **双维度性能 (DEC-044)**: 
   - 内存受限时: DiskHNSW MUST 显著优于 hnswlib (已验证 ✅, 10x@512MB)
   - 全量放开时: DiskHNSW SHOULD 达到 hnswlib 70%+ QPS (当前 21%, 待优化 🔴)
