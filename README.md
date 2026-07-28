# DiskHNSW — 内存受限环境下的磁盘向量搜索

> 在 512MB cgroup 限额下，SIFT1M 数据集实现 95.70% recall / 5800+ QPS（4线程），
> 而 hnswlib 全内存方案需 726MB 会 OOM。

## 项目背景

传统 HNSW 向量搜索需要将全部向量加载到内存。对于 1M 条 128 维 SIFT 向量，hnswlib 需 726MB RSS。在容器化、边缘计算等内存受限场景下，这不可行。

DiskHNSW 的核心思路：

1. **图结构常驻内存**（上层节点向量 + L0 邻接表 CSR，~120MB），保证贪心下降和图遍历零 I/O
2. **向量数据存磁盘**，按 BFS 重排后分块，利用空间局部性
3. **PQ 粗筛**：用 Product Quantization 近似距离做图搜索，无需读磁盘向量
4. **精确精排**（Fine Rerank）：对 PQ 粗筛的候选集，按 4KB 页粒度读取向量做精确 L2 距离
5. **I/O 优化**：io_uring 异步 I/O + page cache 热区 + 软件预取消除间接寻址 cache miss

### 性能弧线（SIFT1M, 512MB cgroup, 单线程）

```
QPS: 53 → 595 → 867 → 2141 → 2643 → 2780    (52x 提升)
```

| 里程碑 | QPS | Recall | 技术 |
|--------|-----|--------|------|
| 基线 | 53 | 95.7% | 64KB 块同步读 |
| FINE_RERANK | 867 | 95.7% | 4KB 页粒度精排，I/O 减 16x |
| FINE_BUFFERED | 2141 | 95.7% | page cache 热区零 I/O |
| SIMD PQ LUT | 2643 | 95.7% | AVX2 距离表预计算 |
| SW 预取 | 2780 | 95.7% | 间接寻址 pipeline 预取 |
| 4线程并发 | 5808 | 95.7% | pread 多线程 + thread_local PQ table |

### 对比 hnswlib

| 指标 | hnswlib (全内存) | DiskHNSW (512MB cgroup) |
|------|-----------------|------------------------|
| Recall | 95.25% | **95.70%** (反超) |
| QPS (1T) | 12420 | 2780 |
| QPS (4T) | — | **5808** |
| RSS | 726MB (OOM@512MB) | **302MB** |
| 内存节省 | — | **2.4x** |

---

## 编译

```bash
# 依赖: Linux 5.1+ (io_uring), g++ C++17
make all        # 编译全部: pipeline 工具 + benchmark + 测试

# 或分组编译
make pipeline   # 只编数据准备工具 (Step 1-5)
make bench      # 只编 benchmark_diskhnsw + hnswlib 对比基线
make test       # 只编测试
```

> Makefile 的 `all` 已包含 `benchmark_diskhnsw` 和 `write_blocks_veconly`，无需再手动编译。

---

## 数据准备 Pipeline

以 SIFT1M（128 维，100 万向量）为例。**7 个步骤有严格的先后依赖**，下面每步都说明它
产出什么、下一步为什么需要它。

> ### ⚠️⚠️ 三条铁律（不遵守 recall 直接崩到 <1%）
>
> 1. **一套数据从头到尾**：`build_index`、`train_pq.py`、`gen_gt.py` 必须用**同一个**
>    base.fvecs。graph 的 node id、PQ codes 的行号、GT 的邻居 id 是**同一个 id 空间**
>    （base 向量的原始行号）。换了 base 数据其中一环，三者对不上，recall 归零。
> 2. **graph 与 blocks/route/vecblocks 必须同批生成**：Step 4/5 的块文件是从 Step 2 的
>    graph 算出 offset 的。**重新跑 Step 1-2 覆盖了 graph，就必须重跑 Step 4-5**，否则
>    offset 错位，recall 崩。（不要把新旧 output 目录的文件混着用。）
> 3. **PQ 的 M 要匹配维度**：SIFT dim=128 用 **M=32**；Deep dim=96 用 M=8。M 用错
>    （比如 128 维用了 M=8）PQ 粗筛精度极差，recall≈0。

### Step 1: 构建 HNSW 图 → `index.bin`
```bash
./build/build_index data/sift_base.fvecs output/sift1m_index.bin 16 200
```
产出 hnswlib 原生索引（含图 + 全部向量）。参数 `M=16 ef_construction=200`。
**下一步要从中剥离出精简图结构。**

### Step 2: 提取图结构 → `graph.bin`
```bash
./build/extract_graph output/sift1m_index.bin output/sift1m_graph.bin 128
```
从 index 剥离出 DiskHNSW 用的图（节点层级 + 上层向量 + labels + L0 邻接表 CSR），
丢掉 L0 全量向量。**这是后面所有块文件的 offset 基准，一旦重生成，Step 4/5 必须跟着重跑。**

### Step 3: BFS 重排 → `bfs.bin`
```bash
./build/bfs_reorder output/sift1m_graph.bin output/sift1m_bfs.bin
```
按图的 BFS 遍历顺序给节点重新编号，让图上相邻的节点在磁盘上也相邻（空间局部性），
提升块内命中率。**产出 old_id↔new_id 映射，Step 4/5 按这个顺序摆放向量。**

### Step 4: 生成向量块文件（Vec-Only）→ `vecblocks_64k.bin` (+ `_route.bin`)
```bash
./build/write_blocks_veconly output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_vecblocks_64k.bin 65536
# 同时生成配套 route: output/sift1m_vecblocks_64k_route.bin
```
把向量按 BFS 顺序打包成 64KB 块（**只存向量，不含邻接表**），供 Phase B 精排按 4KB 页
粒度读取。**route 表和块文件必须同一次调用产出，配套使用。**

### Step 5: 生成旧格式块文件（BlockCache 用）→ `blocks_64k.bin` + `route_64k.bin`
```bash
./build/write_blocks output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_blocks_64k.bin 65536
./build/gen_route output/sift1m_blocks_64k.bin output/sift1m_route_64k.bin
```
生成含邻接表的 64KB 块（BlockCache/fallback 路径用）及其路由表。

### Step 6: 训练 PQ 编码 → `pqco_*.bin`
```bash
# 用法: python3 scripts/train_pq.py <base.fvecs> <输出.bin> [M]
python3 scripts/train_pq.py data/sift_base.fvecs output/pqco_sift1m_M32_correct.bin 32
```
用 faiss 把 base 向量压缩成 PQ codes（codebook + 每向量 M 字节），供 Phase A 粗筛
做零 I/O 的 ADC 近似距离。
- **M 可通过第 3 个参数自定义**；缺省时按维度自动选（128→32, 96→8）。
- **必须用与 Step 1 相同的 base 数据**（PQ codes 按 base 原始行号编码，与 graph node id 对齐）。
- 脚本末尾打印重建 MSE 和 ADC top-10 overlap 自检；SIFT+M32 的 overlap 应 >90%。

### Step 7: 准备 Ground Truth → `gt*.bin`
```bash
# 用法: python3 scripts/gen_gt.py <base.fvecs> <query.fvecs> <gt输出.bin> [K]
python3 scripts/gen_gt.py data/sift_base.fvecs data/sift1m_query200.fvecs \
    data/sift1m_gt200.bin 10
```
对每个 query 在 base 里暴力精确 L2 算出真实 top-K（faiss `IndexFlatL2`），benchmark 用它算 recall。
- **GT 的邻居 id 是 base 原始行号**，与 graph/PQ 同一 id 空间。
- **生成时的 K 必须与运行 benchmark 时的 K 一致**（见下方 recall 陷阱）。
- 文件格式：8B header (`n_queries:u32 + K:u32`) + n×K 个 u64 邻居 id（仅 id，无距离）。

---

## 运行 Benchmark

### ⭐ 正式对比测试（唯一维护脚本，推荐）

```bash
# 默认: SIFT1M, cgroup 512M, 4 线程, k=10 ef=50 query=200, 3 轮取峰值
bash scripts/compare_benchmark.sh

# 可调参数（环境变量覆盖）
MEM=512M THREADS=8 K=10 EF=50 NQ=200 RUNS=5 bash scripts/compare_benchmark.sh
```

这个脚本同时跑两个对比，方便直观对比：
- **[A] DiskHNSW**：在 `cgroup MemoryMax=512M` 内存限制 + 多线程跑（磁盘向量搜索，省内存）
- **[B] hnswlib native**：放开 cgroup 限制，全内存基线（性能天花板）

公平对比三要素（脚本已内置）：
1. **充分 warmup**：page cache 预热 + benchmark 内部 CPU 升频 spin + 全 query 预跑一轮
2. **相同输入**：两者用完全一样的 query / GT / k / ef
3. **多轮取峰值**：排除 CPU 调频和后台抢占带来的抖动

> 参考结果（SIFT1M, 512M/4线程）：
> | 方案 | Recall | RSS | QPS |
> |------|--------|-----|-----|
> | DiskHNSW (512M/4线程) | 95.70% | 319 MB | ~580 |
> | hnswlib (全内存) | 95.25% | 726 MB | 数千~1万+ |
>
> 内存节省56%（319 vs 726 MB），recall 持平（甚至略高）。QPS 绝对值受当前机器 intel_pstate
> 对 I/O bound 任务的调频限制，以相对对比为准。

### 手动跑单个 benchmark（调试用）

#### 单线程（推荐配置）
```bash
TWO_STAGE=1 PQ_HYBRID=1 FINE_RERANK=1 FINE_BUFFERED=1 \
VEC_BLOCKS_PATH=output/sift1m_vecblocks_64k.bin FLAT_VEC_MB=64 \
REFINE_EF=100 CACHE_MB=32 \
PQ_CODES_PATH=output/pqco_sift1m_M32_correct.bin \
./build/benchmark_diskhnsw \
    output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_blocks_64k.bin output/sift1m_route_64k.bin \
    data/sift_base.fvecs data/sift1m_query200.fvecs data/sift1m_gt200.bin \
    10 50 200
```

参数含义：`K=10 EF=50 num_queries=200`

### 多线程并发
```bash
NUM_THREADS=4 FINE_PREAD=1 \
TWO_STAGE=1 PQ_HYBRID=1 FINE_RERANK=1 FINE_BUFFERED=1 \
VEC_BLOCKS_PATH=output/sift1m_vecblocks_64k.bin FLAT_VEC_MB=64 \
REFINE_EF=100 CACHE_MB=32 \
PQ_CODES_PATH=output/pqco_sift1m_M32_correct.bin \
./build/benchmark_diskhnsw \
    output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_blocks_64k.bin output/sift1m_route_64k.bin \
    data/sift_base.fvecs data/sift1m_query200.fvecs data/sift1m_gt200.bin \
    10 50 200
```

### 内存限制测试（cgroup）
```bash
systemd-run --user --scope -p MemoryMax=512M -p CPUQuota=400% \
    <上面的命令>
```

### ⚠️ recall 不对？先查这四个坑

跑出来 recall 很低（<90% 甚至 <1%），几乎都是数据准备环节不一致，不是算法问题：

| 现象 | 根因 | 修复 |
|------|------|------|
| recall ≈ **0%** | PQ 的 M 与维度不匹配（如 128 维用了 M=8）| 用 `train_pq.py <base> <out> 32` 重训（SIFT 用 M=32）|
| recall ≈ **0.x%** | graph 与 blocks/route/vecblocks 不配套（重跑了 Step 1-2 但没重跑 Step 4-5）| 同一批重新生成 Step 2 → Step 4/5 的全部文件 |
| recall 偏低且乱跳 | benchmark 运行的 `K` 与 GT 生成的 `K` 不一致 | `gen_gt.py` 生成时的 K 与命令行最后一个参数 K 保持相等 |
| recall 偏低 | base / query / GT 不是同一数据集 | 三者用同一 base；GT 用对应 query 重新 `gen_gt.py` |

> **read_gt 的行为**：benchmark 按运行时的 K 逐行读 GT，但 GT 文件 header 里存的是生成时的
> K。若两者不等，从第二行起 offset 就错位，recall 全错。**所以生成 K 与测试 K 必须一致。**

### 一键复现（推荐）
为避免上述坑，直接用脚本一次性生成全套配套文件：
```bash
# 用法: bash scripts/build_pipeline.sh <base.fvecs> <前缀> <M>
bash scripts/build_pipeline.sh data/sift_base.fvecs sift1m 32
# 产出 output/<前缀>_{index,graph,bfs,blocks_64k,route_64k,vecblocks_64k}.bin + pqco_<前缀>_M<M>.bin
```

---

## 环境变量参考

### 核心搜索模式

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `TWO_STAGE` | 0 | 1=PQ粗筛+精确精排两阶段搜索 | **开启后 recall 从 ~70% 升到 95.7%** |
| `PQ_CODES_PATH` | — | PQ 编码文件路径 | 不设则无 PQ，全精确距离（慢但 recall 高） |
| `PQ_HYBRID` | 0 | 1=cache 命中时用精确距离，miss 用 PQ ADC | 提升粗筛质量，recall +1-2% |
| `REFINE_EF` | 200 | 两阶段粗筛的 ef 值 | 越大 recall 越高但越慢，100 为甜点 |

### 精确精排（Fine Rerank）

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `FINE_RERANK` | 0 | 1=4KB 页粒度读向量做精确精排 | **核心优化，QPS 5x** |
| `FINE_BUFFERED` | 0 | 1=buffered I/O 吃 page cache（热区零 I/O） | **热态 QPS 2.5x**，冷态无差异 |
| `FINE_PREAD` | 0 | 1=pread 替代 io_uring（线程安全） | 多线程必须开启；单线程比 io_uring 慢 ~20% |
| `FINE_MERGE` | 0 | 1=相邻 4KB 页合并为 8KB 读 | 减少 syscall 数，实测收益不大 |
| `VEC_BLOCKS_PATH` | — | Vec-Only 块文件路径 | FINE_RERANK 必须设置 |
| `FLAT_VEC_MB` | 0 | 热向量 LRU cache 大小 (MB) | 64MB 覆盖 ~13% 向量，命中率影响 recall |

### 缓存与 I/O

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `CACHE_MB` | **必填** | BlockCache 大小 (MB) | 32MB 够用（PQ 模式下 block cache 命中率不关键） |

### 多线程

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `NUM_THREADS` | 0 | >0=并发搜索线程数 | 需配合 `FINE_PREAD=1`；4 线程 2.1x 加速 |
| `BATCH_SIZE` | 0 | >0=批处理 I/O 重叠模式 | 不走 TWO_STAGE 路径，recall 会掉 |

### 性能优化

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `PREFETCH_SW` | 1 | 0=关闭间接寻址软件预取 | 开启 QPS +8%；关闭用于对照实验 |

### 搜索模式（非 TWO_STAGE）

| 环境变量 | 默认值 | 说明 |
|----------|--------|------|
| `BEAM_WIDTH` | 0 | >0=beam search 宽度 |
| `NONBLOCK` | 0 | 1=非阻塞搜索 |
| `BATCH_IO_N` | 0 | >0=批量 I/O batch size |

### 调试

| 环境变量 | 说明 |
|----------|------|
| `PROFILE_TS` | 1=输出两阶段计时分解 (PhaseA / IOwait / rerank) |
| `PROFILE_FINE` | 1=输出 fine rerank 细粒度计时 |

---

## 架构概述

### 核心问题：为什么 hnswlib 需要 726MB？

hnswlib 把**全部 100 万条 128 维向量**加载到内存（488MB 向量 + 图结构 + 标签 ≈ 726MB）。搜索时每访问一个节点都要读它的向量算距离，全部在内存中完成，所以快（QPS 11800+），但内存是硬需求——512MB cgroup 下直接 OOM。

### DiskHNSW 的核心思路：把向量从内存挪到磁盘

关键洞察：HNSW 一次查询只访问约 5000 个节点（100万中的 0.5%），全量向量常驻内存意味着 99.5% 的内存浪费。但直接把向量放磁盘会导致每次邻居访问都等 I/O，延迟爆炸。

解法是**分层卸载 + 两阶段搜索**：
- 图结构（邻接表）常驻内存，保证图遍历零 I/O
- 向量用 PQ 压缩到 32 字节/条常驻内存，做粗筛（Phase A）
- 只对粗筛出的 ~100 个候选读真实向量做精排（Phase B），I/O 量从 488MB 降到 ~50KB/query

### 内存布局：留什么在内存，放什么到磁盘

```
┌─────────────────────────────────────────────────────────────┐
│                    常驻内存 (~272MB)                         │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐      │
│  │ 上层图+向量   │  │ L0 CSR邻接表  │  │ PQ Codes 30MB │      │
│  │ (63K, 30MB)  │  │ (84MB)       │  │ + Codebook    │      │
│  └──────────────┘  └──────────────┘  └───────────────┘      │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐      │
│  │ route_table  │  │ slot_table   │  │ labels+levels │      │
│  │ (4MB)        │  │ (2MB)        │  │ (12MB)        │      │
│  └──────────────┘  └──────────────┘  └───────────────┘      │
├─────────────────────────────────────────────────────────────┤
│              按需 I/O (page cache 热区)                      │
│  ┌──────────────┐  ┌──────────────┐                         │
│  │ VecBlocks    │  │ FlatVecCache │  ← FLAT_VEC_MB 控制     │
│  │ (磁盘, 496MB) │  │ (内存, 64MB) │                         │
│  └──────────────┘  └──────────────┘                         │
└─────────────────────────────────────────────────────────────┘
```

**常驻内存组件**（为什么必须在内存）：

| 组件 | 大小 | 作用 | 必须在内存的原因 |
|------|------|------|----------------|
| 上层节点向量 | 30MB | 63K 个 Layer 1+ 节点的向量 | 贪心下降每层查几个节点，需快速算距离 |
| L0 邻接表 CSR | 84MB | 2120万条边 | 搜索时频繁遍历邻居列表，不能等 I/O |
| PQ codes | 30MB | 100万 × 32字节 | Phase A 粗筛的压缩向量，替代真实向量 |
| 路由表 | 4MB | node_id -> block_id | 间接寻址，每次邻居访问都要查 |
| slot 表 | 2MB | node_id -> block内偏移 | Phase B 精排定位向量位置 |
| labels + levels | 12MB | 结果返回 + 层级判断 | 基础元数据 |

**磁盘上**（按需读取）：
- 全量向量数据 496MB，按 BFS 顺序打包成 64KB 块
- 只在 Phase B 精排时按 4KB 页粒度读取

### 搜索流程：两阶段搜索的每一步

```
查询到达
    │
    ├─ Step 1: 贪心下降 [纯内存, 零 I/O]
    │   上层图 + 上层向量 -> 找到 Layer 0 入口
    │
    ├─ Step 2: Phase A - PQ 粗筛 [纯内存, 零向量 I/O]
    │   │  CSR 邻接表 (84MB) -> 遍历邻居
    │   │  PQ codes (30MB) -> ADC 近似距离
    │   │  PQ dist table (预计算) -> SIMD 查表
    │   │  PQ_HYBRID: flat vec cache 命中 -> 精确 L2
    │   └─ 产出: top-100 候选 (PQ 近似排序)
    │
    ├─ Step 3: Phase B - 精确精排 [按需 I/O]
    │   │  候选 -> 检查 block cache / flat vec cache
    │   │  miss 候选 -> 收集 4KB 页号 -> 批量 io_uring/pread
    │   │  FINE_BUFFERED: page cache 热区零磁盘 I/O
    │   │  精确 L2 重排 -> top-K
    │   └─ 产出: 最终 top-K 结果
    │
    └─ 返回结果
```

#### Step 1: 贪心下降（纯内存）

从最高层逐层下降到 Layer 1，在每一层遍历当前节点的邻居，如果有更近的就移动过去。
完全在内存中操作上层向量和上层邻接表，零 I/O。

**目的**：为 Layer 0 搜索找到好的起点，避免从随机位置开始。

#### Step 2: Phase A - PQ 粗筛（零向量 I/O）

这是内存卸载的关键。Layer 0 搜索过程与标准 HNSW best-first 搜索完全一致（维护
candidate_set 最小堆 + top_candidates 最大堆），但**距离计算用 PQ 近似替代精确 L2**：

1. **`buildPqDistTable(query)`**：预计算 `[M][ksub]` 距离表，`table[m][k] = |query子向量m -
   中心点k|²`。AVX2 一次处理 2 个中心点，~16μs。
2. **`pqDistance()`**：查 PQ code（32 字节），32 次查表加法得到近似 L2。
   退化为查表后无需访问真实向量，**100 万向量压缩到 30MB 常驻内存**。
3. **`PQ_HYBRID=1`**：cache 命中的节点用精确 L2 替代 PQ，提升粗筛质量
   （recall 从 ~95.2% 升到 95.7%）。
4. **`PREFETCH_SW=1`**：`_mm_prefetch` pipeline 距离 6，消除间接寻址
   (route_table -> pq_codes -> flat_vec) 的 cache miss stall。

**产出**：`REFINE_EF=100` 个候选节点。

**为什么这是核心创新**：标准 HNSW 每展开一个节点都要读向量（512 字节），100万向量
= 488MB 内存。PQ 把每条向量压到 32 字节，30MB 常驻内存，图搜索过程零向量 I/O。
代价是距离不精确，由 Phase B 精排弥补。

#### Step 3: Phase B - 精确精排（按需 I/O）

Phase A 的 100 候选距离是 PQ 近似的，Phase B 读真实向量算精确 L2 重排出 top-K：

1. **检查 cache hit**：先看候选向量是否在 block cache 或 flat vec cache 中，命中则零 I/O。
2. **4KB 页粒度读**：miss 候选按 4KB 页对齐，收集页号，用 io_uring（单线程）或
   pread（多线程）批量提交。**I/O 量 = 100 × 512B ≈ 50KB**（而非 488MB 全量）。
3. **跨页向量处理**：SIFT 向量 512B，slot%8==6 时跨两个 4KB 页，有专门拼接逻辑。
4. **`FINE_BUFFERED=1`**：buffered I/O 吃 page cache，重复查询的候选页热态零磁盘 I/O
   （io_rest ≈ 1μs/query）。

**为什么 4KB 而非 64KB**：100 候选散布在 ~83 个 block，按 block 读要 5.3MB/query；
按 4KB 页读只需 ~470KB/query，**I/O 减 13 倍**。

### 为什么能卸载内存——逻辑链条

```
全量向量 488MB（内存装不下 @512M cgroup）
    ↓ PQ 压缩 (488MB -> 30MB)
PQ codes 常驻内存，替代向量做 Phase A 粗筛
    ↓ Phase A: PQ 粗筛零向量 I/O，选出 100 候选
    ↓ Phase B: 只读 100 个候选的真实向量 (4KB 页粒度)
    ↓ I/O 量 = 100 × 512B ≈ 50KB/query（而非 488MB）
    ↓ page cache 热区：第二次查询命中率 >99%
-> 512MB cgroup 下 recall 95.70%，hnswlib 726MB 直接 OOM
```

### 优化措施在链条中的位置

| 优化 | 阶段 | 解决的瓶颈 | 机制 | QPS 提升 |
|------|------|-----------|------|---------|
| BFS 重排 | 数据准备 | cache 命中率低 | 图上相邻节点物理相邻，命中率 86.8% | 基线 |
| PQ 粗筛 | Phase A | 向量不在内存 | 32B 近似距离替代 512B 精确距离 | 53->595 |
| PQ_HYBRID | Phase A | PQ 近似精度差 | cache 命中用精确 L2，miss 用 PQ | recall +1% |
| SIMD PQ LUT | Phase A | PQ 距离计算慢 | 预计算 [32][256] 表 + AVX2 查表 | 2141->2643 |
| FINE_RERANK | Phase B | 精排 I/O 量太大 | 4KB 页粒度读替代 64KB block 读 | 595->867 |
| FINE_BUFFERED | Phase B | 磁盘 I/O 延迟 | page cache 热区，重复查询零磁盘 I/O | 867->2141 |
| SW 预取 | Phase A+B | 间接寻址 cache miss | `_mm_prefetch` pipeline 距离 6 | 2570->2780 |
| 多线程 | 整体 | 单线程吞吐不够 | 4 线程 pread 并发，thread_local PQ table | 2780->5808 |

### 为什么 recall 能反超 hnswlib

hnswlib 全内存 recall 95.25%，DiskHNSW 95.70%，反超 0.45%：

1. **两阶段搜索本身就是 recall 提升手段**：Phase A 用大 ef（100 vs hnswlib 的 50）做粗筛，
   Phase B 精排。等价于 hnswlib 用 ef=100 搜索但只返回 top-10，自然 recall 更高。
2. **PQ_HYBRID 让粗筛更准**：cache 命中节点用精确距离，减少 PQ 近似误判。
3. **代价是速度**：QPS 6322 vs hnswlib 11862，慢 ~1.9x，但用一半内存换来。

### 数据文件说明

| 文件 | 格式 | 内容 |
|------|------|------|
| `*_graph.bin` | HARG | 图结构：节点层级 + 上层向量 + labels + L0邻接表 |
| `*_bfs.bin` | BFS | BFS 重排映射：old_id ↔ new_id |
| `*_blocks_64k.bin` | HKLB | 64KB 块文件（含邻接表，BlockCache 用） |
| `*_route_64k.bin` | ROUTE | 节点->块映射表 |
| `*_vecblocks_64k.bin` | HKLB | Vec-Only 块文件（仅向量，FINE_RERANK 用） |
| `*_vecblocks_64k_route.bin` | ROUTE | Vec-Only 的节点->块映射（**必须与 vecblocks 配套**） |
| `pqco_*.bin` | PQCO/OPQ1 | PQ 编码：codebook + codes |

> ⚠️ **重要**：vecblocks 文件和它的 route table 必须配套生成（同一次 `write_blocks_veconly` 调用）。混用不同版本的文件会导致 offset 错误、recall 暴跌。

---

## 项目结构

```
hnsw-predictor/
├── include/               # 公共头文件
│   ├── disk_hnsw.h        #   DiskHNSW 搜索引擎
│   ├── block_cache.h      #   LRU 块缓存
│   ├── graph_prefetcher.h #   io_uring 异步预取
│   ├── io_uring_wrapper.h #   io_uring C++ 封装
│   ├── common.h           #   图结构定义 + IO 工具
│   ├── block_heat_evaluator.h # 块热度评估
│   ├── layout_provider.h  #   块布局策略 (BFS/Linear)
│   └── replacement_policy.h #  缓存替换策略 (LRU)
├── src/
│   ├── core/              # 核心库
│   │   ├── disk_hnsw.cpp  #   DiskHNSW 实现（搜索、PQ、精排）
│   │   ├── block_cache.cpp#   BlockCache 实现
│   │   └── graph_prefetcher.cpp # 图引导预取
│   ├── pipeline/          # 索引构建工具 (Step 1-5)
│   │   ├── build_index.cpp        # HNSW 建图
│   │   ├── extract_graph.cpp      # 提取图结构
│   │   ├── bfs_reorder.cpp        # BFS 重排
│   │   ├── write_blocks_veconly.cpp # Vec-Only 块生成
│   │   ├── write_blocks.cpp       # 块生成（含邻接表）
│   │   ├── write_pq_blocks.cpp    # PQ 块生成
│   │   ├── gen_route.cpp          # 路由表生成
│   │   └── verify.cpp             # 数据验证
│   ├── benchmark/         # 基准测试
│   │   ├── benchmark_diskhnsw.cpp #   DiskHNSW 主 benchmark
│   │   └── benchmark_hnswlib_native.cpp # hnswlib 全内存对比基线
│   └── test/              # 单元测试
│       ├── test_disk_hnsw.cpp
│       ├── test_block_cache.cpp
│       └── test_pq_search_quality.cpp
├── scripts/               # Python / Shell 脚本
│   ├── train_pq.py        #   PQ 训练（faiss, 支持自定义 M）
│   ├── gen_gt.py          #   Ground Truth 生成（暴力精确 top-K）
│   ├── build_pipeline.sh  #   一键跑完整 Step 1-7
│   └── convert_hdf5_to_fvecs.py # ann-benchmarks HDF5 → fvecs
├── data/                  # 小样例 query/GT（大 base 需自备, 见数据准备）
├── hnswlib/               # HNSW 库（外部依赖）
└── Makefile
```

---

## 已知限制与注意事项

1. **vecblocks 与 route table 必须配套**：混用不同版本的文件会导致 offset 错误
2. **io_uring 非线程安全**：多线程必须设置 `FINE_PREAD=1`
3. **fio 测试不要指向数据文件**：fio 会覆写文件内容（惨痛教训）
4. **性能异常先查 CPU 频率**：`grep MHz /proc/cpuinfo`，热保护降频会让所有测量慢 2.5x
5. **OPQ 旋转对 SIFT 无效**：SIFT 直方图特征各维已均衡，M=32/dsub=4 下 OPQ 重建误差反而更大
6. **GT 文件有 8B header**：`n_queries(u32) + k(u32)`，读取时需跳过
