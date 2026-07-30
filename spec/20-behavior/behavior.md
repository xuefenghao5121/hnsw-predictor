# Behavior — 函数、条件分支、状态机

## 搜索流程状态机 {#BEH-001}
<!-- ndf: kind=req level=must layer=L1 status=stable since=0.1 source=observed -->

DiskHNSW 搜索 MUST 遵循以下状态转移：

```
[查询到达] → Phase0: greedyDescent (L_high → L1)
           → PhaseA: searchLayer0 (PQ ADC, ef=REFINE_EF or ef_search)
           → PhaseB: FineRerank (4KB page reads, exact L2)
           → [返回 top-K]
```

**Phase B 精排子状态机** (from `disk_hnsw.cpp:1688-1913`):

```
[Phase A 候选] → 遍历候选
  ├─ cache hit (block cache or flat_vec_cache) → 立即算 L2 → consider()
  └─ cache miss
       ├─ FINE_PREAD=1 → pread 批量 4KB 读 → consider() + putFlatVector()
       └─ FINE_PREAD=0 → io_uring 批量提交 4KB 读
            ├─ FINE_MERGE=1 → 合并相邻页 8KB
            └─ FINE_MERGE=0 → 单页 4096B
            → waitCompletion + reap → consider() + putFlatVector()
```

## 搜索模式分支 {#BEH-002}
<!-- ndf: kind=req level=must layer=L1 status=stable since=0.1 source=observed -->

`searchKnn()` 在 `disk_hnsw.cpp:1601` 中 MUST 根据环境变量选择搜索策略：

| 环境变量 | 模式 | 函数 |
|----------|------|------|
| `TWO_STAGE=1` + PQ | 两阶段搜索 | `searchLayer0()` (ef=REFINE_EF) → FineRerank |
| `BEAM_WIDTH>0` | Beam search | `searchLayer0Beam()` |
| `NONBLOCK=1` | 非阻塞 I/O overlap | `searchLayer0NonBlocking()` |
| `BATCH_IO_N>0` | 批量并行 I/O | `searchLayer0BatchIO()` |
| 默认 | 标准 best-first | `searchLayer0()` (ef=ef_search) |

## Layer 0 搜索核心循环 {#BEH-003}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

所有 `searchLayer0*` 变体 MUST 使用相同的核心循环结构（`disk_hnsw.cpp:390-820`）：

```
while candidate_set not empty:
  pop candidate with min distance
  if candidateDist > lowerBound AND |top_candidates| == ef: break
  获取邻居列表 (CSR in-mem > CachedBlock)
  对每个未访问邻居：
    ├─ PQ mode → pqDistance() (ADC 查表)
    ├─ PQ_HYBRID=1 + cache hit → exact L2
    ├─ in-cache → exact L2
    └─ miss → pending list → batch wait I/O → exact L2
    如果距离 < lowerBound 或 |top_candidates| < ef:
      加入 candidate_set 和 top_candidates
      更新 lowerBound = top_candidates.top().first
```

## 贪心下降 {#BEH-004}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

`greedyDescent()` (`disk_hnsw.cpp:343-384`) MUST:
1. 从 `entry_point` 开始，从 `max_level` 向下到 Layer 1
2. 每层：遍历当前节点在该层的邻居，如果找到更近的邻居就移动过去
3. 重复直到在当前层没有更近的邻居
4. 完全在内存中操作（上层向量 + 上层邻接表），零 I/O

## PQ 距离计算 {#BEH-005}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

`pqDistance()` (`disk_hnsw.cpp:301-337`) MUST 分两条路径：

1. **查表快路径**（`pq_dist_table_` 非空时）：
   - 展开循环 4 路并行（m+0..m+3 同时查表累加）
   - 退化为 `M * ksub` 表查值 + 加法

2. **ADC fallback**（距离表未构建时）：
   - 直接计算 `|query_sub - centroid|²`
   - 每个子向量独立计算后累加

## PQ 距离表构建 {#BEH-006}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

`buildPqDistTable()` (`disk_hnsw.cpp:244-299`) MUST 分支：
- **`dsub == 4`**: AVX2 路径，一次处理 2 个 centroid (8 floats)，用 `_mm256_sub_ps` + `_mm256_mul_ps` + `_mm_hadd_ps` 水平加法
- **`dsub != 4`**: 标量路径，三重循环 (M × ksub × dsub)
- 结果存入 **thread_local** `pq_dist_table_`，保证多线程安全

## Fine Rerank 4KB 页计算 {#BEH-007}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

Fine Rerank (`disk_hnsw.cpp:1688-1913`) MUST 按以下公式计算 vecblocks 文件偏移：

```
off = 4096 + b * vec_block_size_ + block_data_offset_[b] + node_slot_table_[nid] * dim * sizeof(float)
page0 = off >> 12
oip = off & 4095
cross = (oip + dim * sizeof(float)) > 4096  // 跨页检测
```

关键：MUST 使用 `vec_route_table_[nid]`（而非 `route_table_[nid]`）获取 vecblocks 块号。

## 邻接表访问策略 {#BEH-008}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

`getInMemNeighbors()` (`disk_hnsw.h:333`) MUST 分三条路径：

1. **CSR 内存邻接表**（`has_inmem_adjacency_ == true`）：
   - 非压缩：直接返回 `&adj_csr_neighbors_[adj_csr_offsets_[new_id]]`
   - 压缩：解码 `adj_csr_compact_` 从 `adj_csr_byte_offsets_[new_id]` 到 `csr_decode_buf_` (thread_local)
2. **BlockCache fallback**: `CachedBlock::getNeighbors()`

## Block 解析分支 {#BEH-009}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

`BlockCache::parseBlock()` (`block_cache.cpp:395-557`) MUST 检测并分支：

1. **Vec-only 格式**（`FLAG_VEC_ONLY` 位设置）：16B header + node_ids + vectors，无邻接表
2. **PQ 模式**（`data_offset == 0`）：BlockHeader + node_ids + adj_lists，无向量
3. **标准格式**（`data_offset > 0`）：
   - 压缩格式（`FLAG_NEIGHBOR_DELTA_VARINT`）：解码 delta+varint 邻居
   - 原始格式：uint32 数组邻居

## 缓存替换状态转移 {#BEH-010}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

BlockCache MUST 在缓存满时通过以下流程淘汰：

```
getBlockByNodeId / getBlockById:
  lock(mutex_)
  if cache_map_.find(block_id) != end:
    stats_.cache_hits++
    policy_->onAccess(block_id)
    标记 was_accessed (预取准确率)
    return &block
  else:
    stats_.cache_misses++
    while cache_map_.size() >= cache_slots_:
      victim = policy_->selectVictim()
      if victim == UINT32_MAX: break
      结算预取准确率 (was_prefetched → useful/wasted)
      cache_map_.erase(victim)
      policy_->onRemove(victim)
      invalidateFlatBlockCache(victim)
      stats_.evictions++
    loadBlockFromDisk(block_id)
    parseBlock(block)
    cache_map_.emplace(block_id, block)
    policy_->onInsert(block_id)
    populateFlatCache(block)
    return &block
```

## 跨页向量拼接 {#BEH-011}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

当向量跨越 4KB 页边界时（`slot % 8 == 6`，SIFT dim=128, data_offset=520），Fine Rerank MUST：
1. 从 page0 读取 `4096 - oip` 字节
2. 从 page1 读取 `dim * sizeof(float) - first` 字节
3. 拼接到临时 buffer `tmp_vec[512]` 或 `tmp_vec_pread[512]`
4. 然后计算精确 L2 距离

## 投机预取节流 {#BEH-012}
<!-- ndf: kind=req level=may layer=L2 status=stable since=0.1 source=observed -->

`SPEC_PREFETCH=1` 时，PQ 模式下每 16 个 top_candidates 更新触发一次投机预取（`disk_hnsw.cpp:688-705`）：收集当前 top_candidates 中不在缓存的 blocks，批量提交 io_uring 预取。使用 `spec_pf_counter_` 节流。

## QPS/Recall/RSS 指标提取 {#BEH-013}
<!-- ndf: kind=req level=must layer=L2 status=stable since=0.1 source=observed -->

benchmark MUST 在 `benchmark_diskhnsw.cpp` 中：
1. Warmup: CPU spin + 全 query 预跑一轮
2. 计时搜索: `high_resolution_clock` 测总耗时 → QPS = N / total_s
3. Recall: 搜索结果 ∩ GT / K
4. RSS: 读取 `/proc/self/status` VmRSS
5. 多轮取峰值（排除调频噪声）

## Page Search 行为 {#BEH-014}
<!-- ndf: kind=req level=L1 status=draft since=0.2 source=deduced -->
<!-- refines: DEC-017 -->

当 `PAGE_SEARCH=1` 时，Fine Rerank MUST 在读取每个 4KB 页后，扫描页内所有向量计算精确 L2 距离并插入候选集。

**L1 契约**：
- 页内所有向量 MUST 被计算，而非仅候选向量
- "邻居向量"（同页但非候选）只做距离计算，MUST NOT 入图遍历队列
- MUST 使用 `vec_slot_to_node_[block][slot]` 反向映射定位页内向量
- 环境变量 `PAGE_SEARCH=0`（默认）时 MUST 完全跳过此行为，零额外开销

> 当前状态: recall +0.5pp (96.20%), QPS -11% (1832). 降级为实验性 SHOULD, 见 [[DEC-020]] [[CON-SLA-008]].

### Page Search 扫描实现 {#BEH-014-L2}
<!-- ndf: kind=req level=L2 status=draft since=0.2 source=observed -->
<!-- refines: BEH-014 -->

`pageSearchScan(pg, page_data, page_len)` 在 `disk_hnsw.cpp:1757` 中定义，MUST 按以下步骤扫描页内所有向量：

1. **页位置计算** (`disk_hnsw.cpp:1759-1772`):
   - 从页号 `pg` 计算文件字节偏移 `pg_off = pg << 12`
   - 排除 4096B 文件头区域（`pg_off < 4096` 直接返回）
   - 计算所属 vecblock: `b = (pg_off - 4096) / vec_block_size_`
   - 计算向量区起始: `blk_start = 4096 + b * vec_block_size_ + block_data_offset_[b]`
   - 计算首个向量 slot: `first_slot = (pg_off - blk_start) / slot_size`
   - 计算页内 slot 数: `slots_in_page = page_len / slot_size`
   - 其中 `slot_size = dim * sizeof(float)` (SIFT=512B)

2. **Slot 遍历** (`disk_hnsw.cpp:1773-1782`):
   ```cpp
   for (uint32_t s = first_slot; s < first_slot + slots_in_page && s < max_slot; s++) {
       uint32_t nid = vec_slot_to_node_[b][s];   // DEC-017: slot→node 反向映射
       if (page_considered.count(nid)) continue;  // 去重 (候选可能已计算)
       const float* vec = reinterpret_cast<const float*>(page_data + byte_off);
       consider(nid, vec);                         // 计算 L2 + 插入 refined 候选集
       cache_->putFlatVector(nid, vec);           // 回填热向量 cache
       ps_extra_considered++;
   }
   ```

3. **去重机制** (`disk_hnsw.cpp:1745-1752`):
   - 全局 `std::unordered_set<uint32_t> page_considered`
   - `consider()` lambda 内部检查 `page_considered.count(nid)`，避免重复计算
   - 候选向量已在进入 Page Search 前通过 `consider()` 计算，其 node_id 已标记

4. **反向映射构建** (`disk_hnsw.cpp:2206,2225`):
   - `buildFineRerank()` 中新增 `vec_slot_to_node_.resize(num_blocks)`，然后逐 block 存入 `ids[0..cnt-1]`
   - 索引语义: `vec_slot_to_node_[b][slot] = node_id`，O(1) 访问

5. **调用点**:
   - **pread 路径** (`disk_hnsw.cpp:1828-1833`): 每页 pread 完成后立即 `pageSearchScan(pg, buf.get(), 4096)`
   - **io_uring 路径** (`disk_hnsw.cpp:1965-1971`): 对去重页集 `scanned_pages` 逐一调用 `pageSearchScan`

6. **统计输出** (`disk_hnsw.cpp:2007-2009`):
   - `PROFILE_FINE=1` 时额外输出 `ps_extra` (每 query 额外考虑向量数) 和 `ps_hits` (命中 top-K 数)

## Dynamic Width 行为 {#BEH-015}
<!-- ndf: kind=req level=L1 status=draft since=0.2 source=deduced -->
<!-- refines: DEC-019 -->

当 `DYNAMIC_WIDTH=1` 时，Phase A 搜索 MUST 使用自适应 efSearch 宽度：搜索初期使用全宽度，候选集收敛后逐步收窄。

**L1 契约**：
- 收敛检测: 连续 `DW_CONVERGE_HOP` 跳 top-K 无变化时触发收窄
- 收窄策略: efSearch 按几何衰减 `× DW_DECAY`，下限 `EF_SEARCH_MIN`
- `DYNAMIC_WIDTH=0`（默认）时 MUST 使用固定 efSearch，行为与基线完全一致
- 收窄后 MUST 更新 lowerBound 并裁剪 top_candidates

> 当前状态: 实现完成但性能验证无效果（EF=50 搜索路径短，收敛阈值未触发），待更大 EF 场景验证。

### Dynamic Width 收敛检测实现 {#BEH-015-L2}
<!-- ndf: kind=req level=L2 status=draft since=0.2 source=observed -->
<!-- refines: BEH-015 -->

收敛检测与宽度衰减在 `searchLayer0()` 的 while 循环末尾 (`disk_hnsw.cpp:826-848`) 执行：

1. **参数配置** (`disk_hnsw.cpp:447-453`):
   - `DYNAMIC_WIDTH=1` 开启（默认 0 关闭）
   - `DW_CONVERGE_HOP` 收敛跳数（默认 10）
   - `DW_DECAY` 衰减率（默认 0.75，几何衰减）
   - `EF_SEARCH_MIN` 最小 efSearch（默认 32）

2. **状态变量** (`disk_hnsw.cpp:454-457`):
   ```cpp
   size_t dw_effective_ef = current_ef;   // 当前有效宽度，初值=传入的 ef
   size_t dw_converge_count = 0;          // 连续无变化计次
   uint64_t dw_prev_topk_hash = 0;        // 上一轮 top_candidates 签名
   ```

3. **变化检测** (`disk_hnsw.cpp:829-833`):
   - 对 `top_candidates` 做快照拷贝 `tc_copy`
   - 取前 3 个元素的 `node_id` 做 XOR hash: `hash ^= (uint64_t)node_id << (h * 21)`
   - 与 `dw_prev_topk_hash` 比较：相同则 `dw_converge_count++`，不同则重置为 0

4. **宽度衰减** (`disk_hnsw.cpp:836-844`):
   ```cpp
   if (dw_converge_count >= kDwConvergeHop) {
       size_t new_ef = max(kEfSearchMin, dw_effective_ef * kDwDecay);
       while (top_candidates.size() > new_ef) top_candidates.pop();  // 裁剪
       if (!top_candidates.empty()) lowerBound = top_candidates.top().first;  // 更新
       dw_effective_ef = new_ef;
       dw_converge_count = 0;  // 重置计数等待下次收敛
   }
   ```

5. **生效机制**:
   - 裁剪 `top_candidates` 后 `lowerBound` 自动收紧
   - 下一轮 `candidateDist > lowerBound` 条件更严格，自然过滤弱候选
   - 不做 `ef` 全量 text 替换——仅通过外部裁剪 + lowerBound 传导实现选择性增强

6. **适用 scope**: 当前仅在 `searchLayer0()` 中生效（TwoStage 路径调用）。其他变体（Beam/NonBlock/BatchIO）暂未集成。
   - 入口: `searchKnn()` → `kTwoStage` 分支 → `searchLayer0(entryNewId, query, ef_coarse, visited)` → 收敛检测在 while 末尾
   - 不适用: `searchLayer0Beam/NonBlocking/BatchIO` 的直接调用路径


## Page Cache 驱逐行为 {#BEH-016}
<!-- ndf: kind=req level=L1 status=stable since=0.3 source=deduced -->
<!-- refines: DEC-021 -->

当 `EVICT_PAGE_CACHE=1` 时，DiskHNSW MUST 在每次 `searchKnn()` 返回前对 vecblocks 文件
调用 `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`，驱逐整个文件的 page cache。

**L1 契约**：
- 驱逐目标: 仅 vecblocks fd，MUST NOT 驱逐 blocks_64k（BlockCache 自管理）
- 时机: 每次查询完成后（return result 前）
- `EVICT_PAGE_CACHE=0`（默认）时 MUST 零开销
- 驱逐后下一查询的 Fine Rerank 读取走真实磁盘 I/O

> 实测: 热态 2083 QPS -> 冷态 842 QPS（-60%），I/O 占比 ~60%。
> 冷态 SLA: QPS ≥ 500（见 [[CON-SLA-010]]）。
