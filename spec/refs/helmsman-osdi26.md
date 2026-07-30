# HELMSMAN: Clustering-Based ANNS at Scale {#REF-HELMSMAN}
<!-- ndf: kind=ref origin="OSDI 2026 — Huang et al." origin-status=paraphrase since=2026-07-29 -->

## 源信息

- **标题**: The Clustering Strikes Back: Building Cost-Effective and High-Performance ANNS at Scale with Helmsman
- **会议**: OSDI 2026 (CCF-A)
- **作者**: Yuchen Huang, Baiteng Ma, Yiping Sun, Yang Shi, Xiao Chen, Xiaocheng Zhong, Zhiyong Wang, Yao Hu, Erci Xu, Chuliang Weng
- **机构**: 小红书 + 华东师范大学 + 上海交通大学
- **arXiv**: <https://arxiv.org/abs/2606.13145>
- **GitHub**: <https://github.com/Red-EAD/helmsman> (MiniHyperVec PoC)
- **现状**: 生产部署中，40 台机器替代 35,000 核心 + 0.35PB DRAM

## 核心主张

1. **聚类 ANNS 比图 ANNS 更适合 SSD**: 图遍历产生串行 I/O 依赖链，无法利用 SSD 带宽；聚类允许批量无依赖 I/O
2. **用户态 I/O 是必需的**: 内核 I/O 栈（含 io_uring）仅利用 26-59% SSD 带宽，SPDK 可达 85%
3. **学习式剪枝优于固定剪枝**: GBDT 预测最优 nprobe 层级，自适应 top-k 和 query 分布
4. **GPU 加速构建**: 10B 索引 4-7 小时（vs CPU 16+ 小时）

## 与 DiskHNSW 的关系

| 维度 | DiskHNSW | HELMSMAN |
|------|----------|----------|
| 方法 | 图（HNSW）+ SSD 向量 | 聚类（SPANN）+ SSD cluster |
| I/O 模式 | 图遍历 → 部分串行 I/O | 批量 cluster 读 |
| 目标规模 | 1M → 10M → 100M | 0.1B → 10B → 100B |
| 延迟目标 | <1ms（热态） | <10ms |
| recall 目标 | ≥95% | 90% |
| I/O 栈 | pread / io_uring | SPDK 用户态 |
| 构建 | 单机 CPU | GPU 弹性集群 |
| 成本效率 | — | 250 QPS/$（vs 内存 HNSW 51 QPS/$）|

## 本文档投影的关键结论

1. **图遍历 I/O 瓶颈在 100M+ 规模显现**（[[DEC-026]]）：DiskANN/Starling/PipeANN 均无法满足在线 SLA
2. **当前 1M → 10M 路径仍然有效**（[[DEC-026]]）：10M 规模 vecblocks < 5GB，图遍历 I/O 串行化尚未成为瓶颈
3. **io_uring 的带宽上限需计入 P3 设计**（[[DEC-027]]）：内核 I/O 最多利用 60% SSD 带宽
4. **学习式剪枝可提高 Fine Rerank 效率**（[[DEC-028]]）：用模型预测最佳候选数/页数，替代固定 EF+refine

## 引用这个参考的条款

- [[DEC-026]] — 图 vs 聚类范式分歧
- [[DEC-027]] — 用户态 I/O 评估
- [[DEC-028]] — 学习式剪枝探索
- [[Q-001]] — 图遍历的规模上限
- [[Q-002]] — 学习式剪枝可行性
