# C 方案设计：d-HNSW 风格分区化索引

> 目标: 通过搜索前确定数据需求，避免逐 block 加载的非阻塞 lowerBound 问题
> 参考: d-HNSW 论文 (arXiv:2603.13591)

---

## 核心思想

**搜索前就确定需要加载哪些数据**，而不是在搜索过程中逐 block 加载。

1. **构建轻量级 meta-index**（采样点 + 小型 HNSW）
2. **分区全量数据**（100-500 个子图）
3. **查询路由**：meta-index → 定位 1-2 个分区
4. **批量加载**：一次读取完整分区
5. **本地搜索**：阻塞版搜索（数据已在内存）

---

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                     DiskHNSW (主索引)                         │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              Meta-Index (常驻 DRAM)                 │   │
│  │  采样点: 500 个向量 (~500 * 128 * 4B = 256KB)       │   │
│  │  结构: 3层 HNSW (M=16, efC=200)                     │   │
│  │  职责: 快速定位查询最近的 1-2 个分区入口            │   │
│  └─────────────────────────────────────────────────────┘   │
│                          ↓                                   │
│  ┌─────────────────────────────────────────────────────┐   │
│  │            Partition Mapping Table                   │   │
│  │  [node_id] → partition_id  (1M 条目, ~4MB)           │   │
│  │  映射: 基于聚类标签或 BFS 割边                        │   │
│  └─────────────────────────────────────────────────────┘   │
│                          ↓                                   │
│  ┌─────────────────────────────────────────────────────┐   │
│  │            Sub-HNSW Partitions (NVMe)                │   │
│  │  分区数: 100-500 个                                   │   │
│  │  每分区: 2K-10K 向量 (~1MB-5MB)                      │   │
│  │  结构: 独立的 HNSW 图 + 向量数据                     │   │
│  │  布局: 连续存储，单次 RDMA_READ 读取                 │   │
│  └─────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

---

## 详细设计

### 1. Meta-Index 构建

#### 采样策略
- **均匀采样**: 从全量 1M 向量中随机采样 500 个
- **可选策略**: K-means 采样（更代表数据分布）

#### Meta-HNSW 参数
```cpp
struct MetaHNSWConfig {
    size_t num_samples = 500;           // 采样点数
    size_t max_level = 3;               // 3 层（足够定位）
    size_t M = 16;                      // 连接度
    size_t ef_construction = 200;       // 构建参数
};
```

#### Meta-HNSW 存储格式
```cpp
// meta_index.bin
struct MetaIndex {
    Header {
        uint32_t num_samples;
        uint32_t max_level;
        uint32_t entry_point;  // meta-index 的入口点
    }
    Samples[num_samples] {
        float[128] vector;     // 采样向量
        uint32_t partition_id; // 该采样点所属的分区
    }
    GraphLayer0[num_samples * M] { ... }  // L0 边
    GraphLayer1[num_samples * M] { ... }  // L1 边
    GraphLayer2[num_samples * M] { ... }  // L2 边
}
```

#### 路由算法
```cpp
// 查询 q 的分区入口
std::vector<uint32_t> routeToPartitions(const float* query, int k=2) {
    // 1. meta-HNSW 贪心搜索，找到最近的 k 个采样点
    auto nearest_samples = meta_hnsw_search(query, k);

    // 2. 返回这些采样点所属的分区
    std::vector<uint32_t> partitions;
    for (auto [dist, sample_id] : nearest_samples) {
        uint32_t pid = meta_index.samples[sample_id].partition_id;
        partitions.push_back(pid);
    }

    return partitions;
}
```

---

### 2. 分区策略

#### 方案 A: K-means 聚类
```cpp
// 1. 对全量 1M 向量跑 K-means (K=200)
// 2. 每个向量获得 cluster_id (即 partition_id)
// 3. 每个 cluster 构建独立的 sub-HNSW
```

**优点**: 分区平衡，数据分布均匀
**缺点**: 需要预处理（离线），跨分区边丢失

#### 方案 B: BFS 割边
```cpp
// 1. 从 BFS 顺序开始，每 N 个节点切一个分区
// 2. 保证分区边界最小化跨分区边
// 3. 处理跨分区边：双向连接（sub-HNSW 的入口/出口）
```

**优点**: 不需要 K-means，利用现有 BFS 顺序
**缺点**: 分区大小可能不均衡

**推荐**: 方案 B（更易实现，复用现有 BFS 顺序）

#### 分区参数
```cpp
struct PartitionConfig {
    size_t num_partitions = 200;         // 分区数（可调）
    size_t vectors_per_partition = 5000; // 每分区 ~5K 向量
    size_t partition_size_mb = 2.5;      // ~2.5MB/分区
};
```

---

### 3. Sub-HNSW 存储

#### 存储格式
```cpp
// partitions_{pid}.bin
struct Partition {
    Header {
        uint32_t num_nodes;
        uint32_t entry_point;
        uint32_t max_level;
        uint32_t external_edges;  // 跨分区边数量
    }
    Nodes[num_nodes] {
        float[128] vector;
        uint16_t max_level;       // 该节点的最大层
    }
    GraphLayer0[num_nodes * M0] { ... }  // 内部边
    GraphLayer1[num_nodes * M1] { ... }
    ...
    ExternalEdges[external_edges] {  // 跨分区边（入口/出口）
        uint32_t local_node_id;
        uint32_t external_partition_id;
        uint32_t external_node_id;
    }
}
```

#### 连续存储
- 每个分区存储在连续磁盘区域
- 单次 RDMA_READ 读取整个分区（Header + Nodes + Graph + ExternalEdges）
- 读取后直接在内存中构建子图结构

---

### 4. 查询流程

#### 单查询流程
```
1. Meta-HNSW 贪心搜索 → 获取最近的 2 个分区入口 (sample_id → partition_id)
2. 确定需要加载的分区列表 [p1, p2]
3. 批量加载分区:
   - 检查分区是否在缓存
   - 提交 I/O 请求（并行读取 p1, p2）
   - 等待 I/O 完成
4. 本地搜索:
   - 在 p1 的 sub-HNSW 上运行阻塞版搜索
   - 在 p2 的 sub-HNSW 上运行阻塞版搜索
   - 合并结果，取 top-k
5. 返回 top-k 结果
```

#### 批量查询流程（I/O overlap）
```
Batch: [Q0, Q1, Q2, ..., QN]

for i in range(0, N):
    1. 路由 Q_i → 确定分区列表 [p1, p2]
    2. 预取 Q_{i+1} 的分区（如果 i < N-1）
    3. 等待 Q_i 的分区就绪（可能已在缓存）
    4. 本地搜索 Q_i
    5. 输出结果

关键：搜索 Q_i 时，Q_{i+1} 的分区在后台加载
```

---

### 5. 跨分区处理

#### 两种策略

**策略 A: 仅搜索路由到的分区**
- 优点：简单，无跨分区开销
- 缺点：recall 可能下降（跨分区边丢失）
- 预期 recall: 90-95%

**策略 B: 处理跨分区边**
- 优点：recall 更高（接近 100%）
- 缺点：需要动态加载相邻分区
- 实现：
  ```cpp
  while (!candidate_set.empty()) {
      auto [dist, node] = pop(candidate_set);
      // 检查是否跨分区边
      if (node.is_external) {
          // 预取并加载 adjacent_partition
          loadPartition(node.external_partition_id);
          // 继续
      }
      // 正常展开邻居
  }
  ```

**推荐**: 先实现策略 A（baseline），策略 B 作为优化

---

## 实现计划

### Phase 1: 离线分区构建（1-2天）
1. **K-means 聚类**（或 BFS 割边）
   ```bash
   python scripts/partition_dataset.py \
       --graph output/test1m_graph.bin \
       --vectors data/test_1m.fvecs \
       --k 200 \
       --output partitions/
   ```
2. **构建 sub-HNSW**
   ```bash
   for pid in {0..199}; do
       build_sub_hnsw partitions/${pid}.vectors partitions/${pid}.bin
   done
   ```
3. **构建 meta-index**
   ```bash
   build_meta_index \
       --samples samples_500.fvecs \
       --partitions partitions/ \
       --output meta_index.bin
   ```

### Phase 2: 运行时查询（2-3天）
1. **加载 meta-index**（启动时）
   ```cpp
   class DiskHNSW {
       MetaIndex meta_index_;
       PartitionCache partition_cache_;  // LRU, 10 个分区 (~25MB)
   };
   ```
2. **实现路由逻辑**
   ```cpp
   std::vector<uint32_t> DiskHNSW::routeToPartitions(const float* query);
   ```
3. **实现分区加载**
   ```cpp
   Partition* DiskHNSW::loadPartition(uint32_t pid);
   ```
4. **实现分区搜索**
   ```cpp
   std::vector<SearchResult> DiskHNSW::searchInPartition(
       const float* query,
       Partition* partition,
       size_t k
   );
   ```

### Phase 3: 批量查询优化（1天）
1. **查询间预取**
   ```cpp
   // 在 batchSearch 中
   for i in range(0, N):
       if (i + 1 < N) {
           auto next_partitions = routeToPartitions(queries[i+1]);
           prefetchPartitions(next_partitions);
       }
       // 搜索 queries[i]
   }
   ```
2. **分区缓存优化**
   ```cpp
   // LRU 策略，保留最近访问的 10 个分区
   PartitionCache cache(10);
   ```

### Phase 4: 评估与调优（1天）
1. **Recall 测试**（vs F0 baseline）
2. **QPS 测试**（vs F2-blocking baseline）
3. **参数调优**：
   - 分区数量（100/200/500）
   - 缓存大小（5/10/20 分区）
   - 路由参数（最近 1/2/3 个分区）

---

## 预期性能

### Recall
- 策略 A（不处理跨分区）: 90-95%
- 策略 B（处理跨分区）: 95-99%

### 延迟
- Meta-index 搜索: < 0.1ms（500 采样点，3 层）
- 分区加载: 1-5ms（2MB * 2 分区，NVMe 并行）
- 子图搜索: 0.5-2ms（5K 向量，内存 HNSW）
- **总计**: 2-7ms（vs F2-blocking 的 9.76ms）

### QPS
- 单查询: 140-500 QPS
- 批量查询（bs=4, 预取）: 200-600 QPS
- **提升**: 2-5x vs F2-blocking (100 QPS)

### 内存
- Meta-index: 256KB（常驻）
- 分区缓存: 25MB（10 个分区）
- **总计**: ~25MB（vs F2 的 376MB）

---

## 风险与缓解

### 风险 1: Recall 下降
- **缓解**: 实现策略 B（跨分区边），增加路由分区数（2→3）
- **兜底**: 如果 recall < 95%，考虑混合策略（分区 + fallback 逐 block 加载）

### 风险 2: 分区构建复杂
- **缓解**: 先用简单的 BFS 割边（无需 K-means）
- **备选**: 使用现有工具（faiss 的 IndexIVFFlat）

### 风险 3: 冷启动延迟
- **缓解**: 预加载常用分区（基于访问模式统计）
- **备选**: 启动时后台预加载

---

## 对比现有方案

| 维度 | F2-blocking | F2-nonblocking | C 方案（分区化） |
|------|-------------|----------------|------------------|
| Recall | 100% | 96.2% | 90-99% |
| QPS (单) | 100 | - | 140-500 |
| QPS (batch) | 73-100 | - | 200-600 |
| 内存 | 376MB | 376MB | 25MB |
| I/O 模式 | 逐 block（~70 次） | 逐 block（异步） | 整分区（1-2 次） |
| I/O overlap | ❌ | ❌（未解决） | ✅（查询间） |
| 实现复杂度 | 低 | 中 | 高（需构建分区） |

---

## 参考资料

- d-HNSW 论文: https://arxiv.org/abs/2603.13591
- 论文笔记: `/home/huawei/.openclaw/workspace/memory/papers/d-HNSW-disaggregated-memory.md`
- 当前 baseline: `RECALL100_BASELINE.md`

---

## 下一步

1. **确认方案**: 用户确认是否实现 C 方案
2. **开始 Phase 1**: 离线分区构建（K-means 或 BFS 割边）
3. **复用现有组件**:
   - BlockCache → PartitionCache
   - searchKnn → searchInPartition
   - GraphPrefetcher → PartitionPrefetcher

---

*设计方案版本: 1.0*
*日期: 2026-07-24*