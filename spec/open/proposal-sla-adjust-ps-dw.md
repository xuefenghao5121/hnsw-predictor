# Proposal: SLA 调整 -- Page Search / Dynamic Width 降级为实验性功能

> 提案日期: 2026-07-29
> Status: Pending
> 关联: proposal-fine-rerank-io-optimization.md, perf-20260729.md

## 背景

DEC-017 (Page Search) 和 DEC-019 (Dynamic Width) 经 2 轮修复后性能验证结果：

| 条款 | Recall | QPS | SLA 状态 | 根因 |
|------|--------|-----|---------|------|
| DEC-017 Page Search | 96.20% (+0.5pp) | 1832 (-11%) | QPS 违规 | C. 性能退化（位图开销 + 额外 L2 计算） |
| DEC-019 Dynamic Width | 95.70% (无变化) | 2066 (无变化) | 无效果 | B. 规范缺陷（PQ 搜索在 EF=100 时不收敛） |

## 根因分析

### DEC-017: 性能退化（C 类）

Page Search 的 recall 提升 0.5pp 是真实收益（偏移修复后页内向量被正确扫描）。QPS 下降 11% 来自：
- 每页额外 7 个 L2 距离计算（8 个向量 - 1 个候选）
- 位图 memset 和查询开销

这是**计算换 recall** 的合理 tradeoff，但当前 SLA 要求 QPS ≥ 2000，开启后 1832 不达标。

### DEC-019: 规范缺陷（B 类）

L1 契约假设"PQ 搜索后期候选集会收敛"，但实测发现：
- EF=100 时每次迭代都有新候选进入 top-K
- top-3 hash 和 lowerBound delta 从不稳定（hash_ok=0, lb_ok=0 所有 query）
- PQ 近似距离的浮点波动导致 top-K 持续抖动，直到搜索自然结束

这不是代码 bug，而是 L1 契约的假设错误：**PQ 粗筛在 EF=100 时不收敛**。

## SLA 调整提案

### 1. DEC-017 Page Search: 降级为实验性 SHOULD

- 保持 `level=should`（非 must）
- 新增 SLA 豁免：`PAGE_SEARCH=1` 时 QPS SLA 放宽为 ≥ 基线 85%（当前 1832/2051 = 89%，达标）
- recall SLA 不变（≥ 95%），实测 96.20% 达标
- 记录为"opt-in recall 提升功能，适合 recall 优先于速度的场景"

### 2. DEC-019 Dynamic Width: 标记为规范缺陷，暂不纳入 SLA

- 根因: B 类（规范缺陷），L1 契约假设 PQ 搜索收敛，实际不收敛
- 保留代码（默认关闭，零开销），但不纳入 SLA 考核
- 在 DEC-019 条款中追加"已知限制"说明
- 未来方向: 如果 REFINE_EF 降到 30-50，搜索可能收敛，DW 可能生效

### 3. DEC-018 Page Shuffle: 不变

- 未实现，仍为 SHOULD 级别，不受影响

## 新增 SLA 条款

```
{#CON-SLA-008} level=L1 [指标] 当 PAGE_SEARCH=1 时, QPS ≥ 基线 × 85%
  rationale: Page Search 是 opt-in recall 提升功能, 用 ~15% QPS 换 0.5pp recall。
  当 PAGE_SEARCH=0 时, 原始 SLA (QPS ≥ 2000) 不变。
```

## 修改的条款

| 条款 | 修改内容 |
|------|---------|
| DEC-017 | 追加"已知限制: QPS 开销 ~11-19%" |
| DEC-019 | 追加"已知限制: PQ 搜索在 EF≥100 时不收敛, DW 无效果" |
| CON-007 | 追加 SLA 豁免说明 |
