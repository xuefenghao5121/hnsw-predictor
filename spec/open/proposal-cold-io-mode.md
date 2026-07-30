# Proposal: 冷 I/O 模式 -- 通过 Page Cache 驱逐在 1M 规模制造真实 I/O 瓶颈

> 提案日期: 2026-07-29
> Status: Implemented on 2026-07-29
> 关联: proposal-fine-rerank-io-optimization.md, proposal-sla-adjust-ps-dw.md
> 关联条款: DEC-017, DEC-018, DEC-019, DEC-020

## 1. 动机

### 1.1 当前问题

DiskHNSW 在 1M 规模 + 512MB cgroup 下，vecblocks 文件（496MB）100% 被 OS page cache 覆盖。
Fine Rerank 的 4KB 页读取实际走的是热态 page cache，**零磁盘 I/O**。

这导致：
- OctopusANN 论文的 I/O 优化技术（Page Search / Page Shuffle / Dynamic Width）在 1M 规模无用武之地
- 无法展示 DiskHNSW 在真实冷 I/O 条件下的性能优势
- 无法验证"内存受限磁盘搜索"的核心叙事--因为磁盘 I/O 根本没发生

### 1.2 根因

cgroup v2 的 `memory.max` 限制匿名内存（RSS），但 page cache 作为 file-backed memory
可能不在 cgroup 记账范围内（首次读入发生在宿主机非 cgroup 上下文时）。

P1 阶段已验证：
- `mincore` 显示 vecblocks 496MB 100% 在 page cache
- 冷启动测试（512MB 严格限制）QPS 完全不变（2093 vs 热态 2067）
- 结论：**1M 规模的瓶颈不在 I/O，在 PQ 计算和图遍历**

### 1.3 机会

通过主动驱逐 page cache，在 1M 规模制造真实冷 I/O 条件：

1. **让论文技术立即生效**：Page Search 的 recall +0.5pp 在冷 I/O 下是净赚（每页多算 7 个向量 vs 多读一页）
2. **展示内存节省能力**：在真实冷 I/O 条件下仍达 95%+ recall，证明 PQ + Fine Rerank 架构的有效性
3. **为 P2（10M）铺路**：1M 冷 I/O 实验的结论可直接指导 10M 规模的架构决策

## 2. 提案条款

### 2.1 L1 契约：Page Cache 驱逐模式

```
{#DEC-021} level=L1 [系统] 当 EVICT_PAGE_CACHE=1 时，DiskHNSW MUST 在每次查询完成后
  对 vecblocks 文件调用 posix_fadvise(POSIX_FADV_DONTNEED)，驱逐 page cache。
  这确保下一次查询的 Fine Rerank 读取走真实磁盘 I/O，而非热态 page cache。
  
  refines: {DEC-009} (Fine Rerank 4KB 页粒度读)
  rationale: 在 1M 规模模拟 10M+ 规模的冷 I/O 条件，使 I/O 优化技术可被验证。
```

### 2.2 L1 契约：冷 I/O 基准测试模式

```
{#CON-SLA-010} level=L1 [指标] 当 EVICT_PAGE_CACHE=1 时：
  - Recall SLA 不变（≥ 95%）
  - QPS SLA 放宽为 ≥ 500（冷 I/O 条件下 QPS 自然下降）
  - RSS SLA 不变（≤ 300MB）
  rationale: 冷 I/O 下 Fine Rerank 每页读取 ~10-50μs（vs 热态 ~1μs），QPS 下降是预期行为。
  QPS ≥ 500 对应 < 2ms/query 延迟，仍为交互式可用。
```

### 2.3 L1 契约：冷 I/O 下 Page Search 重新评估

```
{#DEC-022} level=L1 [系统] 在冷 I/O 模式下，Page Search (DEC-017) SHOULD 重新评估：
  - 预期 recall 提升 ≥ 1pp（冷 I/O 下更多候选被精排，减少 PQ 误判）
  - 预期 QPS 下降 ≤ 5%（冷 I/O 下 L2 计算开销相对 I/O 可忽略）
  - 若达标，升级为默认开启
  rationale: 热态下 Page Search 的 QPS 开销 11% 主要来自 unordered_set/L2 计算；
  冷 I/O 下 I/O 延迟 10-50μs/页，额外 7 个 L2 计算（~0.4μs）占比 < 4%。
```

### 2.4 L1 契约：冷 I/O 下 Page Shuffle 优先级提升

```
{#DEC-023} level=L1 [系统] 在冷 I/O 模式下，Page Shuffle (DEC-018) 优先级从"推迟到 P2"
  提升为"P2 前置验证"。冷 I/O 下页内局部性直接决定 I/O 量，Page Shuffle 预期收益：
  - 页命中率从 ~12.5% 提升到 40-60%（论文数据）
  - I/O 量减少 25-30%
  - QPS 提升 20-30%
  rationale: 论文发现 Page Shuffle + Page Search 组合后减少 28.3% 页读取。
  冷 I/O 下每次页读取是真实磁盘 I/O，减少页读取直接提升 QPS。
```

## 3. 实施计划

### Phase 0: 实现 Page Cache 驱逐（1 天，低风险）

- 新增环境变量 `EVICT_PAGE_CACHE=0`（默认关闭）
- 在 `searchKnn()` 查询完成后，对 vecblocks fd 调用 `posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)`
- 驱逐粒度：整文件级别（`offset=0, len=0` 表示整个文件）
- 注意：只驱逐 vecblocks，不驱逐 blocks_64k（BlockCache 自己管理缓存）

### Phase 1: 冷 I/O 基线测量（0.5 天）

- 跑 `EVICT_PAGE_CACHE=1` 基线 benchmark（不开 PS/DW）
- 预期：QPS 从 2050 降到 200-800（取决于磁盘延迟）
- 确认冷 I/O 条件成立（通过 `PROFILE_FINE=1` 查看 io_1st/io_rest 计时）

### Phase 2: 冷 I/O 下 Page Search 评估（0.5 天）

- 跑 `EVICT_PAGE_CACHE=1 PAGE_SEARCH=1` benchmark
- 对比热态和冷态下 PS 的 recall/QPS 差异
- 决策：如果 recall ≥ 96% 且 QPS 下降 ≤ 5%，PS 升级为默认开启

### Phase 3: 冷 I/O 下 Dynamic Width 评估（0.5 天）

- 跑 `EVICT_PAGE_CACHE=1 DYNAMIC_WIDTH=1` benchmark
- 冷 I/O 下搜索路径可能更长（I/O 等待时间占比高），DW 收敛检测可能触发
- 决策：如果有效则保留，如果仍无效则正式标记为"不适用于当前架构"

### Phase 4: 冷 I/O 下 Page Shuffle 实现（3-5 天，视 Phase 2-3 结果）

- 如果 Phase 2 证明 Page Search 在冷 I/O 下有价值，实现 Page Shuffle
- 实现 `shuffle_vecblocks.cpp` 核心算法
- 重新生成 vecblocks 文件，跑冷 I/O benchmark
- 决策：如果 I/O 量减少 ≥ 20%，Page Shuffle 合入主线

## 4. 预期实验矩阵

| 配置 | 热态 QPS | 冷态 QPS（预期） | 冷态 recall |
|------|---------|----------------|------------|
| 基线 | 2050 | 200-800 | 95.70% |
| Page Search | 1832 | 200-780（≤5%下降） | ≥96.20% |
| Dynamic Width | 2066 | ? | 95.70% |
| PS + Shuffle | - | 250-1000（+20-30%） | ≥96% |

## 5. 风险分析

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| posix_fadvise 不生效（内核版本） | 低 | 高 | 先用 mincore 验证驱逐效果 |
| 冷态 QPS 过低（<100） | 中 | 中 | 调整 CON-SLA-010 阈值 |
| Page Search 在冷态下 QPS 下降仍 >5% | 低 | 低 | 保留为 opt-in，不默认开启 |
| Page Shuffle 实现复杂度超预期 | 中 | 中 | 先用简单贪心（BFS 序号 mod 8），再优化 |

## 6. 与论文的对齐

冷 I/O 模式让我们的实验条件与论文对齐：

| 维度 | 论文 OctopusANN | 我们（冷 I/O 模式） |
|------|----------------|-------------------|
| I/O 占查询延迟 | 70-90% | 预期 60-90%（Fine Rerank I/O 主导） |
| 内存限制 | 128GB server | 512MB cgroup（更严格） |
| 磁盘介质 | NVMe SSD | NVMe SSD |
| 图结构 | Vamana (DiskANN) | HNSW (hnswlib) |
| 粗筛 | PQ ADC | PQ ADC（相同） |
| 精排粒度 | 4KB 页 | 4KB 页（相同） |

**关键差异**：我们的内存限制（512MB）远比论文（128GB）严格，如果 DiskHNSW 在更严格的条件下仍达 95%+ recall，说明我们的架构优势更强。
