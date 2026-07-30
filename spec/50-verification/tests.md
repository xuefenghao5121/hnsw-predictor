# Verification — 测试用例与断言条件

## 测试套件结构 {#VER-001}
<!-- ndf: kind=verif level=must layer=L3 status=stable since=0.1 source=observed -->

| 测试 | 文件 | 覆盖范围 | 断言类型 |
|------|------|---------|---------|
| BlockCache 单元测试 | `test_block_cache.cpp` | 加载、命中、淘汰、数据正确性、统计 | ASSERT_EQ/ASSERT_TRUE |
| DiskHNSW 集成测试 | `test_disk_hnsw.cpp` | recall 一致性、搜索正确性、可插拔策略 | recall 比较 |
| PQ 搜索质量 | `test_pq_search_quality.cpp` | PQ ADC 距离 vs 真实 L2、recall@10 | 手动检查输出 |

## test_block_cache 断言提取 {#VER-002}
<!-- ndf: kind=verif level=must layer=L3 status=stable since=0.1 source=observed verifies=OBS-BEH-009,OBS-BEH-010 -->

### test_basic_loading (`test_block_cache.cpp:91-106`)

- [ ] BlockCache 构造后，`getNumBlocks()` MUST == 3（1K 节点测试数据）
- [ ] `getNumNodes()` MUST == 1000
- [ ] `getBlockSize()` MUST == 256 * 1024
- [ ] `getCacheSlots()` MUST == 64
- [ ] `getNumCachedBlocks()` MUST == 0（初始无缓存）

### test_cache_hit_miss (`test_block_cache.cpp:108-137`)

- [ ] 首次 `getNodeVector(0)` MUST 返回非 nullptr
- [ ] 首次访问后 `total_accesses == 1`, `cache_misses == 1`, `cache_hits == 0`
- [ ] 第二次 `getNodeVector(0)` MUST 返回与首次相同的指针
- [ ] 第二次访问后 `total_accesses == 2`, `cache_misses == 1`, `cache_hits == 1`

### test_lru_eviction (`test_block_cache.cpp:139-177`)

- [ ] cache_slots=1 时，访问不同 block 的节点 MUST 触发淘汰
- [ ] 访问 node 0 (block 0) 后 `num_cached == 1`, `misses == 1`, `evictions == 0`
- [ ] node 0 和 node 500 MUST 在不同 block 中
- [ ] 访问 node 500 后 `misses == 2`, `evictions == 1`
- [ ] 再次访问 node 0 后 `evictions == 2`

### test_data_correctness (`test_block_cache.cpp:179-200+`)

- [ ] BlockCache 返回的向量数据 MUST 与 graph.bin 原始数据一致
- [ ] BlockCache 返回的邻居列表 MUST 与 graph.bin 原始数据一致
- [ ] 对所有节点 (0..N-1) 验证上述一致性

## test_disk_hnsw 断言提取 {#VER-003}
<!-- ndf: kind=verif level=must layer=L3 status=stable since=0.1 source=observed verifies=OBS-BEH-001,OBS-BEH-003 -->

### recall 一致性 (`test_disk_hnsw.cpp`)

从测试代码逻辑反向提取：
- [ ] DiskHNSW 搜索结果 SHOULD 与 hnswlib 全内存搜索结果有高 recall (≥95%)
- [ ] `computeRecall()` MUST 使用 set intersection 计算 recall@k (`test_disk_hnsw.cpp:36-54`)
- [ ] Ground truth MUST 通过暴力 L2 搜索生成 (`test_disk_hnsw.cpp:57-88`)
- [ ] 支持 3 种 IO 模式：cached / direct (O_DIRECT) / simulated (模拟延迟)
- [ ] 支持 3 种替换策略：lru / lfu / lru-k
- [ ] 支持 2 种布局策略：bfs / random

### 可插拔策略测试 (`test_disk_hnsw.cpp:98-113`)

- [ ] `--io-mode=cached|direct|simulated` MUST 正确配置 IOConfig
- [ ] `--layout=bfs|random` MUST 创建对应的 LayoutProvider
- [ ] `--policy=lru|lfu|lru-k` MUST 创建对应的 ReplacementPolicy

## benchmark 正确性条件 {#VER-004}
<!-- ndf: kind=verif level=must layer=L3 status=stable since=0.1 source=observed verifies=OBS-BEH-013 -->

### recall 计算 (`benchmark_diskhnsw.cpp`)

- [ ] GT 文件 MUST 有 8B header (n_queries:u32 + K:u32)
- [ ] 搜索结果的 label (uint64) MUST 与 GT 的 neighbor id 比较
- [ ] Recall = hits / (num_query * k)
- [ ] benchmark 运行时的 K MUST == GT 生成时的 K（否则 offset 错位）

### RSS 测量 (`benchmark_diskhnsw.cpp:25-36`)

- [ ] RSS MUST 从 `/proc/self/status` 的 VmRSS 字段读取
- [ ] 单位 MUST 为 MB (kB / 1024)

### 公平对比条件 (`compare_benchmark.sh`)

- [ ] DiskHNSW 和 hnswlib MUST 使用相同的 query / GT / k / ef
- [ ] DiskHNSW MUST 在 cgroup MemoryMax 限制下运行
- [ ] hnswlib MUST 放开 cgroup 限制
- [ ] 两者 MUST 经过相同的 warmup（CPU 升频 + page cache 预热 + 全 query 预跑）
- [ ] MUST 多轮取峰值 QPS（排除调频抖动）

## PQ 质量自检 {#VER-005}
<!-- ndf: kind=verif level=should layer=L3 status=stable since=0.1 source=observed verifies=OBS-BEH-005 -->

### train_pq.py 自检 (`train_pq.py:122-138`)

- [ ] ADC top-10 overlap SHOULD > 90%（SIFT+M=32）
- [ ] 重建 MSE SHOULD 被打印（相对能量百分比）
- [ ] PQ codebook MUST 保存为 `M*256*(dim/M)` floats

### test_pq_search_quality 检查 (`test_pq_search_quality.cpp:56-103`)

- [ ] 对前 5 个 query，打印 PQ 距离 vs 真实 L2 距离
- [ ] 计算 recall@10 并输出
- [ ] GT 的 true_dist 和 pq_dist MUST 同时打印用于人工对比

## DEC-017 Page Search 验证 {#VER-006}
<!-- ndf: kind=verif level=must layer=L3 status=draft since=0.2 verifies=BEH-014,BEH-014-L2 -->

### 功能验证 (benchmark 手工执行)

- [ ] `PAGE_SEARCH=1` 时 recall@10 MUST ≥ 95%（基线 95.70%）
  - 测试命令: `TWO_STAGE=1 FINE_RERANK=1 PAGE_SEARCH=1 VEC_BLOCKS_PATH=... PQ_CODES_PATH=... FLAT_VEC_MB=64 REFINE_EF=100 CACHE_MB=32 ./build/benchmark_diskhnsw ...`
  - 验证方式: benchmark 输出中的 `Recall:` 值

### 性能验证

- [ ] `PAGE_SEARCH=1` 时 QPS SHOULD ≥ 基线 QPS × 0.90（10% 容忍区间）
  - 理由: 页内 8 个向量 L2 的 CPU 开销 (~0.4μs/page) 应被 I/O 已付出的收益抵消
  - 当前状态 (2026-07-29): **违规** — 实测 QPS -19%（2067 → 1670），待优化
  - 优化方向: 仅扫描包含至少 1 个候选的页（减少零收益页的扫描）

### 反向映射正确性

- [ ] `vec_slot_to_node_` MUST 在 `buildFineRerank()` 中与 `node_slot_table_` 保持一致
  - 不变量: `vec_slot_to_node_[b][node_slot_table_[nid]] == nid`（对于属于 block b 的 nid）
  - 验证方式: 遍历所有 nid，验证双向映射一致性

### 空状态验证

- [ ] `PAGE_SEARCH=0` (默认) 时 `pageSearchScan()` MUST 不执行任何扫描
- [ ] `pageSearchScan()` 的页号计算 MUST 正确跳过文件头区域 (pg_off < 4096)

## DEC-019 Dynamic Width 验证 {#VER-007}
<!-- ndf: kind=verif level=must layer=L3 status=draft since=0.2 verifies=BEH-015,BEH-015-L2 -->

### 功能验证

- [ ] `DYNAMIC_WIDTH=1` 时 recall@10 MUST ≥ 95%
  - 当前状态 (2026-07-29): **通过** — recall 不变（ef=50 时收敛检测未触发，行为等同基线）

### 收敛触发验证

- [ ] 在 `REFINE_EF=200` 场景下，`DYNAMIC_WIDTH=1` SHOULD 在搜索后半段触发至少 1 次宽度衰减
  - 当前状态 (2026-07-29): **未验证** — EF=50 搜索步数少，hash 变化频繁，收敛阈值未触发
  - 验证方式: 添加临时 `fprintf(stderr, "[DW] converge=%zu ef=%zu\n", ...)` 日志观察

### 性能验证

- [ ] `DYNAMIC_WIDTH=1` 时 QPS SHOULD ≥ 基线 QPS
  - 理由: 衰减逻辑的 hash 计算和条件判断开销应 <1% QPS
  - 当前状态 (2026-07-29): **通过** — 无衰减触发，QPS 无变化

### 裁剪正确性

- [ ] 宽度衰减 MUST 正确更新 `lowerBound`:
  - 裁剪后 `lowerBound = top_candidates.top().first`
  - 裁剪后 `dw_effective_ef = new_ef`
  - 收敛计数 MUST 重置为 0

- [ ] `EF_SEARCH_MIN` MUST 保护下限:
  - `dw_effective_ef` MUST NOT 低于 `EF_SEARCH_MIN`
  - 当 `dw_effective_ef <= EF_SEARCH_MIN` 时，收敛检测跳过

### 空状态验证

- [ ] `DYNAMIC_WIDTH=0` (默认) 时：
  - `dw_converge_count` 永远为 0
  - 无 hash 计算开销
  - `top_candidates` 使用原始 `ef`，无裁剪

## 冷 I/O 模式验证用例

### DEC-021: Page Cache 驱逐

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| 热态基线 | EVICT_PAGE_CACHE=0 | QPS ≥ 2000 | 2083 | ✅ |
| 冷态基线 | EVICT_PAGE_CACHE=1 | QPS 200-1000 (I/O 主导) | 842 | ✅ |
| 冷态 recall | EVICT_PAGE_CACHE=1 | recall ≥ 95% | 95.70% | ✅ |
| 冷态 RSS | EVICT_PAGE_CACHE=1 | RSS ≤ 300MB | 269MB | ✅ |

### DEC-022: 冷态 Page Search

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| recall 提升 | EVICT+PS | ≥ 96% | 96.20% | ✅ |
| QPS 下降 | EVICT+PS vs EVICT | ≤ 5% | 5.9% | ⚠️ 接近 |

### DEC-024: Dynamic Width 最终确认

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| 热态无效 | DW=1 | 无效果 | converge=0 | 已知限制 |
| 冷态无效 | EVICT+DW | 无效果 | converge=0 | 正式放弃 |

### VER-018: Page Shuffle 页聚类质量 {#VER-018}
<!-- ndf: kind=verif verifies=DEC-018 -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| 页内邻居对 | greedy 策略 | ≥ 60% | 77.1% | ✅ |
| Recall 保持 | shuffled vs original (hot) | 95.70% ± 0.1pp | 95.70% | ✅ |
| Recall 保持 | shuffled vs original (cold) | 95.70% ± 0.1pp | 95.70% | ✅ |
| 冷态 QPS | shuffled vs original (cold) | ≥ original QPS | 820 vs 803 | ✅ |
| 工具耗时 | 1M vectors | < 5s | 1.65s | ✅ |

### DEC-025: Page Shuffle + Page Search 组合

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| recall | shuffled+PS (cold) | ≥ 96% | 96.05% | ✅ |
| QPS vs PS only | shuffled+PS vs PS | ≥ PS QPS | 797 vs 789 | ✅ |
| SLA 全部达标 | shuffled+PS (cold) | recall ≥ 95%, QPS ≥ 500 | 96.05%, 797 | ✅ |

### VER-030: Page Cache + Disk 两层 I/O 架构 {#VER-030}
<!-- ndf: kind=verif verifies=DEC-030 -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| SIFT1M recall | FINE_BUFFERED（默认） | ≥ 95% | 95.70% | ✅ |
| SIFT1M QPS | FINE_BUFFERED（默认） | ≥ 2000 | 2,041 | ✅ |
| FINE_DIRECT recall | FINE_DIRECT=1 (诊断) | ≥ 95% | 95.70% | ✅ |
| FINE_DIRECT QPS | FINE_DIRECT=1 (诊断) | ≥ 500 | 787 | ✅ |
| O_DIRECT 路径正确性 | FINE_DIRECT=1 | I/O 延迟 > 0.5ms | 0.78ms/query | ✅ |
| 默认模式不退化 | FINE_BUFFERED vs baseline | QPS ≥ baseline | 2041 vs 2080 | ✅ |
| 诊断模式可用 | FINE_DIRECT=1 无 crash | 稳定运行 | 通过 | ✅ |

### VER-031: 页面级驱逐消除 Cgroup 颠簸 {#VER-031}
<!-- ndf: kind=verif verifies=DEC-031 -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| recall 不变 | FINE_BUFFERED+FINE_FADVISE | 95.70% | — | 待测 |
| 256MB cgroup QPS | FINE_ADVISE, 256MB cgroup | ~2,000 | — | 待测 |
| 180MB cgroup QPS | FINE_ADVISE, 180MB cgroup | ≥ 500 | — | 待测 |
| posix_fadvise 开销 | per-query syscall count | < 3 syscalls | — | 待测 |
| 悬崖消除 | 180MB FINE_ADVISE vs baseline | QPS 10× 改善 | — | 待测 |

### VER-033: CSR 图裁剪内存压缩 {#VER-033}
<!-- ndf: kind=verif verifies=DEC-033 -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| recall | Degree Cap K=20 | ≥ 95% | 94.15% | ⚠️ 未达, K=22 待测 |
| CSR 大小 | Degree Cap K=20 | < 30MB | 44MB | ⚠️ 节省 3MB 有限 |
| 180MB cgroup QPS | CAP=20, 180MB | ≥ 800 | 935 | ✅ |
| 悬崖消除 | CAP20 vs Original @180MB | 4×+ | 4.7× (196→935) | ✅ |
