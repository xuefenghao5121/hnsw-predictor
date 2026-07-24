# Recall 100% 基线状态记录

> 日期: 2026-07-24
> 状态: Recall 100% @ baseline (blocking search)
> 备份: src/disk_hnsw.cpp.recall100

---

## Benchmark 结果 (200 queries, SIFT1M)

| 配置 | Mean(ms) | P99(ms) | QPS | Recall@HNSW | Hit% |
|------|----------|---------|-----|-------------|------|
| F0: Full-mem | 0.86 | 2.18 | 1164 | 100% | 99.47% |
| F2-single: c1024+GP (blocking) | 9.97 | 16.50 | 100.3 | **100%** | 96.33% |
| F2-batch-4 | 13.62 | 13.62 | 73.4 | 100% | 96.33% |
| F2-batch-8 | 19.82 | 19.82 | 50.5 | 100% | 96.33% |
| F2-batch-16 | 19.60 | 19.60 | 51.0 | 100% | 96.33% |

**关键发现**:
- ✅ Recall 100%（所有 F2 配置）
- ❌ Batch 搜索 QPS 反而下降（bs=4: -27%, bs=8: -50%）
- ❌ 单查询 QPS 100.3，vs F0 的 1164（11.2x 慢）

---

## 当前代码状态

### 修改文件
- `src/disk_hnsw.cpp` (+273 行)
  - 新增 `searchLayer0NonBlocking()` 函数
  - `batchSearch()` 使用 `searchLayer0`（阻塞版）
  - Phase 2 entry block prefetch **已禁用**（导致 batch QPS 下降）

### 关键代码改动

#### batchSearch 使用阻塞版
```cpp
// Line 723
auto top_candidates = searchLayer0(entry_new_ids[i], ...);  // 阻塞版
```

#### Phase 2 prefetch 禁用
```cpp
// Line 709
// Phase 2: 批量预取所有 entry blocks - DISABLED (查询间预取更高效)
// if (graph_prefetch_enabled_ && graph_prefetcher_) { ... }
```

#### 查询间预取保留（Phase 3）
```cpp
// Line 745
uint32_t next_block = getBlockIdFast(entry_new_ids[i + 1]);
if (!cache_->isInCache(next_block)) {
    graph_prefetcher_->submitPrefetch({next_block}, true);
}
```

---

## 非阻塞搜索分析

### 根因
`searchLayer0NonBlocking` 导致 recall 下降（96.2%），根因：

1. **lowerBound 变化问题**:
   - 阻塞版: `waitForBlocks(pending_neighbors)` 期间 lowerBound 不变
   - 非阻塞版: deferred neighbors 就绪期间，其他 in-cache candidate 被处理，lowerBound 变化
   - 结果: 不同 deferred neighbors 用不同 lowerBound 过滤，导致剪枝错误

2. **savedLowerBound 修复失败**:
   - 尝试保存 defer 时的 lowerBound，用其过滤
   - 测试结果: 65 mismatch > baseline 的 31（更差）

3. **同步等待验证**:
   - deferred candidate 同步等待 + deferred neighbor 同步等待 = **0 mismatch** (100% recall)
   - 但这等价于阻塞版，没有 I/O overlap

### 已知无效方案
- ❌ 无条件加入 candidate_set（远距离邻居被展开，浪费）
- ❌ 保存 savedLowerBound（65 mismatch）
- ❌ 延迟 visited 标记（200 mismatch）
- ❌ Phase 2 entry block prefetch（batch QPS 下降）

---

## d-HNSW 论文相关

### d-HNSW 方案核心
1. **搜索前确定数据需求**：meta-HNSW（500 采样点的轻量级索引）→ 路由到 sub-HNSW 分区
2. **批量加载**：一次读取完整分区（图 + 向量，~2MB）
3. **查询级 I/O overlap**：搜索 Q0 时预取 Q1-QN 的分区

### 与当前方案对比

| 维度 | d-HNSW | HNSW-Predictor |
|------|--------|----------------|
| 远端介质 | RDMA 远端 DRAM (~2μs) | 本地 NVMe (~10-50μs) |
| 分区策略 | meta-HNSW → 500 sub-HNSW | BFS 重排 → 2225 blocks |
| 加载粒度 | 整个分区 (~2MB) | 256KB block |
| 每查询加载 | 1-2 个分区 (~2-4MB) | ~70 blocks (~17.9MB) |
| I/O overlap 级别 | 查询间 | 查询内（尝试） |
| Recall | 94% | 100% |
| 延迟 (SIFT1M@1) | 0.81ms | 9.76ms |

---

## 命令

### 备份当前代码
```bash
cd /home/huawei/hnsw-predictor
cp src/disk_hnsw.cpp src/disk_hnsw.cpp.recall100
```

### 运行 benchmark
```bash
cd /home/huawei/hnsw-predictor
make build/benchmark_overlap
./build/benchmark_overlap output/test1m_graph.bin output/test1m_bfs.bin output/test1m_blocks.bin output/test1m_route.bin data/test_1m.fvecs data/test_1m_query1k.fvecs data/test_1m_gt1k.bin 10 50 200
```

### 恢复到 baseline
```bash
cd /home/huawei/hnsw-predictor
cp src/disk_hnsw.cpp.recall100 src/disk_hnsw.cpp
```

---

## 下一步：C 方案设计（d-HNSW 风格分区化）

详见 `C方案设计-分区化索引.md`。

---

## 相关文件

- 论文笔记: `/home/huawei/.openclaw/workspace/memory/papers/d-HNSW-disaggregated-memory.md`
- 代码备份: `src/disk_hnsw.cpp.recall100`
- 设计文档: `hnsw-research/phase2-design.md`, `phase3-design.md`