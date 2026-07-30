# Q-002: Fine Rerank 学习式剪枝的可行性 {#Q-002}
<!-- ndf: kind=open blocks=DEC-028,DEC-017,DEC-020 source=deduced date=2026-07-29 -->

## 问题

HELMSMAN 的 LLSP 用 GBDT 预测最优 nprobe 层级，在聚类 ANNS 上实现 1.1-1.6× 吞吐提升。
DiskHNSW 的 Fine Rerank 能否用类似思路自适应调节 REFINE_EF 和 Page Search 开关？

## 子问题

1. **特征工程**：用什么特征预测 per-query 的最佳策略？
   - 候选特征：query 向量的 PCA 分量、PQ 粗筛距离分布（min/median/max/std）、top-k
   - HELMSMAN 用 query vector + centroid distances，我们对应什么？

2. **标签生成**：什么是"最佳策略"？
   - 最小化 I/O 页数 + 满足 recall ≥95% 的 (refine_ef, enable_ps) 组合
   - 需要 brute-force 扫描所有参数组合来生成训练标签

3. **模型选择**：GBDT (LightGBM) vs 小型 MLP vs 规则表？
   - 推理延迟必须 <1μs（不能成为瓶颈）
   - GBDT 树深度 ≤5 可实现亚微秒推理

4. **训练数据**：10M 规模收集的 per-query profiling 数据是否足够？
   - 需要多少 query 样本？
   - 训练集/测试集如何划分（按 query 还是按数据集）？

## 相关决策

- [[DEC-028]] — 学习式剪枝探索方向（P2.5，低优先级）
- [[DEC-017]] — Page Search 固定策略
- [[DEC-020]] — PS/DW SLA 调整

## 前置依赖

- P2（10M）的 per-query profiling 数据（I/O 量、recall、延迟分布）

## 状态

开放。依赖 P2 profiling 数据。预计 P2.5 阶段启动探索。
