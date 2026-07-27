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
# 依赖: Linux 5.1+ (io_uring), g++ C++17, libaio
make all

# 或手动编译
g++ -O3 -march=native -std=c++17 src/benchmark/benchmark_diskhnsw.cpp \
    src/core/disk_hnsw.cpp src/core/block_cache.cpp src/core/graph_prefetcher.cpp \
    -I hnswlib -I include -o build/benchmark_diskhnsw -pthread
```

还需额外编译 pipeline 工具：
```bash
g++ -O3 -std=c++17 -march=native -I hnswlib -I include \
    src/pipeline/write_blocks_veconly.cpp -o build/write_blocks_veconly -pthread
```

---

## 数据准备 Pipeline

以 SIFT1M（128 维，100 万向量）为例：

### Step 1: 构建 HNSW 图
```bash
./build/build_index data/test_1m.fvecs output/sift1m_index.bin 16 200
```

### Step 2: 提取图结构
```bash
./build/extract_graph output/sift1m_index.bin output/sift1m_graph.bin 128
```

### Step 3: BFS 重排（局部性优化）
```bash
./build/bfs_reorder output/sift1m_graph.bin output/sift1m_bfs.bin
```

### Step 4: 生成向量块文件（Vec-Only 格式）
```bash
./build/write_blocks_veconly output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_vecblocks_64k.bin 65536
# 同时生成 route table: output/sift1m_vecblocks_64k_route.bin
```

### Step 5: 生成旧格式块文件（BlockCache 用）
```bash
./build/write_blocks output/sift1m_graph.bin output/sift1m_bfs.bin \
    output/sift1m_blocks_64k.bin 65536
./build/gen_route output/sift1m_blocks_64k.bin output/sift1m_route_64k.bin
```

### Step 6: 训练 PQ 编码
```bash
python3 scripts/train_pq.py
# 输出: output/pqco_sift1m_M32_correct.bin
```

### Step 7: 准备 Ground Truth
GT 文件格式：8B header (n_queries:u32 + k:u32) + n×k 个 u64 向量 ID。

---

## 运行 Benchmark

### 单线程（推荐配置）
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

```
┌─────────────────────────────────────────────────────────┐
│                    常驻内存 (~120MB)                      │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐  │
│  │ 上层图+向量   │  │ L0 CSR邻接表  │  │ PQ Codes 32MB │  │
│  │ (63K nodes)  │  │ (84MB)       │  │ + Codebook    │  │
│  └──────────────┘  └──────────────┘  └───────────────┘  │
│  ┌──────────────┐  ┌──────────────┐                     │
│  │ route_table  │  │ slot_table   │  ← 间接寻址表       │
│  │ (4MB)        │  │ (2MB)        │                     │
│  └──────────────┘  └──────────────┘                     │
├─────────────────────────────────────────────────────────┤
│              按需 I/O (page cache 热区)                  │
│  ┌──────────────┐  ┌──────────────┐                     │
│  │ VecBlocks    │  │ FlatVecCache │  ← FLAT_VEC_MB 控制 │
│  │ (磁盘, 496MB) │  │ (内存, 64MB) │                     │
│  └──────────────┘  └──────────────┘                     │
└─────────────────────────────────────────────────────────┘
```

### 搜索流程（两阶段）

1. **贪心下降**：在上层图（内存）中找到 Layer 0 入口点
2. **Phase A — PQ 粗筛**：在 Layer 0 用 PQ ADC 距离做 best-first 搜索，收集 `REFINE_EF` 个候选
   - `PQ_HYBRID=1` 时，如果候选向量在 flat vec cache 中命中，用精确距离替代 PQ（提升质量）
3. **Phase B — 精确精排**：对候选集读取真实向量做精确 L2，保留 top-K
   - `FINE_BUFFERED=1`：用 buffered I/O，热数据走 page cache 零磁盘 I/O
   - `FINE_PREAD=1`：用 `pread` 替代 io_uring（多线程安全）

### 数据文件说明

| 文件 | 格式 | 内容 |
|------|------|------|
| `*_graph.bin` | HARG | 图结构：节点层级 + 上层向量 + labels + L0邻接表 |
| `*_bfs.bin` | BFS | BFS 重排映射：old_id ↔ new_id |
| `*_blocks_64k.bin` | HKLB | 64KB 块文件（含邻接表，BlockCache 用） |
| `*_route_64k.bin` | ROUTE | 节点→块映射表 |
| `*_vecblocks_64k.bin` | HKLB | Vec-Only 块文件（仅向量，FINE_RERANK 用） |
| `*_vecblocks_64k_route.bin` | ROUTE | Vec-Only 的节点→块映射（**必须与 vecblocks 配套**） |
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
│   ├── layout_provider.h  #   块布局策略 (BFS/Linear)
│   └── replacement_policy.h #  缓存替换策略 (LRU)
├── src/
│   ├── core/              # 核心库
│   │   ├── disk_hnsw.cpp  #   DiskHNSW 实现（搜索、PQ、精排）
│   │   ├── block_cache.cpp#   BlockCache 实现
│   │   └── graph_prefetcher.cpp # 图引导预取
│   ├── pipeline/          # 索引构建工具
│   │   ├── build_index.cpp        # HNSW 建图
│   │   ├── extract_graph.cpp      # 提取图结构
│   │   ├── bfs_reorder.cpp        # BFS 重排
│   │   ├── write_blocks_veconly.cpp # Vec-Only 块生成
│   │   ├── write_blocks.cpp       # 块生成（含邻接表）
│   │   ├── gen_route.cpp          # 路由表生成
│   │   └── verify.cpp             # 数据验证
│   ├── benchmark/         # 基准测试
│   │   ├── benchmark_diskhnsw.cpp #   DiskHNSW 主 benchmark
│   │   ├── benchmark_hnswlib_native.cpp # hnswlib 对比基线
│   │   └── benchmark_overlap.cpp  #   早期 I/O 重叠 benchmark
│   └── test/              # 单元测试
├── scripts/               # Python 脚本
│   ├── train_pq.py        #   PQ 训练（faiss）
│   └── train_opq.py       #   OPQ 旋转训练（已验证不适用 SIFT）
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
