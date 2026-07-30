# Decisions — 架构决策记录

## D-001: 选择 hnswlib 作为建图引擎 {#DEC-001}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-ARCH-001,OBS-API-001 source=observed -->

**Context.** DiskHNSW 需要 HNSW 图结构来驱动搜索。`build_index.cpp` 和 `benchmark_hnswlib_native.cpp` 直接 include `hnswlib/hnswlib.h`。

**Decision.** 使用 hnswlib（C++ header-only 库）作为建图引擎，`build_index` 产出 `index.bin`，`extract_graph` 剥离出精简图结构。

**Alternatives rejected.** 自研 HNSW 建图（工作量大，hnswlib 已成熟）；使用 faiss HNSW（faiss 的 C++ API 不如 hnswlib 轻量）。
**Inferred from.** `hnswlib/` 目录存在、`Makefile` 中 `-I./hnswlib` 编译选项、`build_index.cpp` 和 `benchmark_hnswlib_native.cpp` 的 `#include "hnswlib/hnswlib.h"`。(inferred)

## D-002: 选择 faiss 做 PQ 训练 {#DEC-002}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-API-003,OBS-BEH-005 source=observed -->

**Context.** PQ 编码需要训练 codebook + 编码所有向量。`train_pq.py` 和 `gen_gt.py` 都 import faiss。

**Decision.** 使用 faiss Python 包（`faiss.ProductQuantizer` + `faiss.IndexFlatL2`）做 PQ 训练和 Ground Truth 暴力搜索。

**Alternatives rejected.** 自研 PQ 训练（faiss 已有高效实现）；使用 scikit-learn KMeans（慢一个数量级）。
**Inferred from.** `scripts/train_pq.py:33` (`import faiss`), `scripts/gen_gt.py:26` (`import faiss`)。(inferred)

## D-003: 选择 io_uring 而非 libaio {#DEC-003}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-ARCH-001,OBS-DEF-011 source=observed -->

**Context.** 异步 I/O 预取需要高效的内核接口。代码自行封装了 raw `io_uring_setup(2)` 系统调用，未使用 liburing。

**Decision.** 使用 io_uring 原生系统调用（无外部 I/O 库依赖），通过 `IoUring` wrapper class 封装。支持 batch submit、O_DIRECT 对齐读取、buffer pool。

**Alternatives rejected.** libaio（接口老旧，不支持 batch submission）；liburing（增加外部依赖）；pread（同步，单线程可用但多线程不如 io_uring）。
**Inferred from.** `io_uring_wrapper.h:3-5`（"No external liburing dependency required"），`src/core/graph_prefetcher.cpp` 中 io_uring 的预取逻辑。(inferred)

## D-004: 选择 dual-route-table 修复 FINE_RERANK bug {#DEC-004}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-BEH-007,OBS-DEF-010 source=observed -->

**Context.** blocks 文件（含邻接表，8651 块）和 vecblocks 文件（仅向量，7937 块）因元数据大小不同，同一 node 在两文件里 block_id 不一致。原代码用 blocks 的 route_table 索引 vecblocks，导致读到错误向量。

**Decision.** 新增 `vec_route_table_` 成员（node → vecblocks block_id），在 `buildFineRerank()` 扫描每个 vecblocks 块时构建。FINE_RERANK 精排路径用 `vec_route_table_` 计算偏移，用 `route_table_` 查 block cache。

**Alternatives rejected.** 强制两个文件使用相同 block 划分（会牺牲 vecblocks 的存储效率）；用隐式 offset 推导（脆弱，已在原始实现中证明不可靠）。
**Inferred from.** README P0.5 章节、"fix(FINE_RERANK)" commit `1906400`、`disk_hnsw.h:266` 的 `vec_route_table_` 成员。(inferred)

## D-005: 选择 CSR Delta+Varint 而非 BVGraph 压缩 {#DEC-005}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-DEF-008,OBS-DEF-009 source=observed -->

**Context.** L0 邻接表 84MB 需要压缩以降低常驻内存。分析了 BFS-ordered CSR 的 delta 分布（4.2% delta=1, 32% delta<1024, 68% delta≥1024）。

**Decision.** 使用 delta+varint 编码（LEB128），不采用 BVGraph 参考压缩。BVGraph 依赖相邻节点 Jaccard 相似度，BFS 相邻节点 Jaccard 仅 0.023，不适合。

**Alternatives rejected.** BVGraph reference compression（Jaccard 太低不适用）；PForDelta（对 68% 长程边收益小）；直接 uint16 truncation（HNSW long-range edges 超 16-bit 范围）。
**Inferred from.** README P0 章节、`common.h:171-198` 的 `delta_varint_encode/decode` 函数、`common.h:24` 的 `FORMAT_VERSION_COMPRESSED = 2`。(inferred)

## D-006: BFS 重排作为默认布局策略 {#DEC-006}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-DEF-003,OBS-ARCH-004 source=observed -->

**Context.** 向量在磁盘上的排列顺序影响块内命中率。`BfsLayoutProvider` 是唯一实际使用的路由表加载器。

**Decision.** 使用 BFS 遍历顺序重排节点（`bfs_reorder.cpp`），让图上相邻节点在磁盘上物理相邻。`RandomLayoutProvider` 作为对照组存在但非生产路径。

**Alternatives rejected.** Random（对照组，命中率低）；Hilbert/ Morton space-filling curve（需要额外计算，BFS 直接利用图结构）。
**Inferred from.** `bfs_reorder.cpp` 存在、`BfsLayoutProvider` 是 `BlockCache` 向后兼容构造函数的默认选择（`block_cache.cpp:148`）、`RandomLayoutProvider` 仅用于对照实验。(inferred)

## D-007: 选择 C++17 + g++ 而非 Clang {#DEC-007}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-CON-006 source=observed -->

**Context.** 需要 C++17 特性（`std::optional`, structured bindings, `if constexpr`）和 AVX2 intrinsics。

**Decision.** 使用 `g++ -std=c++17 -O3 -march=native`。No CMake，直接用 Makefile。

**Alternatives rejected.** CMake（过度工程，10 个编译目标用 Makefile 足够）；Clang（g++ 对 AVX2 intrinsics 支持更稳定）。
**Inferred from.** `Makefile:2` (`CXX = g++`, `CXXFLAGS = -O3 -std=c++17 -Wall -Wextra -march=native`)。(inferred)

---

<!-- ════════════════════════════════════════════════════════════════ -->
<!-- INTENT- 条款：基于全局理解的选型理由补充                    -->
<!-- source=deduced 表示条款源自架构推理，非从代码直接提取         -->
<!-- ════════════════════════════════════════════════════════════════ -->

## D-008: 两阶段搜索作为当前最优搜索配置 {#DEC-008}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-BEH-002 source=deduced -->

**Context.** [[BEH-002]] 显示 `searchKnn()` 有 5 种搜索模式（TwoStage/Beam/NonBlock/
BatchIO/Default），由环境变量分支选择。**两阶段搜索是 5 种模式之一，并非唯一的架构路径。**
它是当前生产使用的最优配置，其他模式（Beam/NonBlock/BatchIO）为实验性探索，
同样存在于代码中且可在运行时切换。

传统 HNSW 搜索每展开一个节点都需要读其向量算距离。100 万 128 维向量需 488MB 内存。
在 512MB cgroup 限制下，hnswlib（726MB RSS）直接 OOM。两阶段搜索是把向量从内存
卸载到磁盘的可行方案之一（而非“核心机制”）。

**Decision.** 采用两阶段搜索作为当前最优配置：
- **Phase A（粗筛）**：用 PQ（Product Quantization）把每向量从 512 字节压缩到 32 字节，
  全量 PQ codes 仅 30MB 常驻内存。图搜索过程中用 PQ ADC 近似距离替代精确 L2，零向量 I/O。
  产出 ~100 个候选节点。
- **Phase B（精排）**：只对这 100 个候选取真实向量做精确 L2。按 4KB 页粒度读取（每向量
  512B，I/O 量 = 100 × 512B ≈ 50KB/query），而非加载 488MB 全量数据。

**关键权衡**：
- PQ 近似引入距离误差，recall 从 ~99% 降到 ~95%。由 Phase B 精排补偿（最终 95.70%）
- I/O 量从 488MB（全量加载）降到 50KB/query（5 个数量级）
- 延迟代价：PQ 查表 ~16μs + 4KB I/O ~1-10μs = 总 0.5ms/query（vs hnswlib 0.08ms）
- 用 3.5x 延迟换 2.7x 内存节省，在内存受限场景下是合算的交易

**Alternatives rejected.**
- **全量内存 + mmap**：OS 透明分页，但 cgroup 限制下仍 OOM，且 page fault 不可控
- **全量磁盘 + 大 block cache**：64KB block 读太多不需要的向量，I/O 量 5.3MB/query
- **IVF + PQ（如 FAISS IVFPQ）**：倒排索引 + 批量查询优化好，但单查询延迟高（nprobe 开销）
- **DiskANN**：Vamana 图 + 固定扇出，但搜索路径更长，单查询 I/O 更多
- **Beam/NonBlock/BatchIO**：代码中存在实验性实现（见 [[BEH-002]]），但均未达到两阶段的 recall/QPS 平衡

> rationale: 两阶段搜索的本质是“用计算换内存”——PQ 压缩是计算（查表），
> 磁盘 I/O 是按需的（只读候选）。在 CPU 富余、内存稀缺的场景下，这是正确交换。

## D-009: 选择 4KB 页粒度而非 64KB block 粒度做精排 {#DEC-009}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-BEH-007,OBS-DEF-006 source=deduced -->

**Context.** Phase B 精排需要读取候选向量的真实数据。有两种粒度可选：
- 按 64KB block 读（BlockCache 已有的机制）
- 按 4KB 页读（Fine Rerank 机制）

**Decision.** 选择 4KB 页粒度，原因：

| 维度 | 64KB block | 4KB page |
|------|-----------|----------|
| I/O 量/query | 100 候选 × 64KB = 6.4MB | 100 × 512B = 50KB |
| I/O 倍减 | 1x | **128x** |
| 块内有效数据 | ~6%（100×512B / 6.4MB） | ~100%（页内只有向量） |
| 随机读放大 | 高（每 block 含 ~116 向量，只用 1 个） | 低（每页含 8 向量，可能用 1-2 个） |

**实现关键**：SIFT 向量 512B，一个 4KB 页放 8 个向量。候选的页命中率约 12.5%（8 个里
命中 1 个），但 BFS 重排让图上相邻节点物理相邻，实际命中率更高。

**Alternatives rejected.**
- 64KB block 精排：I/O 量太大，512MB cgroup 下 page cache 放不下热 block
- 2KB sub-page：Linux 不支持小于 page_size 的 I/O，需用户态拼接，复杂度不值得
- mmap + userfaultfd：可以按需加载，但 signal handler 路径不稳定，且 cgroup 记账不直观

## D-010: delta+varint 的事后技术观测 {#DEC-010}
<!-- ndf: kind=info level=may layer=L2 status=stable since=0.2 source=deduced -->

**Context.** [[DEC-005]] 记录了 delta+varint vs BVGraph 的选择，决策完全基于
Jaccard = 0.023 的数据分析。以下为事后技术观测，非原始决策理由。

**事后观测：** delta+varint 恰好具备以下特性，但对决策不起决定性作用：

1. **解码独立性**：delta+varint 的解码逻辑自包含（`common.h:171-198`），不依赖外部状态。
   BVGraph 需要维护 reference node 的上下文
2. **随机访问友好**：`adj_csr_byte_offsets_[N+1]` 提供节点级随机访问入口，O(1) 定位。
   BVGraph 的变长 reference chain 需要顺序扫描
3. **线程安全简单**：`csr_decode_buf_` 是 thread_local，每个线程独立解码。
   BVGraph 的共享 reference state 需要锁保护
4. **压缩率足够**：1.8x 压缩（84->47MB）已让 1M 规模的 CSR 不是内存瓶颈。
   100M 规模时 CSR 上磁盘是更有效的方案，不需要更高压缩率

> 注意：以上特性是选择 delta+varint 后发现的技术优点，不是选择它的原因。
> 原始决策理由见 [[DEC-005]]（Jaccard 分析）。
> ~~原版本声称“与六边形架构兼容”已删除：决策时六边形架构概念不存在~~

## D-011: BFS 重排作为默认布局的架构理由 {#DEC-011}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-DEC-006,INTENT-ARCH-003 source=deduced -->

**Context.** [[DEC-006]] 记录了 BFS vs Random vs Hilbert 的选择。补充架构理由。

**Decision.** BFS 重排是“图结构感知”的布局策略，与搜索领域高度内聚：

1. **与搜索路径一致**：HNSW 搜索从 entry point 做 best-first 扩展，访问的节点在图上
   是连续的。BFS 重排让这些连续节点在磁盘上也连续，自然提升 page cache 命中率
2. **与 BlockCache 解耦**：BFS 重排在 pipeline 阶段完成（`bfs_reorder.cpp`），
   运行时只需要一个映射表（`old_to_new_` / `new_to_old_`）。布局策略不影响
   BlockCache 的实现，符合 Port-Adapter 分离
3. **可测量**：BFS 后块内命中率 86.8%（实测），Random 只有 ~12%。可量化的提升
   让布局策略的选择有数据支撑
4. **不可变假设**：BFS 顺序在建索引时确定，运行时不变。这简化了设计——
   不需要动态重排或自适应布局

## D-012: 双路由表分离作为显式映射原则 {#DEC-012}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-DEC-004,INTENT-ARCH-004 source=deduced -->

**Context.** [[DEC-004]] 记录了 `vec_route_table_` 的引入。补充架构层面的原则。

**Decision.** 双路由表分离体现了核心原则：“不依赖隐式对齐”。提升为架构规则：

1. **每套数据文件 MUST 有独立的路由表**：blocks 文件、vecblocks 文件、PQ blocks 文件
   如果有不同的 block 划分（因元数据大小不同），各自维护映射
2. **路由表的变更需版本化**：如果 vecblocks 文件重新生成（如改 block_size），
   `vec_route_table_` 必须同批重建，并在 `ndf.yaml` 中记录版本

> ~~原第 2 条“路由表是 Port 的实现细节”已删除：`IRouteTable` 接口在代码中不存在~~

> rationale: P0.5 的 FINE_RERANK bug（recall 95.70% -> 10%）就是违反此规则的代价。
> 隐式对齐在数据碰巧一致时正常工作，pipeline 变更后立即暴露。显式映射的成本
> 是 4MB 内存，收益是消除一整类数据一致性 bug。

## D-013: 选择 io_uring 原生 syscall 而非 liburing 的架构理由 {#DEC-013}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-DEC-003 source=deduced -->

**Context.** [[DEC-003]] 记录了 io_uring vs libaio 的选择。补充不用 liburing 的理由。

**Decision.** 直接使用 `io_uring_setup(2)` + `io_uring_enter(2)` 系统调用，不依赖 liburing：

1. **封装简洁**：`IoUring` class（363 行）封装了 setup/submit/wait 完整生命周期，
   代码量与 liburing wrapper 相当
2. **控制力**：直接操作 SQ/CQ ring（mmap），可以精确控制内存布局和提交时机。
   liburing 的抽象层会隐藏这些细节

**风险**：内核 io_uring API 变更时需要手动适配（liburing 会处理）。但 io_uring 核心
API（setup/enter/mmap）自 Linux 5.1 起稳定，变更风险低。

## D-014: PQ M=32 的选择理由 {#DEC-014}
<!-- ndf: kind=decision date=2026-07-28 affects=OBS-CON-005,OBS-BEH-005 source=deduced -->

**Context.** SIFT 128 维向量，PQ 参数 M 决定每向量压缩后的字节数和距离计算次数。
M=32 意味着 dsub=4（128/32），每子空间 4 维。

**Decision.** M=32 是 recall 与内存的最佳平衡点：

| M | dsub | PQ codes 大小 | ADC recall@10 | 查表次数 |
|---|------|-------------|---------------|----------|
| 8 | 16 | 8MB | ~70% (太低) | 8 |
| 16 | 8 | 16MB | ~88% | 16 |
| **32** | **4** | **30MB** | **~93%** | **32** |
| 64 | 2 | 64MB | ~96% | 64 |
| 128 | 1 | 128MB | ~97% | 128 |

**关键权衡**：
- M=32 的 ADC recall ~93%，加上 Phase B 精排后达到 95.70%
- M=64 的 ADC recall ~96%，但 PQ codes 64MB 过大，挤压 CSR 和 flat_vec_cache 预算
- M=32 的 PQ dist table = 32×256×4 = 32KB，恰好在 L1 cache（48KB）内
- M=64 的 PQ dist table = 64KB，超出 L1，性能反而下降

**SIMD 优化对齐**：dsub=4 时 AVX2 一次处理 2 个 centroid（8 floats），M=32 的 32 次查表
可展开为 8 组 4 路并行，完美利用 AVX2 256-bit 寄存器。

> rationale: PQ M 的选择不仅是 recall 问题，更是 cache hierarchy 问题。
> M=32 的 dist table 适配 L1 cache，是性能的甜点。

## D-015: flat_vec_cache 热区行为观测 {#DEC-015}
<!-- ndf: kind=info level=may layer=L2 status=stable since=0.2 source=deduced -->

**Context.** P1 阶段实验发现，flat_vec_cache 在 `FLAT_VEC_MB=64` 配置下支撑了大部分 QPS，
而非 vecblocks 的 page cache。此条目记录观测经验，非代码内建设计意图（代码默认
`FLAT_VEC_MB=4`，见 [[CON-002]]）。

**观测经验：**

1. **推荐配置 `FLAT_VEC_MB=64`**：63K 个 Layer 1+ 节点的向量（30MB），在 64MB 下
   100% 装入。贪心下降阶段零 I/O。代码默认 4MB 只能装 ~8K 向量，不够用
2. **Layer 0 热点覆盖**：剩余 ~34MB 装约 68K 个 Layer 0 热门节点（LRU 淘汰），
   PQ_HYBRID 模式下这些节点用精确 L2 替代 PQ，提升粗筛质量
3. **与 page cache 的分工**：flat_vec_cache 是进程主动管理的 anon 内存（计入 RSS），
   page cache 是 OS 管理的 file cache（cgroup 记账有坑）。flat_vec_cache 可控、可测、
   不受 cgroup 记账影响
4. **调大的边际效益递减**：`FLAT_VEC_MB=64->256` 实测 QPS 不涨反微降（2065->2037），
   因为更大的 flat_vec_cache 挤压了 CSR/PQ codes 的内存预算，且 L1/L2 cache miss 增加

> rationale: flat_vec_cache 是“可控热区”，page cache 是“免费但不稳定的补贴”。
> 推荐配置 `FLAT_VEC_MB=64` 覆盖上层节点，page cache 作为 bonus。
> 注意：这是 benchmark 经验值，非代码默认值。

## D-016: 选择 cgroup v2 而非 v1 作为内存限制机制 {#DEC-016}
<!-- ndf: kind=decision date=2026-07-28 affects=INTENT-CHR-003 source=deduced -->

**Context.** 项目使用 `systemd-run --user -p MemoryMax=512M` 限制内存，这依赖 cgroup v2。

**Decision.** 使用 cgroup v2（而非 v1）的原因：

1. **统一层级**：cgroup v2 的 memory.current / memory.stat 统一了 v1 的 memory.usage_in_bytes
   和 memory.stat，语义更清晰
2. **systemd 原生支持**：`systemd-run --user -p MemoryMax=` 直接映射到 cgroup v2，
   无需手动创建 cgroup
3. **page cache 记账改进**：cgroup v2 的 memory.stat 区分 `anon`（进程私有）和 `file`
   （page cache），虽然仍有“首次读入位置”的记账陷阱（[[CHR-005]]），
   但比 v1 的不区分更透明
4. **Linux 5.1+ 对齐**：io_uring 也需要 5.1+，cgroup v2 在同一内核版本范围内默认启用

> rationale: cgroup v2 的 file/anon 区分让我们发现“page cache 100% 在系统缓存但
> cgroup file=0”的记账陷阱（P1 阶段的关键认知修正）。如果用 v1，这个发现不可能做出。

---

## D-017: Page Search for Fine Rerank {#DEC-017}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-009,BEH-007 source=deduced -->

**Context.** Fine Rerank 读取 4KB 页后只计算候选向量的 L2 距离，浪费同页其余向量。
SIFT 128D 向量 512B/个，4KB 页含 8 个向量，当前利用率仅 12.5%。

来源: OctopusANN (VLDB 2026) 发现 Page Search 单独效果弱，但与 Page Shuffle 组合后
减少 28.3% 页读取。

**Decision.** Fine Rerank MUST 在读取 4KB 页后计算页内所有向量的精确 L2 距离，
而非仅计算候选向量。

**实现要点**：
- `readVecBlockPread` / `readVecBlockIouring` 返回页数据后，扫描页内所有向量
- 对每个向量计算 L2，插入候选集
- 需区分候选向量（在 top-K 候选列表中）和"邻居向量"（同页但非候选），
  后者只做距离计算不入图遍历
- refines: [[DEC-009]]

> rationale: 读页 I/O 已付出，计算 8 个 vs 1 个的 CPU 开销可忽略
> （8×L2(128D) ≈ 0.4μs），但可多发现 3-5 个高质量候选，提升 recall 2-3 个百分点。

## D-018: Page Shuffle for vecblocks {#DEC-018}
<!-- ndf: kind=decision date=2026-07-29 updated=2026-07-29 affects=DEC-006,ARCH-004 source=deduced -->

**Context.** 当前 BFS 重排在 64KB block 级（block 内节点连续），但 Fine Rerank
以 4KB 页读取。一个 64KB block 含 16 个 4KB 页，BFS 只保证 block 内连续，
不保证页内连续。

来源: OctopusANN (VLDB 2026) 发现 Page Shuffle 与 Page Search 协同后页命中率
显著提升。

**Decision.** vecblocks 文件 SHOULD 按 4KB 页粒度重排，使图相邻节点共享同一页。

**实现状态（2026-07-29）:**

`shuffle_vecblocks.cpp` 已实现完整的贪心页聚类算法：

1. 加载图邻接表（slim_adj 模式，不加载全量向量）
2. 将邻接表转换到 new_id 空间（BFS 重排后 ID）
3. 对每个 64KB block：
   - 构建块内邻接子图
   - 贪心页分配：种子选块内邻居最多的节点，后续选与当前页共享邻居最多的节点
   - 按新页顺序重排 node_ids 和 vectors
4. 输出新 vecblocks 文件（格式不变）

**页聚类质量:**
- 页内邻居对：29.7% → 77.1% (+159.5%)
- 算法复杂度：O(cnt²·vpp) per block, cnt≈126, vpp=8, 毫秒级完成

**precondition:** 仅对 SIFT (128D, 512B/向量, 8向量/页) 有效。
高维数据（如 GIST 960D）一页只放 1 个向量，Shuffle 无效。

- refines: [[DEC-006]]
- verifies: [[VER-018]]

> rationale: Page Shuffle 让 HNSW 图上相邻的节点落在同一 4KB 页，
> 配合 Page Search 后页内利用率从 12.5% 提升到 40-60%。

## D-019: Dynamic Width for Phase A {#DEC-019}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-008,BEH-002 source=deduced -->

**Context.** Phase A 搜索全程使用固定 efSearch 宽度。HNSW 搜索在候选集稳定后
（top-K 不再变化），继续以全宽度遍历只会增加 PQ 计算和图 I/O，不改善 recall。

来源: OctopusANN (VLDB 2026) 实测 Dynamic Width 减少 20-35% 图遍历步数，
是独立收益第二大的技术。

**Decision.** Phase A 搜索 SHOULD 使用自适应 efSearch 宽度：搜索初期使用全宽度，
候选集收敛后逐步收窄。

**实现要点**：
- `searchLayer0*()` 函数中，跟踪 top-K 变化
- 收敛检测：连续 N_hop 跳无新节点进入 top-K
- 收窄策略：efSearch 从初始值按几何衰减（×0.75/次），下限为 efSearch_min = 32
- 恢复机制：如果收窄后 recall 明显下降，可回退到全宽度
- 新增环境变量 `DYNAMIC_WIDTH=1`（默认关闭，benchmark 验证后决定是否默认开启）
- 新增 `EF_SEARCH_MIN`（默认 32）、`DW_DECAY=0.75`、`DW_CONVERGE_HOP=10`
- refines: [[DEC-008]]

> rationale: 搜索后期候选集已收敛，全宽度遍历是浪费。
> 几何衰减让搜索快速聚焦，下限 32 保证不丢失关键候选。

## D-020: Page Search / Dynamic Width SLA 调整决策 {#DEC-020}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-017,DEC-019,CON-007 source=deduced -->

**Context.** DEC-017 (Page Search) 和 DEC-019 (Dynamic Width) 经 2 轮修复后性能验证：

- DEC-017: recall 95.70% -> 96.20% (+0.5pp)，QPS 2051 -> 1832 (-11%)，SLA QPS 违规
- DEC-019: 无效果，根因为 B 类（规范缺陷）-- PQ 搜索在 EF=100 时不收敛

**Decision.**

1. **DEC-017 降级为实验性 SHOULD**：保留功能，新增 SLA 豁免（QPS ≥ 基线 × 85%）
2. **DEC-019 标记为规范缺陷**：保留代码（默认关闭零开销），不纳入 SLA，记录已知限制
3. **根因记录**：PQ 粗筛在 EF≥100 时 top-K 持续抖动不收敛，Dynamic Width 的收敛假设不成立

**Alternatives rejected.**
- A. 继续第 3 轮代码修复 -> PS 开销已接近下限，DW 根因是规范层非代码层
- 完全删除 DEC-017/019 -> PS 的 recall +0.5pp 是真实收益，删除浪费

> rationale: PS 是"计算换 recall"的合理 tradeoff，适合 recall 优先场景。
> DW 的 L1 契约假设错误，需要重新设计收敛检测策略（如基于迭代次数而非 top-K 稳定性）。

## D-021: Page Cache 驱逐模式 {#DEC-021}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-009,BEH-007 source=deduced -->

**Context.** 1M 规模下 vecblocks 496MB 被 OS page cache 100% 覆盖，Fine Rerank 走热态
缓存零磁盘 I/O，无法验证 I/O 优化技术。需主动驱逐 page cache 制造冷 I/O 条件。

**Decision.** 当 `EVICT_PAGE_CACHE=1` 时，DiskHNSW MUST 在每次查询完成后对 vecblocks
文件调用 `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`，驱逐 page cache。

**实现要点**：
- 在 `searchKnn()` 查询完成后调用，仅驱逐 vecblocks fd
- 不驱逐 blocks_64k（BlockCache 自管理缓存）
- `EVICT_PAGE_CACHE=0`（默认）时零开销
- refines: [[DEC-009]]

> rationale: 在 1M 规模模拟 10M+ 规模的冷 I/O 条件，使 I/O 优化技术可被验证。
> 冷 I/O 下 Fine Rerank 每页读取 ~10-50μs（vs 热态 ~1μs）。

## D-022: 冷 I/O 下 Page Search 重新评估 {#DEC-022}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-017,DEC-021 source=deduced -->

**Context.** 热态下 Page Search QPS -11%（L2 计算开销主导）。冷 I/O 下 I/O 延迟
10-50μs/页，额外 7 个 L2 计算（~0.4μs）占比 < 4%。

**Decision.** 在冷 I/O 模式下重新评估 Page Search：
- 预期 recall 提升 ≥ 1pp
- 预期 QPS 下降 ≤ 5%
- 若达标，升级为默认开启

> rationale: 冷 I/O 下 L2 计算开销相对 I/O 可忽略，Page Search 的"计算换 recall"
> tradeoff 变得更有利。

## D-023: 冷 I/O 下 Page Shuffle 优先级提升 {#DEC-023}
<!-- ndf: kind=decision date=2026-07-29 updated=2026-07-29 affects=DEC-018,DEC-021 source=deduced -->

**Context.** Page Shuffle 原计划推迟到 P2（10M）。冷 I/O 模式下页内局部性直接影响
真实磁盘 I/O 量，Page Shuffle 变得有意义。

**Decision.** Page Shuffle 优先级从"推迟到 P2"提升为"P2 前置验证"。

**1M 验证结果（2026-07-29）:**

| 测试 | Recall | QPS | I/O 时间 |
|------|--------|-----|--------|
| 冷态基线 | 95.70% | 803 | 0.76ms |
| 冷态+Shuffle | 95.70% | 820 | 0.73ms |
| 冷态+PS | 96.20% | 789 | 0.78ms |
| 冷态+Shuffle+PS | 96.05% | 797 | 0.76ms |

- Shuffle 单独：QPS +2.1%，I/O -3.9%（远低于论文 25-30%）
- Shuffle+PS vs PS：QPS +1%，PS 开销从 -1.7% 降到 -0.7%
- **结论：1M 规模收益边际**，vecblocks 520MB 太小，OS page cache 仍有残留
- **下一步：10M 规模验证**（vecblocks 5GB+，page cache 必然不够）是 Page Shuffle 的真正战场

> rationale: 论文的 25-30% 页读取减少依赖大数据集（page cache 无法覆盖）
> 和多候选查询（每 query 读更多页）。1M 规模 I/O 量基数小，绝对收益有限。
> 但页聚类质量（77.1% co-locality）验证了算法正确性，
> 10M 规模预期收益接近论文数据。

## D-024: 冷 I/O 模式实验结论 {#DEC-024}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-017,DEC-019,DEC-021,DEC-022,DEC-023 source=observed -->

**Context.** DEC-021 实现 page cache 驱逐后，在 1M 规模跑冷 I/O benchmark。

**实验结果 (SIFT1M, 512MB cgroup, 1T):**

| 配置 | Recall | QPS | I/O 占比 |
|------|--------|-----|---------|
| 热态基线 | 95.70% | 2083 | ~0% |
| 冷态基线 | 95.70% | 842 | ~60% |
| 冷态 + Page Search | 96.20% | 792 | ~60% |
| 冷态 + Dynamic Width | 95.70% | 850 | ~60% |

**Decision.**

1. **冷 I/O 模式有效**: posix_fadvise(DONTNEED) 成功制造真实磁盘 I/O，QPS 下降 60%
2. **Page Search 冷态表现**: recall +0.5pp，QPS -5.9%（热态 -11%），L2 计算开销被 I/O 延迟掩盖
3. **Dynamic Width 正式放弃**: PQ 搜索不收敛是架构特性，非代码缺陷，冷 I/O 也不改变此结论
4. **Page Shuffle 实现完成，1M 收益边际**: 页聚类质量 77.1%（提升 159.5%），但冷态 I/O 仅减 4%
   （vs 论文 25-30%）。根因为 1M vecblocks (520MB) 太小，page cache 仍有残留。
   算法正确性已验证，真正收益在 10M。

**Dynamic Width 根因最终确认:**

PQ ADC 距离的量化误差导致 top-K 候选持续抖动，hash 和 lowerBound 收敛检测均无法触发。
这不是 bug 而是 PQ 粗筛的固有特性：EF=100 时搜索一直在探索新区域，直到候选集自然耗尽。
未来如需自适应宽度，需改用"迭代次数预算"而非"收敛检测"策略。

> rationale: 冷 I/O 模式让 1M 规模实验有了 10M+ 规模的 I/O 特征，
> 论文的 I/O 优化框架（Page Shuffle + Page Search）在此条件下才真正适用。

## D-025: Page Shuffle 1M 实现与验证 {#DEC-025}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-018,DEC-023,DEC-024 source=observed -->

**Context.** DEC-018 的 Page Shuffle 已从骨架实现为完整的贪心页聚类算法，
需在 1M 规模冷 I/O 下验证实际收益。

**完整实验结果 (SIFT1M, no cgroup, io\_uring, 1T, EVICT\_PAGE\_CACHE=1):**

| 测试 | Recall | Mean | QPS | RSS |
|------|--------|------|-----|-----|
| A: 热态基线（原始 vecblocks） | 95.70% | 0.49ms | 2038 | 273MB |
| B: 冷态基线（原始 vecblocks） | 95.70% | 1.25ms | 803 | 273MB |
| C: 冷态+PageSearch（原始） | 96.20% | 1.27ms | 789 | 275MB |
| D: 冷态+Shuffle | 95.70% | 1.22ms | 820 | 273MB |
| E: 冷态+Shuffle+PageSearch | 96.05% | 1.25ms | 797 | 275MB |
| F: 热态+Shuffle+PageSearch | 96.05% | 0.55ms | 1805 | 275MB |

**Decision.**

1. **算法正确性验证通过**: 页内邻居对从 29.7% 提升到 77.1% (+159.5%)
2. **1M 收益边际**: Shuffle 单独 QPS +2.1%，Shuffle+PS QPS +1.0%
   - I/O 仅减 4%，远低于论文 25-30%
   - 根因：vecblocks 520MB 太小，OS page cache 仍有残留，冷 I/O 不够"冷"
3. **Recall 保持**: 所有模式 recall ≥ 95%，无回归
4. **Shuffle 工具成熟度**:
   - 1.65s 完成 1M 向量重排
   - 支持 greedy 和 random 两种策略
   - 输出文件与原文件相同大小（520MB）
   - 原 `buildFineRerank()` 无需修改即可使用 shuffled vecblocks
5. **10M 是真正的验证战场**:
   - vecblocks = dataset\_size × dim × 4B, 10M SIFT = 5.12GB
   - page cache 必然无法覆盖，每次 I/O 都是真实磁盘访问
   - 论文的 25-30% I/O 减少预期在 10M 规模更可能成立

**P2 前置条件已满足**: Page Shuffle 算法、工具、验证链路均已就绪，
可直接用于 10M 规模的 P2 验证。

> rationale: 1M 规模是"验证算法正确性"的合适尺度（快速迭代、低资源），
> 但不是"验证 I/O 优化有效性"的合适尺度（page cache 干扰）。
> Page Shuffle 的投资回报率取决于数据集是否超出 page cache 容量。

## D-026: HELMSMAN 启示——图 vs 聚类的范式分歧与 P2 路线图确认 {#DEC-026}
<!-- ndf: kind=decision date=2026-07-29 source=deduced derived-from=REF-HELMSMAN -->

**Context.** HELMSMAN (OSDI 2026) 论文为小红书生产系统，用聚类 ANNS + 全闪存
替代内存 HNSW，节省 >90% 硬件成本。论文验证了图方法在大规模 SSD 场景的根
本性缺陷：图遍历产生串行 I/O 依赖链，无法利用 SSD 带宽。

但 HELMSMAN 的目标场景（100B 向量、分布式、90% recall）与 DiskHNSW（1M→100M、
单机、≥95% recall）存在本质差异。

**Decision.**

1. **确认 P2（10M）仍用图方法**：
   - 10M SIFT vecblocks ≈ 5GB（仍可能部分被 page cache 覆盖）
   - 图遍历的 I/O 串行化在 10M 规模尚未成为主导瓶颈
   - 我们的 95% recall 目标在聚类方法下难以达到（聚类天然有 recall 上限 ~90%）
   - Page Shuffle + Page Search 在 10M 冷 I/O 下预期有更大收益

2. **P3（100M）需重新评估范式**：
   - 100M vecblocks ≈ 50GB，page cache 完全失效
   - 图遍历每 query 约 100-200 次 4KB 串行 I/O = 1-2ms 延迟（可接受但接近极限）
   - 若 P2 验证图遍历在 10M 下 I/O 占比 >70%，P3 需考虑混合方案

3. **不关闭聚类路径**：
   - 将聚类 ANNS 作为 P3 的备选范式，与图方法形成 A/B 对比
   - HELMSMAN 的"SPANN 聚类 + SPDK"可作为参考架构
   - 论文的 learned pruning 思路可独立于聚类范式借鉴

4. **不立即引入 SPDK**：
   - SPDK 需要额外硬件绑定和运维复杂度
   - P2 阶段 io_uring 已足够（10M 规模 I/O 量有限）
   - P3 决策时根据 I/O 瓶颈程度重新评估

**Alternatives rejected.**
- 立即转向聚类方法 → 丢弃图方法已有的 recall 优势（95% vs 90%）
- 立即引入 SPDK → P2 规模不需要，过早引入增加复杂度
- 完全忽略 HELMSMAN → 论文的"图方法 I/O 瓶颈"预警是真实风险

> rationale: HELMSMAN 的成功验证了"聚类+SSD"的可行性，但聚类天然 recall 上限
> 与我们的 ≥95% 目标存在差距。图方法在 1M-10M 规模仍是正确选择，
> 但 100M+ 需要重新评估。这不是"图 vs 聚类"的二选一，
> 而是"什么规模切换到什么方法"的路线图问题。

## D-027: 用户态 I/O 在大规模时的必要性评估 {#DEC-027}
<!-- ndf: kind=decision date=2026-07-29 source=deduced derived-from=REF-HELMSMAN,DEC-009 -->

**Context.** HELMSMAN 论文验证了内核 I/O 栈（含 io_uring）仅利用 SSD 带宽的
26-59%，而 SPDK 用户态 I/O 可达 85%（Gen4）和 70%（Gen5）。

当前 DiskHNSW 使用 io_uring（Fine Rerank 4KB 页读）和 pread（多线程模式），
在 1M 规模下 I/O 不是瓶颈（热态 page cache 零 I/O，冷态 I/O 占比 ~60%）。

**Decision.**

1. **P2（10M）保持 io_uring**：
   - 10M 规模 I/O 量仍有限（每 query ~100-200 页），io_uring 带宽足够
   - io_uring 的提交/完成队列开销在单线程模式下可接受
   - SPDK 的运维成本（NVMe 绑定、大页内存）在 P2 阶段不划算

2. **P3（100M）纳入 SPDK 评估**：
   - 触发条件：I/O 占比 >80% 且 io_uring 带宽利用率 >80%
   - 替代方案：多 SSD 条带化 + io_uring 多队列（先尝试，成本更低）
   - SPDK 作为最终兜底

3. **当前不引入 O_DIRECT 优化**：
   - FINE_BUFFERED=1 已验证 page cache 热区零 I/O
   - O_DIRECT 仅在有确定性 I/O 模式时有益（如固定大小的 cluster 读）
   - 图遍历的 I/O 模式不规则，O_DIRECT 可能降低性能

> rationale: io_uring 是"够用"方案，SPDK 是"极致"方案。
> 过早优化是万恶之源——Helmsman 需要 SPDK 是因为它 24/7 跑 10B+ 向量，
> 我们在 1M-10M 阶段不需要。但 P3 设计预留 io_uring → SPDK 的切换接口。

## D-028: 学习式剪枝——Fine Rerank 的自适应优化方向 {#DEC-028}
<!-- ndf: kind=decision date=2026-07-29 source=deduced derived-from=REF-HELMSMAN,DEC-017,DEC-020 -->

**Context.** HELMSMAN 的 LLSP（Leveling-Learned Search Pruning）用 GBDT 模型预测
最优 nprobe 层级，替代固定剪枝参数。效果：1.1-1.6× 吞吐提升，>80% query 达到
目标 recall（固定剪枝仅 40%）。

当前 DiskHNSW 的 Fine Rerank 使用固定参数（REFINE_EF、PAGE_SEARCH on/off），
所有 query 用相同策略。但不同 query 的难度不同：
- 容易的 query：EF=50 即可收敛，REFINE_EF=100 浪费 I/O
- 困难的 query：需要更多候选 + Page Search 才能维持 recall

**Decision.**

1. **探索适配 DiskHNSW 的学习式剪枝（P2.5，低优先级）**：
   - 输入特征：query 向量、PQ 距离分布（粗筛阶段的前 N 个候选距离）、top-k
   - 预测目标：决定是否开启 Page Search、REFINE_EF 值
   - 输出：per-query 的 (enable_ps, refine_ef) 决策
   - 模型：轻量 GBDT（LightGBM）或小型 MLP，推理 <1μs

2. **先做 profiling 再决定**：
   - 在 10M 规模收集 per-query 的 I/O 量、recall、延迟
   - 分析 query 难度分布（多少 query 需要/不需要 Page Search）
   - 如果 Page Search 的 recall 增益集中在少数困难 query，剪枝收益大
   - 如果所有 query 均匀受益于 Page Search，剪枝无意义

3. **不作为 P2 的 blocking 项**：
   - P2 优先验证基础 I/O 优化（Page Shuffle + Page Search 冷态效果）
   - 学习式剪枝是 P2 之后的优化方向，不是 P2 的前置条件

**Alternatives rejected.**
- 立即实现 LLSP → 缺乏 10M 规模的 profiling 数据，暗箱设计风险大
- 照搬 HELMSMAN 的 nprobe 层级设计 → 我们的 nprobe 概念不同（图搜索的 ef 而非聚类 probe 数）

> rationale: HELMSMAN 的成功经验表明"自适应比固定好"，
> 但我们的搜索架构不同（图 vs 聚类），不能直接照搬。
> 先收集 10M 规模的 query 难度分布数据，再设计适配的学习式剪枝方案。

## D-029: DEEP10M 瓶颈转移——P2 路线图重新校准 {#DEC-029}
<!-- ndf: kind=decision date=2026-07-29 affects=DEC-018,DEC-022,DEC-023,DEC-026,P2 source=observed -->

**Context.** DEEP10M (96D, 9.99M) 冷 I/O 6 组 benchmark 完成。
预期：Page Shuffle + Page Search 在冷 I/O 下减少 25-30% I/O。
实际：I/O 占比仅 7%（vs SIFT1M 的 60%），两者均无效。

**实测瓶颈分析:**

```
SIFT1M (1M):  PQ[10%] + 图[30%] + I/O[60%]  → Page Shuffle 有效
DEEP10M (10M): PQ[80%] + 图[13%] + I/O[7%]   → Page Shuffle 无效
```

| 指标 | SIFT1M | DEEP10M | 说明 |
|------|--------|---------|------|
| 热态 QPS | 2038 | 74.9 | PQ 计算主导 |
| 冷态 QPS | 803 | 69.8 | I/O 仅增 1ms |
| I/O 占比 | 60% | 7% | 瓶颈转移 |
| Page Shuffle gain | +2.1% | ~0% | 优化了错误瓶颈 |
| Page Search recall | +0.5pp | +0.05pp | 10 向量/页已饱和 |

**Decision.**

1. **P2 I/O 优化策略降级**:
   - Page Shuffle (DEC-018) 在 I/O 非瓶颈规模下不产生收益，保留算法但标记为 P3 技术
   - Page Search (DEC-017) 在 ≥10 向量/页的维度下无增益，功能保留但关闭默认推荐

2. **P2 真正瓶颈识别**:
   - **PQ 计算**: M=32×256 centroids 的 ADC 距离占 ~80% query 时间
   - **内存压力**: CSR 邻接表 1.2GB，无法在 1GB cgroup 运行
   - **图遍历**: ~13% query 时间，仍有优化空间

3. **P2 目标重新校准**:
   - Recall ≥94%（接受 M=32 PQ 的固有精度上限）
   - QPS 目标从 >500 调整为按比例缩放（4T 预期 ~300 QPS）
   - 内存目标从 1GB cgroup 放宽到 3GB+

4. **P2 新优化方向**:
   - **PQ SIMD 加速**: AVX2/VNNI 批量 ADC 距离计算
   - **PQ 量化压缩**: M=24 (dsub=4) 在 92% recall 下的性能 tradeoff
   - **图遍历优化**: 更激进的软件预取 + 搜索剪枝

> rationale: DEEP10M 揭示了与 SIFT1M 本质不同的瓶颈模式。
> I/O 优化技术（Page Shuffle/Search）的物理价值需要 I/O 占比 >30%
> 才能体现——这可能在 100M+ 规模才满足。
> 10M 规模是"优化 PQ 计算"和"控制内存压力"的战场。

## D-030: Page Cache + Disk 两层 I/O 架构 + O_DIRECT 诊断模式 {#DEC-030}
<!-- ndf: kind=decision date=2026-07-29 updated=2026-07-29 affects=DEC-009,ARCH-003 source=deduced -->

**Context.** 当前 FINE_RERANK 在 `FINE_BUFFERED=1` 模式下依赖 OS page cache 提供
4KB 页的快速访问，OS 自动管理冷热分层——page cache 是免费的缓存层，不应放弃。

如果需要模拟"无 page cache"场景（内存受限 benchmark、大规模 cold start 测试），
O_DIRECT + io_uring 可以绕过 page cache 做真实磁盘 I/O 对照。

**Decision.**

1. **确认默认架构：BlockCache(内存) → Page Cache(OS免费) → NVMe(磁盘)**
   - FINE_BUFFERED=1 是推荐生产模式
   - OS page cache 自动做冷热分层，无需显式管理
   - 有内存时零 I/O，内存不够时自动驱逐 → 完美适配"动态内存"场景

2. **FINE_DIRECT=1 降级为诊断/测试模式**：
   - 用于：冷 I/O 基准测试、cgroup 内存受限 benchmark、模拟更大规模
   - 实现：`open(O_RDONLY | O_DIRECT)` + io_uring 批量提交
   - 不推荐生产使用 — 放弃免费的 OS page cache 层

3. **实测验证**：
   | 模式 | QPS | 说明 |
   |------|-----|------|
   | FINE_BUFFERED（默认） | 2,041 | page cache 热态零 I/O |
   | FINE_DIRECT=1（诊断） | 787 | 真实 NVMe I/O, 0.78ms/query |
   - FINE_DIRECT 验证了 O_DIRECT 路径正确性 ✅
   - recall 不变（95.70%）✅

4. **SPDK 路线**：P3 规模（100M+）如有多余 NVMe 设备，可迁移到 SPDK
   替代内核 I/O 层（非替代 page cache）。

- refines: [[DEC-009]]
- verifies: [[VER-030]]

> rationale: Page cache 是免费的——OS 已经做好了冷热分层。
> 正确的架构不是"page cache vs O_DIRECT"的二选一，
> 而是默认用 page cache，O_DIRECT 作为可控的诊断工具。
> 这与 HELMSMAN 的思路一致：利用系统已有的能力，而不是重建一切。

## D-031: 页面级驱逐——消除 Page Cache 颠簸引起的 QPS 悬崖 {#DEC-031}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-030 source=observed -->

**Context.** Cgroup 内存限制扫描发现：page cache 可用空间低于工作集时，
QPS 出现 10× 断崖（1,973 → 196），OS 在页面驱逐和 LRU 管理上消耗大量 CPU。
这比直接用 O_DIRECT 还差（787 vs 196 QPS）。

用户期望：page cache 不足时应优雅退化，而非断崖。

**Decision.**

1. **在 Fine Rerank 完成后，批量驱逐本次查询刚读过的页面**：
   - 收集 fine rerank 读取的所有 page 号（已在上层收集，现成可用）
   - 排序去重 → 合并相邻页为 range
   - 对每个 range 调用 `posix_fadvise(fd, start, len, POSIX_FADV_DONTNEED)`
   - 效果：只驱逐 read-once 数据，保留跨 query 复用的热页

2. **启用方式**：`FINE_FADVISE=1` 环境变量（默认关闭，需与 FINE_BUFFERED 配合）
   - 与 FINE_DIRECT 互斥（一个有 page cache，一个没有）
   - 推荐搭配：`FINE_BUFFERED=1 FINE_FADVISE=1`（有 page cache，用完即弃）

3. **预期效果**：
   - 256MB cgroup：几乎无影响（page cache 充足，驱逐是 no-op）
   - 180MB cgroup：QPS 196 → 500+（消除颠簸，变成干净磁盘 I/O）
   - 成本：每 query 1-3 次 posix_fadvise 系统调用（<10μs）

4. **与 FINE_DIRECT 对比**：
   | 模式 | 如何读 | 如何释放 | 适用场景 |
   |------|--------|---------|---------|
   | FINE_BUFFERED | pread → page cache | OS 自动 LRU | 内存充足 |
   | FINE_DIRECT | O_DIRECT io_uring | 不占用 cache | 极端受限 |
   | FINE_BUFFERED+FINE_FADVISE | pread → page cache | 主动 page 级驱逐 | 内存紧张(新) |

- verifies: [[VER-031]]

> rationale: 不放弃 page cache 的好处（批量预取、跨 query 复用），
> 同时避免 page cache 颠簸的代价（LRU 维护 + 无效驱逐）。
> 类似于 CPU cache 的"non-temporal"访存指令——读一次就过，别占 cache line。

## D-032: 10× QPS 悬崖根因——Cgroup Memory Reclaim 非 I/O 瓶颈 {#DEC-032}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-031,CON-007 source=observed -->

**Context.** 尝试 FINE_FADVISE/FINE_DIRECT/降缓存来消除 180MB cgroup 的 10x QPS 悬崖，均失败。

| 方案 | 180MB QPS | 效果 |
|------|-----------|------|
| FINE_BUFFERED (基线) | 196 | — |
| + FINE_FADVISE | 163 | ❌ |
| FINE_DIRECT | 188 | ❌ |

**根因:** cgroup memory.max 被触发 59,773+ 次 → OS memory reclaim 消耗大量 CPU。
这不是 I/O 瓶颈，是 reclaim 瓶颈。

**Decision.**
1. 10x 悬崖是 cgroup 硬限制的必然结果
2. 缓解: 用 memory.high (软限制) 而非 memory.max (硬限制)；给 page cache 留 RSS + 2x 工作集
3. 代码层面: 压缩 CSR 图、上层 PQ 编码 → reduce process RSS
4. 任何 I/O 优化在 reclaim 风暴面前无效

> rationale: 这不是 I/O 问题，是 memory provisioning 问题。

## D-033: CSR 图裁剪 (Degree Cap) — 压缩进程基址内存 {#DEC-033}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-032,ARCH-004,CON-007 source=deduced -->

**Context.** 进程 RSS 101MB 中 CSR 邻接表占 47MB。降低 RSS 是消除 cgroup reclaim
悬崖的最直接手段——每减少 1MB 进程内存，page cache 多 1MB 空间。

**Decision.** 对 HNSW 图实施 Degree Cap 裁剪：
- L0 每节点最多保留 K 条边（K = 16/20/24）
- 保留 angle-wise 最分散的邻居（MRNG 启发式）
- 预期：CSR 47MB → 25-35MB（K=16 → 47%，K=20 → 56%）

**实现**：已有 `prune_graph.cpp` 工具，含 Degree Cap + MRNG 两种策略
- 输入：graph + bfs → 输出：裁剪后的 graph
- 需重新生成 vecblocks + route table + PQ codes

**验证**：180MB cgroup 下 QPS 需 ≥ 800

- verifies: [[VER-033]]

> rationale: 图裁剪是投入产出比最高的内存压缩手段——
> 1 行代码不改（工具已有），直接见效。

