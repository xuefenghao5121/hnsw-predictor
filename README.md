# DiskHNSW - 内存受限环境下的磁盘向量搜索

> **SIFT1M**: 512MB cgroup, 95.70% recall / 5800+ QPS (4T), hnswlib 需 726MB OOM
> **DEEP10M**: 2GB cgroup, 95.15% recall / 2340 QPS (12T), hnswlib 需 ~6GB OOM

## 项目背景

传统 HNSW 向量搜索需要将全部向量加载到内存。对于 1M 条 128 维 SIFT 向量，hnswlib 需 726MB RSS；对于 10M 条 96 维 DEEP 向量，需 ~6GB。在容器化、边缘计算等内存受限场景下，这不可行。

DiskHNSW 的核心思路:

1. **图结构常驻内存**(上层节点向量 + L0 邻接表 CSR,~272MB),保证贪心下降和图遍历零 I/O
2. **向量数据存磁盘**,按 BFS 重排后分块,利用空间局部性
3. **PQ 粗筛**:用 Product Quantization 近似距离做图搜索,无需读磁盘向量
4. **精确精排**(Fine Rerank):对 PQ 粗筛的候选集,按 4KB 页粒度读取向量做精确 L2 距离
5. **I/O 优化**:io_uring 异步 I/O + page cache 热区 + 软件预取消除间接寻址 cache miss

### 性能弧线(SIFT1M, 512MB cgroup, 单线程)

```
QPS: 53 → 595 → 867 → 2141 → 2643 → 2780    (52x 提升)
RSS: 337MB → ... → 301MB → 269MB   (CSR 压缩后↓ 32MB)
```

| 里程碑 | QPS | Recall | RSS | 技术 |
|--------|-----|--------|-----|------|
| 基线 | 53 | 95.7% | 337MB | 64KB 块同步读 |
| FINE_RERANK | 867 | 95.7% | 337MB | 4KB 页粒度精排,I/O 减 16x |
| FINE_BUFFERED | 2141 | 95.7% | 337MB | page cache 热区零 I/O |
| SIMD PQ LUT | 2643 | 95.7% | 337MB | AVX2 距离表预计算 |
| SW 预取 | 2780 | 95.7% | 337MB | 间接寻址 pipeline 预取 |
| **CSR Delta+Varint (P0)** | **2092** | **95.7%** | **269MB** | 邻接表 varint 压缩 1.8x |
| **FINE_RERANK bug 修复 (P0.5)** | 2067 | 95.7% | 269MB | 双路由表分离,不再依赖隐式对齐 |
| 4线程并发 | **5808** | 95.7% | 286MB | pread 多线程 + thread_local PQ table |

### 对比 hnswlib

| 指标 | hnswlib(全内存) | DiskHNSW(512MB cgroup) |
|------|-----------------|------------------------|
| Recall | 95.25% | **95.70%**(反超) |
| QPS (1T) | 12420 | 2780 |
| QPS (4T) | - | **5808** |
| RSS | 726MB(OOM@512MB) | **269MB** |
| 内存节省 | - | **2.7x** |

### P2: DEEP10M 规模验证 (10M 向量)

| 指标 | hnswlib(全内存) | DiskHNSW(2GB cgroup) |
|------|-----------------|---------------------|
| Recall | 95.60% (ef=400) | **95.15%** (EF=300) |
| QPS (1T) | 1557 | 590 |
| QPS (12T) | - | **2340** |
| RSS | ~6GB (OOM@2GB) | **1612MB** |
| 内存节省 | - | **3.7x** |

> P2 关键发现：10M 规模瓶颈从 I/O 转移到 PQ 计算(80%)。VisitedList uint32->uint8
> 消除内存分配瓶颈，带来 2x QPS 提升。io_uring fine rerank 在候选数>200 时丢结果，
> 改用 pread 路径修复。1GB cgroup 不可行(CSR 591MB + PQ codes 304MB + upper vecs 228MB
> = 1.1GB 核心数据)，2GB cgroup 下 hnswlib OOM、DiskHNSW 正常运行。

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

> Makefile 的 `all` 已包含 `benchmark_diskhnsw` 和 `write_blocks_veconly`,无需再手动编译。

---

## 数据准备 Pipeline

以 SIFT1M(128 维,100 万向量)为例。**7 个步骤有严格的先后依赖**,下面每步都说明它
产出什么、下一步为什么需要它。

> ### ⚠️⚠️ 三条铁律(不遵守 recall 直接崩到 <1%)
>
> 1. **一套数据从头到尾**:`build_index`、`train_pq.py`、`gen_gt.py` 必须用**同一个**
>    base.fvecs。graph 的 node id、PQ codes 的行号、GT 的邻居 id 是**同一个 id 空间**
>    (base 向量的原始行号)。换了 base 数据其中一环,三者对不上,recall 归零。
> 2. **graph 与 blocks/route/vecblocks 必须同批生成**:Step 4/5 的块文件是从 Step 2 的
>    graph 算出 offset 的。**重新跑 Step 1-2 覆盖了 graph,就必须重跑 Step 4-5**,否则
>    offset 错位,recall 崩。(不要把新旧 output 目录的文件混着用。)
> 3. **PQ 的 M 要匹配维度**:SIFT dim=128 用 **M=32**;Deep dim=96 用 M=8。M 用错
>    (比如 128 维用了 M=8)PQ 粗筛精度极差,recall≈0。

### Step 1: 构建 HNSW 图 → `index.bin`
```bash
./build/build_index data/sift_base.fvecs output/sift1m_index.bin 16 200
```
产出 hnswlib 原生索引(含图 + 全部向量)。参数 `M=16 ef_construction=200`。
**下一步要从中剥离出精简图结构。**

### Step 2: 提取图结构 → `graph.bin`
```bash
./build/extract_graph output/sift1m_index.bin output/sift1m_graph.bin 128
```
从 index 剥离出 DiskHNSW 用的图(节点层级 + 上层向量 + labels + L0 邻接表 CSR),
丢掉 L0 全量向量。**这是后面所有块文件的 offset 基准,一旦重生成,Step 4/5 必须跟着重跑。**

### Step 3: BFS 重排 → `bfs.bin`
```bash
./build/bfs_reorder output/sift1m_graph.bin output/sift1m_bfs.bin
```
按图的 BFS 遍历顺序给节点重新编号,让图上相邻的节点在磁盘上也相邻(空间局部性),
提升块内命中率。**产出 old_id↔new_id 映射,Step 4/5 按这个顺序摆放向量。**

### Step 4: 生成向量块文件(Vec-Only)→ `vecblocks_64k.bin` (+ `_route.bin`)
```bash
./build/write_blocks_veconly output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_vecblocks_64k.bin 65536
# 同时生成配套 route: output/sift1m_vecblocks_64k_route.bin
```
把向量按 BFS 顺序打包成 64KB 块(**只存向量,不含邻接表**),供 Phase B 精排按 4KB 页
粒度读取。**route 表和块文件必须同一次调用产出,配套使用。**

### Step 5: 生成旧格式块文件(BlockCache 用)→ `blocks_64k.bin` + `route_64k.bin`
```bash
./build/write_blocks output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_blocks_64k.bin 65536
./build/gen_route output/sift1m_blocks_64k.bin output/sift1m_route_64k.bin
```
生成含邻接表的 64KB 块(BlockCache/fallback 路径用)及其路由表。

### Step 6: 训练 PQ 编码 → `pqco_*.bin`
```bash
# 用法: python3 scripts/train_pq.py <base.fvecs> <输出.bin> [M]
python3 scripts/train_pq.py data/sift_base.fvecs output/pqco_sift1m_M32_correct.bin 32
```
用 faiss 把 base 向量压缩成 PQ codes(codebook + 每向量 M 字节),供 Phase A 粗筛
做零 I/O 的 ADC 近似距离。
- **M 可通过第 3 个参数自定义**;缺省时按维度自动选(128→32, 96→8)。
- **必须用与 Step 1 相同的 base 数据**(PQ codes 按 base 原始行号编码,与 graph node id 对齐)。
- 脚本末尾打印重建 MSE 和 ADC top-10 overlap 自检;SIFT+M32 的 overlap 应 >90%。

### Step 7: 准备 Ground Truth → `gt*.bin`
```bash
# 用法: python3 scripts/gen_gt.py <base.fvecs> <query.fvecs> <gt输出.bin> [K]
python3 scripts/gen_gt.py data/sift_base.fvecs data/sift1m_query200.fvecs \
    data/sift1m_gt200.bin 10
```
对每个 query 在 base 里暴力精确 L2 算出真实 top-K(faiss `IndexFlatL2`),benchmark 用它算 recall。
- **GT 的邻居 id 是 base 原始行号**,与 graph/PQ 同一 id 空间。
- **生成时的 K 必须与运行 benchmark 时的 K 一致**(见下方 recall 陷阱)。
- 文件格式:8B header (`n_queries:u32 + K:u32`) + n×K 个 u64 邻居 id(仅 id,无距离)。

---

## 运行 Benchmark

### ⭐ 正式对比测试(唯一维护脚本,推荐)

```bash
# 默认: SIFT1M, cgroup 512M, 4 线程, k=10 ef=50 query=200, 3 轮取峰值
bash scripts/compare_benchmark.sh

# 可调参数(环境变量覆盖)
MEM=512M THREADS=8 K=10 EF=50 NQ=200 RUNS=5 bash scripts/compare_benchmark.sh
```

这个脚本同时跑两个对比,方便直观对比:
- **[A] DiskHNSW**:在 `cgroup MemoryMax=512M` 内存限制 + 多线程跑(磁盘向量搜索,省内存)
- **[B] hnswlib native**:放开 cgroup 限制,全内存基线(性能天花板)

公平对比三要素(脚本已内置):
1. **充分 warmup**:page cache 预热 + benchmark 内部 CPU 升频 spin + 全 query 预跑一轮
2. **相同输入**:两者用完全一样的 query / GT / k / ef
3. **多轮取峰值**:排除 CPU 调频和后台抢占带来的抖动

> 参考结果(SIFT1M, 512M/4线程):
> | 方案 | Recall | RSS | QPS |
> |------|--------|-----|-----|
> | DiskHNSW (512M/4线程) | 95.70% | 319 MB | ~580 |
> | hnswlib (全内存) | 95.25% | 726 MB | 数千~1万+ |
>
> 内存节省56%(319 vs 726 MB),recall 持平(甚至略高)。QPS 绝对值受当前机器 intel_pstate
> 对 I/O bound 任务的调频限制,以相对对比为准。

### 手动跑单个 benchmark(调试用)

#### 单线程(推荐配置)
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

参数含义:`K=10 EF=50 num_queries=200`

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

### 内存限制测试(cgroup)
```bash
systemd-run --user --scope -p MemoryMax=512M -p CPUQuota=400% \
    <上面的命令>
```

### ⚠️ recall 不对?先查这四个坑

跑出来 recall 很低(<90% 甚至 <1%),几乎都是数据准备环节不一致,不是算法问题:

| 现象 | 根因 | 修复 |
|------|------|------|
| recall ≈ **0%** | PQ 的 M 与维度不匹配(如 128 维用了 M=8)| 用 `train_pq.py <base> <out> 32` 重训(SIFT 用 M=32)|
| recall ≈ **0.x%** | graph 与 blocks/route/vecblocks 不配套(重跑了 Step 1-2 但没重跑 Step 4-5)| 同一批重新生成 Step 2 → Step 4/5 的全部文件 |
| recall 偏低且乱跳 | benchmark 运行的 `K` 与 GT 生成的 `K` 不一致 | `gen_gt.py` 生成时的 K 与命令行最后一个参数 K 保持相等 |
| recall 偏低 | base / query / GT 不是同一数据集 | 三者用同一 base;GT 用对应 query 重新 `gen_gt.py` |

> **read_gt 的行为**:benchmark 按运行时的 K 逐行读 GT,但 GT 文件 header 里存的是生成时的
> K。若两者不等,从第二行起 offset 就错位,recall 全错。**所以生成 K 与测试 K 必须一致。**

### 一键复现(推荐)
为避免上述坑,直接用脚本一次性生成全套配套文件:
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
| `PQ_CODES_PATH` | - | PQ 编码文件路径 | 不设则无 PQ,全精确距离(慢但 recall 高) |
| `PQ_HYBRID` | 0 | 1=cache 命中时用精确距离,miss 用 PQ ADC | 提升粗筛质量,recall +1-2% |
| `REFINE_EF` | 200 | 两阶段粗筛的 ef 值 | 越大 recall 越高但越慢,100 为甜点 |

### 精确精排(Fine Rerank)

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `FINE_RERANK` | 0 | 1=4KB 页粒度读向量做精确精排 | **核心优化,QPS 5x** |
| `FINE_BUFFERED` | 0 | 1=buffered I/O 吃 page cache(热区零 I/O) | **热态 QPS 2.5x**,冷态无差异 |
| `FINE_PREAD` | 0 | 1=pread 替代 io_uring(线程安全) | 多线程必须开启;单线程比 io_uring 慢 ~20% |
| `FINE_MERGE` | 0 | 1=相邻 4KB 页合并为 8KB 读 | 减少 syscall 数,实测收益不大 |
| `VEC_BLOCKS_PATH` | - | Vec-Only 块文件路径 | FINE_RERANK 必须设置 |
| `FLAT_VEC_MB` | 0 | 热向量 LRU cache 大小 (MB) | 64MB 覆盖 ~13% 向量,命中率影响 recall |

### 缓存与 I/O

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `CACHE_MB` | **必填** | BlockCache 大小 (MB) | 32MB 够用(PQ 模式下 block cache 命中率不关键) |

### 多线程

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `NUM_THREADS` | 0 | >0=并发搜索线程数 | 需配合 `FINE_PREAD=1`;4 线程 2.1x 加速 |
| `BATCH_SIZE` | 0 | >0=批处理 I/O 重叠模式 | 不走 TWO_STAGE 路径,recall 会掉 |

### 性能优化

| 环境变量 | 默认值 | 说明 | 影响 |
|----------|--------|------|------|
| `PREFETCH_SW` | 1 | 0=关闭间接寻址软件预取 | 开启 QPS +8%;关闭用于对照实验 |

### 搜索模式(非 TWO_STAGE)

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

### 核心问题:为什么 hnswlib 需要 726MB?

hnswlib 把**全部 100 万条 128 维向量**加载到内存(488MB 向量 + 图结构 + 标签 ≈ 726MB)。搜索时每访问一个节点都要读它的向量算距离,全部在内存中完成,所以快(QPS 11800+),但内存是硬需求--512MB cgroup 下直接 OOM。

### DiskHNSW 的核心思路:把向量从内存挪到磁盘

关键洞察:HNSW 一次查询只访问约 5000 个节点(100万中的 0.5%),全量向量常驻内存意味着 99.5% 的内存浪费。但直接把向量放磁盘会导致每次邻居访问都等 I/O,延迟爆炸。

解法是**分层卸载 + 两阶段搜索**:
- 图结构(邻接表)常驻内存,保证图遍历零 I/O
- 向量用 PQ 压缩到 32 字节/条常驻内存,做粗筛(Phase A)
- 只对粗筛出的 ~100 个候选读真实向量做精排(Phase B),I/O 量从 488MB 降到 ~50KB/query

### 内存布局:留什么在内存,放什么到磁盘

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

**常驻内存组件**(为什么必须在内存):

| 组件 | 大小 | 作用 | 必须在内存的原因 |
|------|------|------|----------------|
| 上层节点向量 | 30MB | 63K 个 Layer 1+ 节点的向量 | 贪心下降每层查几个节点,需快速算距离 |
| L0 邻接表 CSR | 84MB | 2120万条边 | 搜索时频繁遍历邻居列表,不能等 I/O |
| PQ codes | 30MB | 100万 × 32字节 | Phase A 粗筛的压缩向量,替代真实向量 |
| 路由表 | 4MB | node_id -> block_id | 间接寻址,每次邻居访问都要查 |
| slot 表 | 2MB | node_id -> block内偏移 | Phase B 精排定位向量位置 |
| labels + levels | 12MB | 结果返回 + 层级判断 | 基础元数据 |

**磁盘上**(按需读取):
- 全量向量数据 496MB,按 BFS 顺序打包成 64KB 块
- 只在 Phase B 精排时按 4KB 页粒度读取

### 搜索流程:两阶段搜索的每一步

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

#### Step 1: 贪心下降(纯内存)

从最高层逐层下降到 Layer 1,在每一层遍历当前节点的邻居,如果有更近的就移动过去。
完全在内存中操作上层向量和上层邻接表,零 I/O。

**目的**:为 Layer 0 搜索找到好的起点,避免从随机位置开始。

#### Step 2: Phase A - PQ 粗筛(零向量 I/O)

这是内存卸载的关键。Layer 0 搜索过程与标准 HNSW best-first 搜索完全一致(维护
candidate_set 最小堆 + top_candidates 最大堆),但**距离计算用 PQ 近似替代精确 L2**:

1. **`buildPqDistTable(query)`**:预计算 `[M][ksub]` 距离表,`table[m][k] = |query子向量m -
   中心点k|2`。AVX2 一次处理 2 个中心点,~16μs。
2. **`pqDistance()`**:查 PQ code(32 字节),32 次查表加法得到近似 L2。
   退化为查表后无需访问真实向量,**100 万向量压缩到 30MB 常驻内存**。
3. **`PQ_HYBRID=1`**:cache 命中的节点用精确 L2 替代 PQ,提升粗筛质量
   (recall 从 ~95.2% 升到 95.7%)。
4. **`PREFETCH_SW=1`**:`_mm_prefetch` pipeline 距离 6,消除间接寻址
   (route_table -> pq_codes -> flat_vec) 的 cache miss stall。

**产出**:`REFINE_EF=100` 个候选节点。

**为什么这是核心创新**:标准 HNSW 每展开一个节点都要读向量(512 字节),100万向量
= 488MB 内存。PQ 把每条向量压到 32 字节,30MB 常驻内存,图搜索过程零向量 I/O。
代价是距离不精确,由 Phase B 精排弥补。

#### Step 3: Phase B - 精确精排(按需 I/O)

Phase A 的 100 候选距离是 PQ 近似的,Phase B 读真实向量算精确 L2 重排出 top-K:

1. **检查 cache hit**:先看候选向量是否在 block cache 或 flat vec cache 中,命中则零 I/O。
2. **4KB 页粒度读**:miss 候选按 4KB 页对齐,收集页号,用 io_uring(单线程)或
   pread(多线程)批量提交。**I/O 量 = 100 × 512B ≈ 50KB**(而非 488MB 全量)。
3. **跨页向量处理**:SIFT 向量 512B,slot%8==6 时跨两个 4KB 页,有专门拼接逻辑。
4. **`FINE_BUFFERED=1`**:buffered I/O 吃 page cache,重复查询的候选页热态零磁盘 I/O
   (io_rest ≈ 1μs/query)。

**为什么 4KB 而非 64KB**:100 候选散布在 ~83 个 block,按 block 读要 5.3MB/query;
按 4KB 页读只需 ~470KB/query,**I/O 减 13 倍**。

### 为什么能卸载内存--逻辑链条

```
全量向量 488MB(内存装不下 @512M cgroup)
    ↓ PQ 压缩 (488MB -> 30MB)
PQ codes 常驻内存,替代向量做 Phase A 粗筛
    ↓ Phase A: PQ 粗筛零向量 I/O,选出 100 候选
    ↓ Phase B: 只读 100 个候选的真实向量 (4KB 页粒度)
    ↓ I/O 量 = 100 × 512B ≈ 50KB/query(而非 488MB)
    ↓ page cache 热区:第二次查询命中率 >99%
-> 512MB cgroup 下 recall 95.70%,hnswlib 726MB 直接 OOM
```

### 优化措施在链条中的位置

| 优化 | 阶段 | 解决的瓶颈 | 机制 | QPS 提升 |
|------|------|-----------|------|---------|
| BFS 重排 | 数据准备 | cache 命中率低 | 图上相邻节点物理相邻,命中率 86.8% | 基线 |
| PQ 粗筛 | Phase A | 向量不在内存 | 32B 近似距离替代 512B 精确距离 | 53->595 |
| PQ_HYBRID | Phase A | PQ 近似精度差 | cache 命中用精确 L2,miss 用 PQ | recall +1% |
| SIMD PQ LUT | Phase A | PQ 距离计算慢 | 预计算 [32][256] 表 + AVX2 查表 | 2141->2643 |
| FINE_RERANK | Phase B | 精排 I/O 量太大 | 4KB 页粒度读替代 64KB block 读 | 595->867 |
| FINE_BUFFERED | Phase B | 磁盘 I/O 延迟 | page cache 热区,重复查询零磁盘 I/O | 867->2141 |
| SW 预取 | Phase A+B | 间接寻址 cache miss | `_mm_prefetch` pipeline 距离 6 | 2570->2780 |
| 多线程 | 整体 | 单线程吞吐不够 | 4 线程 pread 并发,thread_local PQ table | 2780->5808 |
| CSR Delta+Varint | 数据结构 | CSR 邻接表内存占用大 | delta+varint 压缩,84->47MB (1.8x) | RSS -32MB,QPS -4% |

### 为什么 recall 能反超 hnswlib

hnswlib 全内存 recall 95.25%,DiskHNSW 95.70%,反超 0.45%:

1. **两阶段搜索本身就是 recall 提升手段**:Phase A 用大 ef(100 vs hnswlib 的 50)做粗筛,
   Phase B 精排。等价于 hnswlib 用 ef=100 搜索但只返回 top-10,自然 recall 更高。
2. **PQ_HYBRID 让粗筛更准**:cache 命中节点用精确距离,减少 PQ 近似误判。
3. **代价是速度**:QPS 6322 vs hnswlib 11862,慢 ~1.9x,但用一半内存换来。

### 数据文件说明

| 文件 | 格式 | 内容 |
|------|------|------|
| `*_graph.bin` | HARG | 图结构:节点层级 + 上层向量 + labels + L0邻接表 |
| `*_bfs.bin` | BFS | BFS 重排映射:old_id ↔ new_id |
| `*_blocks_64k.bin` | HKLB | 64KB 块文件(含邻接表,BlockCache 用) |
| `*_route_64k.bin` | ROUTE | 节点->块映射表 |
| `*_vecblocks_64k.bin` | HKLB | Vec-Only 块文件(仅向量,FINE_RERANK 用) |
| `*_vecblocks_64k_route.bin` | ROUTE | Vec-Only 的节点->块映射(**必须与 vecblocks 配套**) |
| `pqco_*.bin` | PQCO/OPQ1 | PQ 编码:codebook + codes |

> ⚠️ **重要**:vecblocks 文件和它的 route table 必须配套生成(同一次 `write_blocks_veconly` 调用)。混用不同版本的文件会导致 offset 错误、recall 暴跌。

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
│   │   ├── disk_hnsw.cpp  #   DiskHNSW 实现(搜索、PQ、精排)
│   │   ├── block_cache.cpp#   BlockCache 实现
│   │   └── graph_prefetcher.cpp # 图引导预取
│   ├── pipeline/          # 索引构建工具 (Step 1-5)
│   │   ├── build_index.cpp        # HNSW 建图
│   │   ├── extract_graph.cpp      # 提取图结构
│   │   ├── bfs_reorder.cpp        # BFS 重排
│   │   ├── write_blocks_veconly.cpp # Vec-Only 块生成
│   │   ├── write_blocks.cpp       # 块生成(含邻接表)
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
│   ├── train_pq.py        #   PQ 训练(faiss, 支持自定义 M)
│   ├── gen_gt.py          #   Ground Truth 生成(暴力精确 top-K)
│   ├── build_pipeline.sh  #   一键跑完整 Step 1-7
│   └── convert_hdf5_to_fvecs.py # ann-benchmarks HDF5 → fvecs
├── data/                  # 小样例 query/GT(大 base 需自备, 见数据准备)
├── hnswlib/               # HNSW 库(外部依赖)
└── Makefile
```

---

## 优化日志

记录已完成的优化措施,供后续开发者参考。每条记录包含:动机、做法、效果、代价。

### P0: CSR Delta+Varint 压缩 (2026-07-28)

**动机**:随着图规模增长(10M/100M),L0 CSR 邻接表线性增长(1M=84MB → 10M=888MB →
100M=8.9GB),成为内存瓶颈。需要先压缩 CSR 延缓上磁盘的时间点。

**数据分析**:对 SIFT1M BFS-ordered CSR 的 delta 分布分析发现:
- 4.2% 的 delta=1,32% 的 delta<1024(局部边)
- 68% 的 delta≥1024(长程边,HNSW small-world 特性决定)
- BVGraph reference 压缩不适用(BFS 相邻节点 Jaccard 仅 0.023,几乎无重复邻接表)
- 纯 delta+varint 理论压缩比 1.7x

**做法**:
1. `adj_csr_neighbors_`(`vector<uint32_t>`,4 bytes/edge)改为 `adj_csr_compact_`
   (`vector<uint8_t>`,delta+varint 编码的字节流)
2. `adj_csr_offsets_`(节点->edge offset)改为 `adj_csr_byte_offsets_`(节点->byte offset)
3. `getInMemNeighbors()` 从直接返回指针改为解码到 `thread_local` buffer
4. 解码开销:~100-200ns/节点(21 个 delta 的 varint 解码),图搜索约 5000 节点/query
5. 保留 `adj_csr_offsets_`(uint32 N+1)用于快速查度数,不再存 neighbors 数组

**代码变更**:
- `include/disk_hnsw.h`:新增 `adj_csr_compact_`、`adj_csr_byte_offsets_`、`csr_compressed_`、
  `csr_decode_buf_` (thread_local) 成员;`getInMemNeighbors` 去掉 const
- `src/core/disk_hnsw.cpp`:重写 `buildInMemoryAdjacency()` 做 delta+varint 编码;
  新增 `decodeCsrNeighbors()` 解码方法;`getInMemNeighbors()` 分压缩/非压缩两条路径
- `common.h` 中已有 `varint_encode/decode` 和 `delta_varint_encode/decode` 函数,直接复用

**效果** (SIFT1M, 512MB cgroup, 标准 200 queries, K=10, EF=50):

| 指标 | 改造前 | 改造后 | 变化 |
|------|--------|--------|------|
| CSR 内存 | 84 MB | 47 MB | **-37 MB, 1.8x 压缩** |
| 单线程 RSS | 301 MB | 269 MB | **-32 MB** |
| 单线程 QPS | 2177 | 2092 | -3.9%(解码开销) |
| 单线程 Recall | 95.70% | 95.70% | ✅ 不变 |
| 4线程 RSS | 319 MB | 286 MB | **-33 MB** |
| 4线程 QPS | 6322 | 6070 | -4.0% |
| 4线程 Recall | 95.70% | 95.70% | ✅ 不变 |

**代价**:QPS 下降 ~4%(每查询多 ~1ms 解码时间),换取 32MB 内存节省。

**规模推演**:

| 规模 | CSR 原始 | CSR 压缩后 | 压缩后总常驻(估) | 512M 可行? |
|------|---------|-----------|-------------------|------------|
| 1M | 84 MB | 47 MB | 269 MB | ✅ || 10M | 888 MB | ~470 MB | ~1.2 GB | ❌(1GB cgroup 可行) |
| 100M | 8.9 GB | ~4.7 GB | ~8 GB | ❌(需磁盘 I/O) |

**结论**:CSR 压缩买入一个数量级的延展(1M→10M 窗口扩大),但 100M 级别仍需 CSR 上磁盘。

**回退方法**:`git reset --hard backup-before-csr-compression`

**后续方向**:
- P1: 图裁剪 (Graph Pruning, R=12) -- 边数减半,与 varint 叠加可达 3.4x 压缩
- P2: 10M 规模验证
- P3: CSR 上磁盘 + I/O 掩盖(参考向量卸载的 1-hop 预取模式)

---

### P0.5: FINE_RERANK 隐藏 Bug 修复 (2026-07-28)

**背景**:P1 图裁剪实验中,任何图裁剪(甚至零裁剪往返:load+save 不改任何边)都会让 recall 从 95.70% 崩到 ~10%。经过隔离测试(PQ-only 95.70%、纯 block cache 95.30%),定位到 bug 在 FINE_RERANK 精排路径。

**根因**:
- 系统里有两套块文件:`blocks_64k.bin`(含邻接表,8651 块)和 `vecblocks_64k.bin`(仅向量,7937 块)
- 两文件因元数据大小不同,**同一个 node 在两文件里的 block_id 不一样**
- FINE_RERANK 代码用**blocks 文件的 route_table**(node → 8651-块 block_id)去索引 **vecblocks 文件的偏移**:
  ```cpp
  uint32_t b = route_table_[nid];   // ← blocks 文件的 block_id
  uint64_t off = 4096 + b * vec_block_size_
               + block_data_offset_[b]  // ← vecblocks 文件的偏移表
               + node_slot_table_[nid] * dim * 4;
  ```
  用错误的 block_id 索引 vecblocks 数据,读到的是完全不相关的向量
- **为什么之前跑通了**:原始数据文件的 block 结构碰巧让部分 offset 命中真实向量;重新跑 pipeline 后 block 边界移动,bug 暴露

**修复**(`disk_hnsw.h` + `disk_hnsw.cpp`):
1. 新增 `vec_route_table_`(node → vecblocks_block_id),在 `buildFineRerank()` 扫描每个 vecblocks 块时构建
2. FINE_RERANK 精排路径用 `vec_route_table_` 计算 vecblocks 偏移,用原 `route_table_` 查 block cache

```cpp
uint32_t b = vec_route_table_[nid];                  // ← 修复: vecblocks 专属
uint32_t b_cache = route_table_[nid];                 // ← blocks 文件路由(查 cache 用)
if (auto* cb = cache_->getCachedBlockById(b_cache)) ...
uint64_t off = 4096 + b * vec_block_size_
             + block_data_offset_[b]
             + node_slot_table_[nid] * dim * 4;
```

**效果**:修复后原图 recall 恢复到 95.70%,图裁剪路径可以正常验证。

**代价**:`vec_route_table_` 增加 4MB 常驻内存(1M 节点 × 4 字节);总常驻内存 269MB → 273MB(可忽略)。

**教训**:
- 依赖"多套配套文件的隐式 block_id 对齐"是脆弱设计--任何数据 pipeline 变更都可能触发
- 类似的"隐式对齐依赖" bug 在 vector search 系统里极其常见(PQ codes 顺序、GT 顺序、block 顺序),需要**每套映射都显式建立而非复用**

---

### P1: 图裁剪验证 -- 负结果 (2026-07-28)

**动机**:CSR 压缩后仍有 47MB 常驻,图裁剪能进一步减边+减内存。目标:边数 -30%,recall 保持 >95%。

**做法**:`src/pipeline/prune_graph.cpp` 实现两种裁剪策略:
1. **Degree Cap**:简单截断度数到 R_max
2. **MRNG (Monotonic Relative Neighborhood Graph)**:DiskANN Vamana 用的启发式,保留角度分散的邻居

**结果** (SIFT1M, 512MB cgroup, 修复 FINE_RERANK bug 之后):

| 配置 | Edges | 减边 | CSR 内存 | Recall | QPS | RSS(cgroup total) |
|------|-------|------|---------|--------|-----|-----|
| 原图 (baseline) | 21.2M | 0% | 47MB | **95.70%** | 2067 | 269MB |
| MRNG R_max=28 | 19.6M | -7.4% | 44MB | 95.50% | 2117 | 348MB ⚠️ |
| MRNG R_max=24 | 18.0M | -15.2% | 40MB | 94.75% | 2144 | 333MB ⚠️ |
| MRNG R_max=20 | 16.2M | -23.6% | 36MB | 94.05% | 2197 | 316MB ⚠️ |
| MRNG R_max=16 | 14.1M | -33.5% | 34MB | 93.10% | 2187 | 298MB ⚠️ |

**观察 1:Recall 掉得比预期快**

即使 R_max=28(只裁 7.4% 边)也掉 0.2pp。MRNG 保留角度分散的邻居,但 SIFT 数据集的 HNSW 图已经很紧凑(avg degree 21),裁剪立即触及 recall。

**观察 2:RSS 反常上涨 30-80MB**

更小的图 → 搜索路径更局部化 → 反复访问相同的 vecblocks 页 → **OS page cache 保留更多热页** → cgroup 记账的 file 部分上涨。这不是坏事(page cache 是共享资源,无成本),但说明**内存瓶颈不在 CSR**。

**观察 3:QPS 仅微涨**

MRNG R=20 只提升 QPS 6.3%(2067→2197),因为 QPS 瓶颈不在图遍历(PQ+SIMD 已经很快),而在 flat_vec_cache 和 page cache 的命中路径。

**Page Cache 分析(关键洞察)**:

用 `mincore` 检查 vecblocks 496MB 文件的 page cache 覆盖率:

```
vecblocks 在 OS page cache: 100.0% (496.1 MB)
cgroup memory.stat file: 0-238MB (取决于是否首次读入)
```

- **系统充足内存下**:vecblocks 全部在宿主机 page cache,cgroup 里 file=0(不计入首次读入这个 cgroup 的),我们"白嫖"到了 500MB 的隐形缓存
- **冷启动 512MB 严格限制**:cgroup 首次读会计入 file,稳态 file~200MB + anon 269MB ≈ 470MB,**QPS 完全不变**(2093 vs 热态 2067)
- **说明**:即使真的把 vecblocks 从 page cache 驱逐大部分,flat_vec_cache 已经兜住核心搜索路径

**结论**:

1. **在 1M 规模 + 512MB cgroup 下,图裁剪不产生净收益**:
   - Recall 掉得多 > QPS 涨得少
   - CSR 内存已经不是瓶颈(page cache 才是内存去向)
   - 512MB 预算下当前设计 269MB 常驻 + 500MB page cache(宿主机层面)

2. **图裁剪的真正价值场景在 100M+**:
   - 100M 边 = 8.9GB CSR,即使 varint 压缩后 4.7GB,容不下
   - 100M 时必须靠图裁剪减到 40-50% 边数才能常驻
   - 但届时的 recall 掉幅会比 1M 小(图更冗余,MRNG 更有裁剪空间)

3. **P1 阶段结束,不合入主线**:`sift1m_graph_mrng*.bin` 保留在 output/ 供未来 100M 实验参考,主线继续用原图。

**规模推演修正**:

| 规模 | CSR 原始 | CSR varint | CSR varint+MRNG(R=20) | 常驻总计 | 512M | 1G | 2G |
|------|---------|-----------|----------------------|---------|------|-----|-----|
| 1M | 84 MB | 47 MB | 36 MB | 269 MB | ✅ | ✅ | ✅ |
| 10M | 888 MB | ~470 MB | ~360 MB | ~1.1 GB | ❌ | ✅ 待验证 | ✅ |
| 100M | 8.9 GB | ~4.7 GB | ~3.6 GB | ~7 GB | ❌ | ❌ | ❌(需 CSR 上磁盘) |

---

## 未来方向

P0/P0.5/P1 阶段结束后,1M 规模的优化空间已基本挖尽(95.70% recall / 2780 QPS 1T / 5808 QPS 4T / 269MB RSS)。下一阶段的战略重心转向**规模化**,因为:

- 1M 的向量数据只有 496MB,宿主机 page cache 能装下整个文件,磁盘 I/O 优化的价值被掩盖
- 真正验证"内存受限磁盘向量搜索"叙事的战场在 10M / 100M:vecblocks 5GB / 50GB **必然**装不进 page cache,冷 I/O 成为主导
- 届时 hnswlib 会直接 OOM(10M 需 ~7GB,100M 需 ~70GB),我们的对比才有杀伤力

### P2: 10M 规模验证 (下一步)

**目标**:SIFT10M / DEEP10M,1GB cgroup,recall ≥ 95%,QPS > 500 单线程。

**关键挑战**:
1. **CSR 内存**:10M × 21 edges × varint ≈ 470MB,加 route/slot/PQ codes 常驻总量约 1.1GB。需要 1GB→1.5GB cgroup,或启用 MRNG 裁剪到 ~360MB
2. **flat_vec_cache 覆盖率降至 1%**(64MB 只能装 130K 个上层向量),page cache 变成主要热区
3. **冷启动测试变得重要**:5GB vecblocks 装不进 1GB 预算,首次查询延迟会有真实峰值

**验收指标**:
- Recall@10 ≥ 95%
- P50/P99 延迟:<5ms / <20ms(冷 I/O 可能顶到 P99)
- 单线程 QPS > 500
- vs hnswlib:内存节省 5x+(1GB vs 7GB OOM)

**技术准备**:
- 已完成:CSR 压缩、FINE_RERANK bug 修复、MRNG 裁剪工具
- 待完成:10M 数据集下载 + pipeline 全套跑通 + 更大规模的 PQ M(可能升 M=64 保 recall)

### P3: CSR 上磁盘 + I/O 掩盖 (中期)

**动机**:100M 规模下 CSR varint 也要 4.7GB,即使加 MRNG 裁到 3.6GB 也超预算。CSR 必须能上磁盘。

**思路**:
1. **热点保留**:Layer 1+ (63K 上层节点) 的 CSR 常驻内存(图遍历第一步)
2. **Layer 0 冷区上磁盘**:按 BFS 顺序打包成 4KB 页,用现成的 io_uring/pread 基础设施读
3. **1-hop 预取**:搜索到节点 N,遍历邻居时预先异步读取邻居的邻居(模仿向量的 graph-guided prefetch)
4. **热点 CSR 缓存**:LRU 缓存 ~64MB 热邻接表页

**技术难点**:CSR 是不定长的(varint 编码),4KB 页边界可能切开某个节点的邻居列表。需要设计 page 内 padding 或跨页拼接。

### P4: 分级存储 + 混合 workload (长期)

**动机**:真实生产场景很少是纯查询--常有增量插入、周期性再训 PQ、多租户 QoS。

**方向**:
1. **多级存储**:hot (RAM) / warm (NVMe SSD) / cold (S3) 三层,按热度自动迁移
2. **增量索引**:新向量先写内存 delta 段,周期性合并到磁盘主索引(LSM-tree 思路)
3. **多租户 QoS**:cgroup 内存/IO 隔离 + per-tenant flat_vec_cache 分配
4. **PQ 自适应重训**:数据漂移检测(新向量的重建 MSE 升高)触发在线 PQ 再训

### P5: 硬件亲和优化 (探索)

1. **NUMA 亲和**:CSR / PQ codes 分布式在多 NUMA 节点,避免远端访存(结合 KUMF SPE 分析)
2. **SPDK / user-space NVMe**:跳过内核 buffered I/O,直接用 SPDK 打满 NVMe QD32
3. **GPU 加速 Phase A**:PQ SIMD 查表天然适合 GPU(batch query 场景)
4. **持久内存 (Intel Optane / CXL)**:把 vecblocks 放 PMEM,访问延迟从 ~10μs (NVMe) 降到 ~300ns

### 设计哲学

贯穿所有阶段的核心思想:

1. **常驻内存 = 图结构 + 压缩向量代理**(CSR + PQ codes),不常驻 = 全量向量
2. **两阶段搜索**是内存卸载的本质:Phase A 用内存代理粗筛,Phase B 按需 I/O 精排
3. **每个映射显式建立**,不依赖"多套数据的隐式对齐"(FINE_RERANK bug 教训)
4. **量化优化 ROI**:每个优化都要问「省了多少内存 / QPS 涨了多少 / recall 掉了多少 / 代码复杂度值不值」
5. **规模化优先于极致优化**:1M 的 100 QPS 提升不如 100M 能跑起来重要

---

## 已知限制与注意事项

1. **vecblocks 与 route table 必须配套**:混用不同版本的文件会导致 offset 错误
2. **io_uring 非线程安全**:多线程必须设置 `FINE_PREAD=1`
3. **fio 测试不要指向数据文件**:fio 会覆写文件内容(惨痛教训)
4. **性能异常先查 CPU 频率**:`grep MHz /proc/cpuinfo`,热保护降频会让所有测量慢 2.5x
5. **OPQ 旋转对 SIFT 无效**:SIFT 直方图特征各维已均衡,M=32/dsub=4 下 OPQ 重建误差反而更大
6. **GT 文件有 8B header**:`n_queries(u32) + k(u32)`,读取时需跳过
7. **blocks 和 vecblocks 的 block_id 不一致**:两个文件包含相同 node 但 block 划分不同(因邻接表占用空间)。每个文件必须用自己的 route 表。FINE_RERANK 代码中 `vec_route_table_` 专用于 vecblocks 定位,不可共用 blocks 的 `route_table_`
8. **cgroup memory.file 不等于 page cache 使用量**:如果文件首次读入发生在宿主机(而非 cgroup 内),cgroup file 字段不会计入。想得到真实记账需要先 `posix_fadvise(DONTNEED)` 驱逐再在 cgroup 内冒冷启动
9. **flat_vec_cache 才是真正的热区**:FLAT_VEC_MB=64 已能装 65K 个上层向量,不同于一般直觉,支撑了大部分 QPS(而非 vecblocks 的 page cache)。进一步调大反而限制图结构内存预算

### P2 详细记录: DEEP10M 10M 规模验证

**数据集**: DEEP10M (96D, 9,990,000 向量, 3.7GB base)

**关键优化**:

1. **VisitedList uint32 -> uint8** (★★★ 最大收益)
   - 10M 节点: 40MB -> 10MB per VisitedList
   - T=12: 节省 360MB 匿名内存
   - 消除内存分配瓶颈: 2x QPS 提升 (286 -> 590 @ 1T)
   - 根因: 每次 searchKnn 创建/销毁 VisitedList, 大块 malloc/free 成为隐藏瓶颈

2. **io_uring fine rerank bug 修复**
   - 症状: REFINE_EF>200 时 recall 反降 (200->94.20%, 300->88.40%, 400->70.80%)
   - 根因: io_uring 路径在大候选量时丢失部分 I/O 提交
   - 修复: 使用 FINE_PREAD=1 (pread 替代 io_uring, 线程安全)
   - 修复后: REFINE_EF=300 -> 95.15%, REFINE_EF=400 -> 95.55%

3. **PQ dsub=3 SIMD** (小幅)
   - DEEP10M 96D/M=32 -> dsub=3 (非 4, 现有 AVX2 不覆盖)
   - 添加 SSE 4-centroid 并行 LUT 构建
   - AVX2 gather 加速 pqDistance (8 lookups/iteration)
   - 收益: ~5% QPS (LUT 构建非主要瓶颈)

4. **图裁剪 MRNG R_max=24** (负结果, 不合入主线)
   - CSR: 591MB -> 522MB (仅 -69MB)
   - Recall: 95.15% -> 94.90% (EF=300) / 95.30% (EF=400)
   - QPS: 590 -> 624 (+6%, 因更少边遍历)
   - 结论: 10M 规模 CSR 已不是内存瓶颈 (cgroup 下 page cache 回收后 RSS 仅 941MB)
   - 裁剪收益不足以抵消 recall 损失

**cgroup 内存分析**:

| Cgroup | Init RSS | Peak RSS | QPS (T=4) | 状态 |
|--------|----------|----------|-----------|------|
| 无限制 | 2422 MB | 2484 MB | 590 | ✅ (含 page cache) |
| 2GB | 1088 MB | 1520 MB | 1484 | ✅ |
| 1.8GB | 941 MB | 1395 MB | 1483 | ✅ |
| 1.5GB | - | - | - | ❌ OOM |
| 1GB | - | - | - | ❌ OOM |

> 无 cgroup 时 RSS 2422MB 大部分是 file-backed page cache。cgroup 限制强制回收 page cache,
> 实际匿名内存仅 ~941MB。1GB 不可行因 CSR(591MB)+PQ codes(304MB)+upper vecs(228MB)=1.1GB 核心数据。

**与 SIFT1M (P0-P1) 的瓶颈对比**:

| 指标 | SIFT1M (1M) | DEEP10M (10M) |
|------|-------------|---------------|
| 瓶颈类型 | I/O (60%) | PQ 计算 (80%) |
| CSR 大小 | 47MB | 591MB (12.6x) |
| PQ codes | 32MB | 304MB (9.5x) |
| Page Shuffle 价值 | 有效 (+2.1%) | 无效 (I/O 仅 7%) |
| cgroup 可行性 | 512MB | 1.8GB (3.5x) |
| VisitedList 优化 | 4MB -> 1MB | 40MB -> 10MB (10x 绝对值) |
