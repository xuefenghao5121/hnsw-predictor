# d-HNSW 风格分区实验结果（决定性结论）

> 日期: 2026-07-24
> 状态: ✅ 实验完成，得出决定性结论

---

## 实验设计

用户要求：
1. ✅ 回退到 recall 100% 全局图 baseline（已完成，disk_hnsw.cpp = recall100）
2. ✅ 尝试真正的 d-HNSW 风格：分区后按 block 存盘，保留跨分区边

### 实现方式

**关键洞察**：现有 pipeline 已经保留全量图，recall 100% 的秘密在于 `bfs_reorder` 只改变节点**物理布局顺序**，图边完整保留。

因此实现 d-HNSW 风格 = **用 K-means 分区顺序替换 BFS 顺序**：
- `kmeans_reorder.cpp`：读取 K-means 分区映射，按分区聚集节点顺序
- 同分区节点在磁盘上连续 → 理论上同分区节点落入相邻 block
- 图边完全保留（只是 new_id 重映射）→ recall 保证 100%

生成物：
- `output/test1m_kmeans_order.bin`（重排映射）
- `output/test1m_blocks_kmeans.bin`（2225 blocks）
- `output/test1m_route_kmeans.bin`（路由表）

---

## 决定性结果

### BFS vs K-means 分区（1M SIFT, ef=50, k=10, 200 queries）

| 布局 | Cache hit | F2-single QPS | F2-single Mean | Recall |
|------|-----------|---------------|----------------|--------|
| **BFS（图拓扑序）** | **86.8%** | **36.4** | **27.5ms** | 100% |
| **K-means（空间聚类序）** | 68.4% | 8.3 | 120ms | 100% |

**K-means 分区重排比 BFS 差 4.4 倍（QPS）**。

### 跨分区边统计

- 总边数（L0）: 16,134,100
- **跨分区边: 15,601,818 (96.7%)**
- K-means 分区重排的局部性改善仅 2.88%（vs 随机）

---

## 根本原因

**HNSW 图搜索遵循图边导航，而非空间邻近性。**

1. HNSW 搜索从 entry_point 出发，沿图边贪心走向 query
2. 96.7% 的图边是跨分区的 → 搜索路径频繁跳到其他分区的 block
3. K-means 按空间聚类，但搜索不按空间顺序访问，而是按图拓扑
4. **BFS 重排沿图边遍历**，让"图上相邻"的节点"物理相邻"→ 搜索路径上的节点落入同一 block → 命中率高

### 为什么空间聚类无效

- 向量空间的邻近 ≠ 图上的邻近
- HNSW 的长程边（long-range links）故意连接远处节点（用于快速导航）
- 这些边天然跨越任何空间分区
- 分区化优化的是"最终答案的空间局部性"，但搜索过程的 I/O 由"图遍历路径"决定

---

## 与 d-HNSW 论文的调和

d-HNSW 论文（arXiv:2603.13591）能达到 94% recall + 分区化收益，是因为：

1. **不同的系统假设**：d-HNSW 面向 disaggregated memory（RDMA 远程内存），I/O 单位是分区级 RDMA 传输，不是本地 block
2. **meta-HNSW 路由**：先用小索引路由到分区，只搜索 1-2 个分区（牺牲 recall 到 94%）
3. **查询级并行**：多查询的分区需求批量去重，而非单查询内 I/O overlap
4. **接受 recall 损失**：94% < 100%

**本项目要求 recall 100%**，因此不能用 d-HNSW 的"只搜索部分分区"策略。而"全局图 + 分区物理布局"对本地 block 缓存反而有害（破坏图遍历局部性）。

---

## 最终结论

### ✅ BFS 重排是本地 block 缓存场景的最优布局

- BFS（图拓扑序）: 命中率 86.8%, QPS 36
- K-means（空间序）: 命中率 68.4%, QPS 8
- **保持现有 BFS baseline**

### ❌ d-HNSW 风格分区不适用于本项目

原因：
1. HNSW 图遍历不遵循空间局部性
2. 96.7% 跨分区边使分区物理布局失去意义
3. 只搜索部分分区会降低 recall（违反硬性要求）

### 真正的优化方向（保持 BFS + recall 100%）

回到之前 MEMORY.md 记录的方向：
1. **非阻塞搜索**（算法级）：pending block 不阻塞，继续处理其他 candidate — 但需解决 lowerBound 一致性
2. **多查询流水线**（系统级）：Query A 的 I/O 与 Query B 的计算 overlap
3. **ML 预测预取**（项目核心创新）：提前 2-3 跳预取，扩大 overlap 窗口
4. **带宽压力实验**：构造 DRAM 带宽饱和场景（1B+ 向量），验证 F2 卸载到 NVMe 的 overlap 收益

---

## 文件

- 重排工具: `src/kmeans_reorder.cpp`
- 分区脚本: `scripts/partition_kmeans.py`
- 生成物: `output/test1m_kmeans_order.bin`, `output/test1m_blocks_kmeans.bin`, `output/test1m_route_kmeans.bin`
- BFS baseline: `output/test1m_bfs.bin`, `output/test1m_blocks.bin`, `output/test1m_route.bin`