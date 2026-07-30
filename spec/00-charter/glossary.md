# Glossary — 术语定义

## DEF: GraphStructure {#DEF-001}
<!-- ndf: kind=def layer=L1 status=stable since=0.1 source=observed -->

HNSW 图的内存表示。定义于 `common.h:113-136`，包含：
- `num_nodes`, `dim`, `maxM`, `maxM0`: 图元数据
- `levels[n]`: 每个节点的最高层
- `vectors[n*dim]`: 全量向量（slim 模式下不加载）
- `labels[n]`: uint64 外部标识
- `adjacency0[n]`: L0 邻接表（slim 模式下不加载）
- `upper_adjacency[n][level]`: 上层邻接表

## DEF: slim 加载模式 {#DEF-002}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

`load_graph_structure_slim()` 和 `load_graph_structure_slim_adj()` 只加载上层节点向量和 L0 邻接表，跳过全量 L0 向量。RSS 从 ~584MB 降至 ~52MB（无邻接表）或 ~133MB（含邻接表）。

## DEF: BFS 重排 {#DEF-003}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

按图的 BFS 遍历顺序给节点重新编号，让相邻节点在磁盘块中物理相邻，提升块内命中率。通过 `old_to_new_` / `new_to_old_` 双向映射表维护新旧 ID 关系。

## DEF: Two-Stage Search {#DEF-004}
<!-- ndf: kind=def layer=L1 status=stable since=0.1 source=observed -->

两阶段搜索 = Phase A (PQ ADC 粗筛，零向量 I/O) + Phase B (精确 L2 精排，4KB 页粒度按需读)。环境变量 `TWO_STAGE=1` 启用。

## DEF: PQ (Product Quantization) {#DEF-005}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

将 d 维向量空间分解为 M 个 dsub 维子空间，每个子空间用 ksub=256 个中心点量化。编码后每向量 M 字节。存储在 `pq_codes_[]` 中，按 BFS new_id 索引。

## DEF: Fine Rerank {#DEF-006}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

Phase B 精确精排：对 Phase A 粗筛的候选（约 100 个），按 4KB 页粒度读取真实向量做精确 L2 距离重排。通过 `FINE_RERANK=1` 开启，依赖 `VEC_BLOCKS_PATH`。

## DEF: Block/VecBlock {#DEF-007}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

- **Block**: 64KB/256KB 磁盘块，含邻接表 + 向量（`write_blocks` 产出），供 BlockCache 使用
- **VecBlock**: 仅含向量的 64KB 块（`write_blocks_veconly` 产出），供 Fine Rerank 4KB 页粒度读取

## DEF: CSR (Compressed Sparse Row) {#DEF-008}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

邻接表存储格式。`adj_csr_offsets_[N+1]` + `adj_csr_neighbors_[total_edges]`。P0 优化后支持 delta+varint 压缩（`adj_csr_compact_` 字节流 + `adj_csr_byte_offsets_`）。

## DEF: Varint Encoding {#DEF-009}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

LEB128 无符号整数变长编码 (`common.h:143-167`)。Delta+Varint 组合：先对排序后的邻居 ID 做 delta 编码（存差值），再用 varint 编码差值。4 字节 uint32 可压至 1-5 字节。

## DEF: Route Table {#DEF-010}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

`node_id -> block_id` 映射表（`route_table_`）。存在两个变体：
- `route_table_`: blocks 文件的路由（用于 BlockCache 查询）
- `vec_route_table_`: vecblocks 文件的路由（专用于 Fine Rerank），修复了 blocks/vecblocks block_id 不一致的 bug

## DEF: io_uring {#DEF-011}
<!-- ndf: kind=def layer=L2 status=stable since=0.1 source=observed -->

Linux 5.1+ 内核异步 I/O 接口。`IoUring` 类 (`io_uring_wrapper.h`) 通过 `io_uring_setup(2)` + `mmap` SQ/CQ ring + `io_uring_enter(2)` 实现零外部依赖的异步 I/O。
