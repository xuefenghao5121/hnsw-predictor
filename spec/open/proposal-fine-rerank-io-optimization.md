> **STATUS**: Superseded by DEC-055 finding (page cache 均衡器效应). Revivable with O_DIRECT at P5.
# Proposal: Fine Rerank I/O 效率优化--Page Search + Page Shuffle + Dynamic Width

> 来源论文: "I/O Optimizations for Graph-Based Disk-Resident ANN Search: A Design Space Exploration" (VLDB 2026, Liang Li et al.)
> 论文系统: OctopusANN
> 提案日期: 2026-07-29
> Status: Implemented on 2026-07-29
> 状态: **L1 条款已剪切到 `decisions/adr.md` (DEC-017/018/019)，待审核**

## 已落地条款

| 条款 ID | 固定目录位置 | 标题 |
|---------|------------|------|
| DEC-017 | `decisions/adr.md` | Page Search for Fine Rerank |
| DEC-018 | `decisions/adr.md` | Page Shuffle for vecblocks |
| DEC-019 | `decisions/adr.md` | Dynamic Width for Phase A |

## 动机

### 论文核心发现

1. **Page Search**（读页时计算页内所有向量）单独效果弱，但与 Page Shuffle 组合后减少 28.3% 页读取
2. **Dynamic Width**（搜索后期收窄宽度）是独立收益第二大的技术，减少 45% I/O
3. **Page Shuffle + Page Search 的协同效应**：Shuffle 让图相邻节点共享 4KB 页，Page Search 让每次页读取全部利用

### 我们项目的差距

| 差距 | 当前行为 | 论文最优实践 | 影响 |
|------|---------|------------|------|
| Page Search | 只算候选向量 | 算页内全部 8 个向量 | 浪费 87.5% 已读数据 |
| Page Shuffle | BFS 重排在 64KB block 级 | 需 4KB 页级重排 | 页内局部性未优化 |
| Dynamic Width | 固定 efSearch 全程 | 后期收窄宽度 | 后期浪费 PQ 计算和图遍历 |

## 实施路径

### Phase 1: Page Search（1-2 天，低风险）
- 改动范围：`disk_hnsw.cpp` Fine Rerank 代码块
- 不需要改数据格式，纯搜索逻辑改动
- 预期：recall 95.70% -> 97-98%，QPS 微降或持平
- **验证标准**：recall ≥ 96%，QPS ≥ 1900

### Phase 2: Dynamic Width（2-3 天，中风险）
- 改动范围：`searchLayer0*()` 搜索循环
- 不需要改数据格式，新增收敛检测和宽度衰减逻辑
- 预期：QPS 提升 15-20%
- **验证标准**：recall ≥ 95.5%，QPS ≥ 2300

### Phase 3: Page Shuffle（3-5 天，中风险）
- 改动范围：新增 `shuffle_vecblocks.cpp` pipeline 工具 + `vec_route_table_` 更新
- 需要重新生成 vecblocks 文件
- 预期：配合 Page Search 后 QPS 额外提升 10-15%
- **验证标准**：recall ≥ 95.5%，QPS ≥ 2600

### 组合目标
- **当前基线**: 95.70% recall / 2067 QPS (1T) / 269MB RSS
- **组合目标**: ≥ 96% recall / ≥ 2600 QPS (1T) / ≤ 300MB RSS
