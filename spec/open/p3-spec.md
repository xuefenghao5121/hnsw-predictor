# P3: 100M 规模 - 规范性描述

> 创建: 2026-07-30
> 状态: draft
> 关联: CHR-004 (演进路线), DEC-037 (cgroup 目标校准), Q-001 (规模上限已回答)

---

## P3 L0 意图 {#P3-INTENT-001}
<!-- ndf: kind=arch level=must layer=L0 status=draft since=0.5 source=deduced -->

DiskHNSW MUST 在 100M 规模下维持 recall≥95% / QPS>100 的 SLA，
同时将常驻内存控制在 4GB cgroup 内。

**核心挑战**: 100M 规模下 CSR varint 4.9GB + PQ codes 3.2GB + upper vecs 2.4GB = 11.3GB
核心数据，远超任何合理的 cgroup 限制。P3 的本质是**把图结构从内存卸载到磁盘**。

**规模推演**:

| 组件 | 1M (P0) | 10M (P2) | 100M (P3) | 趋势 |
|------|---------|----------|-----------|------|
| CSR | 47MB | 591MB | 4.9GB | ×10/级 |
| PQ codes | 32MB | 304MB | 3.2GB | ×10/级 |
| Upper vecs | 12MB | 228MB | 2.4GB | ×10/级 |
| 核心 RSS | ~100MB | ~1.1GB | ~11.3GB | ×10/级 |
| cgroup | 512MB | 2GB | 4GB? | ×2/级 |
| 瓶颈 | I/O | PQ 计算 | CSR 内存 | 转移 |

## P3 L1 行为契约

### CSR 分页加载 {#P3-BEH-001}
<!-- ndf: kind=req level=must layer=L1 status=draft since=0.5 source=deduced -->
<!-- refines=P3-INTENT-001 -->

当 CSR 总大小超过 cgroup 预算的 50% 时，DiskHNSW MUST 将 CSR 数据存储在磁盘文件中，
按需分页加载到内存，而非全量常驻。

**外部可观察行为**:
- `searchKnn()` 的返回值与 CSR 常驻内存时 MUST 完全一致（recall 不变）
- 每次邻居展开 MAY 触发至多 1 次 CSR 页读取
- CSR 页大小 SHOULD 为 4KB（与 vecblocks 页对齐，复用 page cache）

### 1-hop CSR 预取 {#P3-BEH-002}
<!-- ndf: kind=req level=should layer=L1 status=draft since=0.5 source=deduced -->
<!-- refines=P3-BEH-001 -->

DiskHNSW SHOULD 在展开当前候选的邻居时，异步预取这些邻居的 CSR 页，
使得下一步展开时 CSR 页已在内存。

**预取策略**:
- 预取深度: 1 hop（只预取下一跳的 CSR 页）
- 预取方式: io_uring 批量提交 或 pread 线程池
- 预取淘汰: LRU，CSR 页缓存大小可配置（`CSR_CACHE_MB`）

### PQ codes 内存映射 {#P3-BEH-003}
<!-- ndf: kind=req level=should layer=L1 status=draft since=0.5 source=deduced -->
<!-- refines=P3-INTENT-001 -->

PQ codes (3.2GB@100M) SHOULD 通过 `mmap` 映射到进程地址空间，
由 OS 按需分页。访问模式为随机读（按 node_id × M 偏移）。

**行为约束**:
- `pqDistance()` 的返回值与全量加载时 MUST 完全一致
- mmap 使用 `MAP_PRIVATE` (只读)
- OS page cache 管理 PQ codes 页的换入换出
- cgroup v2 下 mmap 的 file-backed pages 计入 `memory.file`

### 上层向量 PQ 编码 {#P3-BEH-004}
<!-- ndf: kind=req level=should layer=L1 status=draft since=0.5 source=deduced -->
<!-- refines=P3-INTENT-001 -->
<!-- depends-on=DEC-034 (Upper Layer PQ, 已有决策) -->

上层节点向量 (2.4GB@100M) SHOULD 用 PQ 编码压缩到常驻内存，
而非存储精确向量。

**行为约束**:
- 贪心下降阶段使用 PQ ADC 距离替代精确 L2
- recall 损失 SHOULD ≤ 0.5pp
- 上层 PQ 编码大小 SHOULD ≤ 200MB（M=32, 6.2M nodes × 32 bytes）

## P3 L2 机制设计

### CSR 分页文件格式 {#P3-ARCH-001}
<!-- ndf: kind=arch level=must layer=L2 status=draft since=0.5 source=deduced -->
<!-- refines=P3-BEH-001 -->

CSR 分页文件 (`csr_pages.bin`) 格式:

```
[4KB Header]
  magic: u32 = 0x43535200 ("CSR\0")
  num_nodes: u32
  num_edges: u64
  page_size: u32 = 4096
  num_pages: u32
  offsets_offset: u64  // byte_offsets[] 在文件中的偏移

[Page Table: num_nodes × 4B]
  byte_offset[node_id]: u32  // 节点 node_id 的 CSR 数据在文件中的字节偏移
  // 对齐到 4KB 页: byte_offset = page_index * 4096 + page_inner_offset

[CSR Data Pages: num_pages × 4096B]
  每页包含多个节点的 delta+varint 编码邻接表
  节点边界由 byte_offset[] 定位
```

**设计约束**:
- 每页 4KB，对齐 NVMe 扇区
- 节点的 CSR 数据 MAY 跨页（大度数节点）
- byte_offset 索引常驻内存: 100M × 4B = 400MB（可接受）

### CSR 页缓存 {#P3-ARCH-002}
<!-- ndf: kind=arch level=must layer=L2 status=draft since=0.5 source=deduced -->
<!-- refines=P3-BEH-001 -->

CSR 页缓存 (`CSRCache`):
- LRU 淘汰策略
- 大小可配置: `CSR_CACHE_MB` (默认 256MB)
- 缓存命中: 直接返回页内 CSR 数据
- 缓存未命中: pread 4KB 页，插入缓存

**性能目标**:
- 缓存命中率 SHOULD ≥ 80%（BFS 重排保证局部性）
- CSR 页 I/O 延迟 SHOULD ≤ 50μs (NVMe)

### 搜索路径修改 {#P3-ARCH-003}
<!-- ndf: kind=arch level=must layer=L2 status=draft since=0.5 source=deduced -->
<!-- refines=P3-BEH-001,P3-BEH-002 -->

`searchLayer0()` 修改:

```cpp
// 当前 (P2): CSR 常驻内存
const uint32_t* neighbors = getInMemNeighbors(candidateId, neighborCount);

// P3: CSR 分页加载
const uint32_t* neighbors = csrCache->getNeighbors(candidateId, neighborCount);
// 内部: 查页缓存 -> 命中返回 / 未命中 pread 4KB
```

**关键不变量**:
- `getNeighbors()` 返回的指针在下次 `getNeighbors()` 调用前有效
- thread_local 解码缓冲区 (`csr_decode_buf_`) 避免锁竞争

## P3 约束

### P3 内存预算 {#P3-CON-001}
<!-- ndf: kind=constraint level=must layer=L1 status=draft since=0.5 source=deduced -->

| 组件 | 大小 | 存储 | 说明 |
|------|------|------|------|
| CSR byte_offset 表 | 400MB | 常驻内存 | 100M × 4B 索引 |
| CSR 数据 | 4.9GB | 磁盘 + 页缓存 | 4KB 分页, CSR_CACHE_MB=256 |
| PQ codes | 3.2GB | mmap | OS 按需分页 |
| 上层 PQ 编码 | 200MB | 常驻内存 | 6.2M × 32B |
| BFS 映射 | 800MB | 常驻内存 | 2 × 100M × 4B |
| VisitedList (T=12) | 120MB | 常驻内存 | 12 × 10MB (uint8) |
| Route 表 + 槽位表 | ~1.2GB | 常驻内存 | 3 × 100M × 4B |
| Block cache + flat vec | 192MB | 常驻内存 | CACHE_MB=128 + FLAT_VEC_MB=64 |
| **常驻合计** | **~2.9GB** | | 4GB cgroup 可行 |

### P3 SLA {#P3-CON-002}
<!-- ndf: kind=constraint level=must layer=L1 status=draft since=0.5 source=deduced -->

| 指标 | 目标 | 理由 |
|------|------|------|
| Recall@10 | ≥95% | 与 P2 一致 |
| QPS (1T) | >100 | 100M 规模每查询需 ~200 次 CSR 页读 + ~200 次 PQ code 访问 |
| QPS (12T) | >500 | 多线程并行 |
| RSS | ≤4GB | 4GB cgroup |
| vs hnswlib | 内存节省 ≥3x | hnswlib 100M 需 ~50GB (38GB vec + 12GB graph) |

## P3 验证准则

### P3 功能验证 {#P3-VER-001}
<!-- ndf: kind=verif level=must layer=L3 status=draft since=0.5 source=deduced -->
<!-- verifies=P3-BEH-001,P3-BEH-002,P3-BEH-003,P3-BEH-004 -->

| 用例 | 配置 | 预期 | 说明 |
|------|------|------|------|
| Recall 一致性 | CSR 分页 vs 常驻 | ±0.0pp | 分页不影响结果 |
| CSR 缓存命中 | 200 query 后 | ≥80% | BFS 局部性 |
| PQ mmap 正确性 | mmap vs 全量加载 | ±0.0pp | 距离值完全一致 |
| 上层 PQ recall | 上层 PQ vs 精确 | recall 损失 ≤0.5pp | 贪心下降精度 |
| 多线程安全 | T=12 并发 | 无 crash, recall 不变 | thread_local 缓冲 |

### P3 性能验证 {#P3-VER-002}
<!-- ndf: kind=verif level=must layer=L3 status=draft since=0.5 source=deduced -->
<!-- verifies=P3-CON-002 -->

| 用例 | 预期 | 说明 |
|------|------|------|
| 1T QPS | ≥100 | 100M 规模 |
| 12T QPS | ≥500 | 多线程 |
| RSS (4GB cgroup) | ≤4GB | 不 OOM |
| hnswlib 4GB cgroup | OOM | 对比基线 |
| CSR 页 I/O 延迟 | ≤50μs | NVMe 4KB read |

## P3 开放问题

### Q-003: 100M 数据集选择 {#Q-003}
<!-- ndf: kind=open blocks=P3-VER-001 source=deduced date=2026-07-30 -->

**问题**: P3 需要 100M 规模数据集，当前只有 DEEP10M (10M)。

**候选**:
1. DEEP1B 子集 100M (96D, 与 P2 一致)
2. SIFT1B 子集 100M (128D, 与 P0-P1 一致但维度不同)
3. 合成数据 (Gaussian/random, 验证性能但不验证 recall)

**决议条件**: 需要下载 ~40GB 原始数据 + 构建 ~5GB 索引。

### Q-004: PQ codes mmap vs 分页加载 {#Q-004}
<!-- ndf: kind=open blocks=P3-BEH-003 source=deduced date=2026-07-30 -->

**问题**: PQ codes 3.2GB 用 mmap 还是显式分页加载？

| 方案 | 优点 | 缺点 |
|------|------|------|
| mmap | 零代码改动, OS 管理分页 | cgroup 记账有坑 (P1 教训), 无法控制预取 |
| 显式分页 | 可控预取, 精确 cgroup 记账 | 需要实现 PQPageCache, 代码量大 |
| 全量加载 (3.2GB) | 最快访问 | 占 80% 的 4GB cgroup |

**初步倾向**: mmap + `posix_fadvise(POSIX_FADV_RANDOM)` 提示随机访问模式。

### Q-005: Route 表内存优化 {#Q-005}
<!-- ndf: kind=open blocks=P3-CON-001 source=deduced date=2026-07-30 -->

**问题**: 3 个路由表 (route + vec_route + node_slot) 共 ~1.2GB@100M，占常驻内存 41%。

**候选优化**:
1. 合并为单一路由表（blocks 和 vecblocks block_id 对齐）
2. 压缩为 uint16（如果 block 数 <65536）
3. 放到上层 PQ 编码中（用 PQ 重建路由信息）

## P3 设计选项

### CSR 页缓存大小 {#P3-OPT-001}
<!-- ndf: kind=option level=tbd layer=L2 status=draft since=0.5 -->
<!-- ndf: default=256 explore=128,256,512,1024 unit=MB -->
<!-- ndf: couples-with=P3-CON-001 -->

- **default:** 256MB - 覆盖 ~5% 的 CSR 页（4.9GB total）
- **exploration range:** {128, 256, 512, 1024} MB
- **coupling:** 与总 cgroup 预算竞争 ([[P3-CON-001]])

### CSR 页大小 {#P3-OPT-002}
<!-- ndf: kind=option level=tbd layer=L2 status=draft since=0.5 -->
<!-- ndf: default=4096 explore=4096,16384,65536 unit=bytes -->
<!-- ndf: couples-with=P3-ARCH-001 -->

- **default:** 4096 bytes (1 page) - 与 vecblocks 对齐
- **exploration range:** {4096, 16384, 65536} bytes
- **tradeoff**: 小页 I/O 量少但命中率低，大页命中率高但 I/O 放大

### 上层 PQ M 值 {#P3-OPT-003}
<!-- ndf: kind=option level=tbd layer=L2 status=draft since=0.5 -->
<!-- ndf: default=32 explore=16,24,32 unit=subquantizers -->
<!-- ndf: couples-with=P3-BEH-004 -->

- **default:** M=32 (与 L0 PQ 一致)
- **exploration range:** {16, 24, 32}
- **tradeoff**: M=16 省内存 (100MB) 但贪心下降精度低；M=32 精度高但 200MB
