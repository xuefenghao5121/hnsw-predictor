# P3 内存分配架构 - SIFT1M (NDF)

> 日期: 2026-07-30
> 类型: 架构参考文档
> 依据: DEC-039 诚实 benchmark 追踪
> 用途: 后续优化和开发时的内存预算参考

## 概述 {#MEM-001}
<!-- ndf: kind=arch level=L1 layer=L1 status=stable since=0.5 source=verified -->

DiskHNSW 的内存分为**匿名内存**（进程分配，不可回收）和 **page cache**（文件页，内核可回收）。
在 cgroup v2 中，两者共享 MemoryMax 预算。

```
cgroup MemoryMax = 匿名内存 + page cache
                   ↑不可回收     ↑可回收(LRU)
```

## SIFT1M 文件生命周期 {#MEM-002}
<!-- ndf: kind=arch level=L1 layer=L1 status=stable since=0.5 source=verified -->

### 数据文件总览

| 文件 | 大小 | I/O 方式 | 运行时角色 | 常驻类型 |
|------|------|---------|-----------|---------|
| sift1m_graph.bin | 587MB | ifstream (buffered) | init 解析为 CSR → fadvise 驱逐 | 仅 init |
| sift1m_blocks_64k.bin | 541MB | **O_DIRECT** | BlockCache 按需读取 | 不进 page cache |
| sift1m_vecblocks_64k.bin | 497MB | pread (buffered) | Fine Rerank 按需读取 | page cache (cgroup 计费) |
| pqco_sift1m_M32.bin | 31MB | ifstream (buffered) | init 加载到 pq_codes_ → fadvise 驱逐 | 仅 init |
| sift1m_bfs.bin | 7.7MB | ifstream | init 加载 → fadvise 驱逐 | 仅 init |
| route files | ~8MB | ifstream | init 加载到内存 | 匿名 |

### 每个文件的详细 I/O 路径

#### graph.bin (587MB) — 流式解析后驱逐 {#MEM-003}

```
打开: std::ifstream (buffered I/O, 走 page cache)

init 过程:
  ① 读 header (32B)
  ② 读 levels[1M] (4MB) 
  ③ seekg() 跳过 L0 向量 (488MB 不读, slim 格式)
  ④ 逐个 seekg() 读上层向量 (63001 × 512B = 30MB)
  ⑤ 读 labels (8MB)
  ⑥ 读 L0 邻接表 (80MB, 逐节点 16-bit count + edges)
  ⑦ 读上层邻接表 (~2MB)
  ⑧ 解析为 CSR: delta + varint 压缩 → 47MB 匿名内存
  ⑨ posix_fadvise(DONTNEED) → 驱逐所有文件页

实际读取量: ~124MB (跳过了 488MB L0 向量)
init 后文件页: 0 (fadvise 驱逐)
匿名内存产出: CSR compact 47MB + offsets 3MB + upper vecs 5MB = 55MB
```

**关键: seekg() 只读取上层节点向量, 跳过 488MB L0 向量。**
graph.bin 的 587MB 大部分是 L0 原始向量, 但 slim 格式不加载它们。

#### blocks_64k.bin (541MB) — O_DIRECT 绕过 page cache {#MEM-004}

```
打开: O_RDONLY | O_DIRECT (DMA 直接到用户 buffer, 不进 page cache)

init 过程:
  ① 读文件头 (对齐 buffer)
  ② BlockCache 初始化: 512 slots × 64KB = 32MB 匿名内存
  ③ 文件 fd 保持打开, 搜索时按需 pread

搜索过程:
  ① graph 遍历到 L0 节点 → 查 BlockCache
  ② cache miss → pread(block, O_DIRECT) → 真实磁盘 I/O
  ③ 读到的 block 存入 LRU slot (匿名内存)
  ④ LRU 淘汰时直接释放 (不写回, 因为是只读)

常驻: 32MB 匿名 (block cache slots)
page cache: 0 (O_DIRECT 绕过)
磁盘 I/O: cache miss 时真实读盘
覆盖率: 512/8651 = 5.9% (大部分搜索靠 PQ 距离, 不需要精确向量)
```

**关键: O_DIRECT 完全绕过 page cache。541MB 文件的 I/O 不消耗 cgroup 内存。**

#### vecblocks_64k.bin (497MB) — Fine Rerank page cache {#MEM-005}

```
打开: O_RDONLY (FINE_BUFFERED=1, buffered I/O)

搜索过程 (Fine Rerank):
  ① PQ 粗筛产出 ~100 候选 (REFINE_EF=100)
  ② 对每个候选: 计算 page_id = (vecblocks_offset >> 12)
  ③ 去重: 100 候选 → ~60 unique pages (BFS 重排保证局部性)
  ④ pread(4KB) 读取每个 page → page cache
  ⑤ 从 page 中提取 512B 向量, 做 L2 精排

200 query 的工作集:
  200 × 60 unique pages = ~12000 unique 4KB pages = 48MB
  (不是 200 × 100 × 4KB = 78MB, 因为 BFS 重排有大量去重)

常驻: 0 匿名 (vec_blocks_fd_ 只是文件描述符)
page cache: ~48MB (热页, 被 cgroup 计费)
```

#### PQ codes (31MB) — 加载后驱逐 {#MEM-006}

```
打开: std::ifstream (buffered)

init 过程:
  ① 读 header (28B)
  ② 读 codebook (M×K×dsub×4B = 32×256×3×4 = 96KB, 忽略)
  ③ 读 pq_codes (1M × 32B = 31MB) → pq_codes_ vector (匿名)
  ④ posix_fadvise(DONTNEED) → 驱逐文件页

常驻: 31MB 匿名
page cache: 0 (fadvise 驱逐)
```

## SIFT1M 512MB cgroup 内存分配表 {#MEM-007}
<!-- ndf: kind=constraint level=must layer=L1 status=verified since=0.5 source=verified -->

### 匿名内存 (不可回收)

| 组件 | 大小 | 说明 |
|------|------|------|
| CSR compact + offsets | 50MB | L0 邻接表 delta+varint 压缩 |
| PQ codes | 31MB | 1M × 32B, Phase A ADC 距离 |
| Upper layer vectors | 5MB | 63001 × 512B, 上层精确搜索 |
| Flat vec cache | 32MB | 65027 slot × 512B, upper layer 精确距离缓存 |
| Block cache slots | 32MB | 512 × 64KB, L0 block LRU |
| Fine slot table | 4MB | 1M × 4B, node→vecblocks 页偏移映射 |
| vec_route_table | 4MB | 1M × 4B, vecblocks 的独立路由表 |
| BFS mapping | 8MB | old_to_new + new_to_old |
| Route table | 4MB | BFS layout block 路由 |
| VisitedList (4T) | 4MB | 4 × 1M × 1B (uint8) |
| 线程栈/malloc | ~60MB | 4 线程 + PQ LUT table + 其他 |
| **匿名总计** | **~234MB** | |

### Page Cache (可回收, cgroup 计费)

| 组件 | 大小 | 说明 |
|------|------|------|
| vecblocks 热页 | ~48MB | 200 query Fine Rerank 工作集 |
| 其他 | ~5MB | 运行时动态文件页 |
| **page cache 总计** | **~53MB** | |

### 总计

```
匿名:     234MB
Page cache: 53MB
────────────────
总计:     287MB  (cgroup 限制 512MB, 余量 225MB)
```

## DEEP10M 内存分配对比 {#MEM-008}
<!-- ndf: kind=info level=L2 layer=L2 status=stable since=0.5 source=verified -->

| 组件 | SIFT1M | DEEP10M | 倍数 |
|------|--------|---------|------|
| CSR (compact+offsets) | 50MB | 591MB | 12x |
| PQ codes | 31MB | 304MB | 10x |
| Upper vectors | 5MB | 228MB | 46x |
| Flat vec cache | 32MB | 64MB | 2x |
| Block cache slots | 32MB | 128MB | 4x |
| Fine slot table | 4MB | 40MB | 10x |
| Route/tables | 16MB | 76MB | 5x |
| VisitedList (max T) | 4MB | 12MB | 3x |
| **匿名总计** | **~234MB** | **~1.1GB** | 5x |
| vecblocks page cache | ~48MB | ~900MB | 19x |
| CSR cache (进程内) | 0 | 256MB | - |
| **总计** | **~287MB** | **~2.3GB** | 8x |

## cgroup 预算分析 {#MEM-009}
<!-- ndf: kind=info level=L2 status=verified since=0.5 source=verified -->
<!-- updated=2026-07-30: 增加 pgmajfault 运行时分析 -->

### 稳态 vs 运行时峰值

> **关键认知: 288MB 是稳态值, 运行时内存会顶到 cgroup 上限。**

```
稳态 (搜索间隙):       运行时 (4T 并发搜索):
  匿名: 234MB            匿名: 264MB (+30MB malloc arena)
  file:   53MB           file:  119-149MB (vecblocks + slot table + 系统)
  slab:  ~5MB            slab: ~15MB
  总:   287MB            总:  ~398MB
                         memory.peak = cgroup MemoryMax (顶满)
```

运行时比稳态增加的来源:
1. **glibc malloc arena**: 4 线程 × 64MB arena → +30MB 匿名碎片
2. **slot table 构建**: vecblocks 全文件扫描 (7937 × 4KB = 31MB file pages)
3. **vecblocks 并发工作集**: 4 线程访问不同 query 区域 → 瞬时工作集更大
4. **系统文件页**: 共享库, locale 等
5. **内核 slab**: dentry/inode 等

### cgroup 统计对比 (384MB vs 512MB)

| 指标 | 384MB | 512MB | 说明 |
|------|-------|-------|------|
| QPS (4T) | 3519 | 6157 | 差 43% |
| RSS | 271MB | 275MB | 几乎相同 |
| memory.peak | 384MB | 512MB | 都顶满上限 |
| file pages | 119MB | 149MB | 512MB 多 30MB 缓存 |
| **pgmajfault** | **5523** | **10** | ★ 关键: 384MB 有大量真实磁盘 I/O |
| pgrefill | 3899 | 1440 | 384MB 回收更频繁 |
| pgscan | 175K | 128K | 384MB 扫描更多页 |
| pgsteal | 170K | 128K | 384MB 驱逐更多页 |
| workingset_refault | 1332 | 0 | 384MB 页面反复驱逐又重读 |

**根因: 运行时实际需求 ~398MB > 384MB cgroup。**
内核被迫频繁回收 page cache, 导致 5523 次真实磁盘 I/O (pgmajfault),
QPS 降低 43%。512MB 下需求 < 上限, 几乎无回收。

### SIFT1M cgroup 扫描

| cgroup | 匿名(稳态) | 运行时峰值 | pgmajfault | QPS (4T) |
|--------|-----------|-----------|------------|----------|
| 512MB | 234MB | ~398MB < 512 | 10 | 5247 |
| 384MB | 234MB | ~398MB > 384 | 5523 | 3744 |
| 256MB | 233MB | >256 | OOM-边缘 | 423 |

### DEEP10M cgroup 扫描

| cgroup | 匿名(稳态) | 运行时峰值 | QPS (12T) |
|--------|-----------|-----------|-----------|
| 2GB | 1.1GB | ~2.0GB ≈ 2GB | 1762 |
| 1.5GB | 817MB | ~2.0GB > 1.5GB | 327 |
| 1.2GB | 731MB | ~2.0GB > 1.2GB | 252 |

## 开发参考 {#MEM-010}
<!-- ndf: kind=info level=L2 status=stable since=0.5 source=verified -->

### 添加新功能时的内存预算检查

```
新功能内存需求 = X MB (匿名)

SIFT1M: cgroup 512MB - 匿名 234MB - page cache 53MB = 余量 223MB
         → X < 223MB ✅ (安全)

DEEP10M 2GB: cgroup 2048MB - 匿名 1100MB - page cache 900MB = 余量 48MB
         → X < 48MB ✅ (紧张)
         → 超过则需缩减 CSR cache 或 flat_vec_cache
```

### 优化方向的内存约束

| 优化 | 增加匿名 | 增加 page cache | 可行性 |
|------|---------|----------------|--------|
| CSR cache 增大 (256→512MB) | +256MB | 0 | SIFT1M ✅, DEEP10M 需增 cgroup |
| Block cache 增大 | +32MB | 0 | ✅ |
| Flat vec cache 增大 | +32MB | 0 | ✅ |
| 批处理 I/O 去重 | +少量 buffer | -工作集减少 | ✅ 双赢 |
