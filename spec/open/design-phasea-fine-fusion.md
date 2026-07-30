# DEC-045 P1: PhaseA + Fine Rerank 融合 — 实验否决

> 创建: 2026-07-30
> 状态: closed (实验否决)
> 关联: DEC-045, SLA-006

---

## 方案

在 searchLayer0 的 PQ 模式中，当 cache miss 且 PQ 距离有竞争力时，
inline pread 4KB vecblocks 页精确化距离，消除独立 Fine Rerank 阶段。

## 实验结果 (SIFT1M, 2GB cgroup, 1T)

| 模式 | REFINE_EF | Recall | QPS | 延迟 |
|------|-----------|--------|-----|------|
| Baseline (PQ + Fine Rerank) | 100 | 95.70% | **6219** | 0.16ms |
| FUSED_SEARCH | 100 | 96.20% | **311** | 3.2ms |
| FUSED_SEARCH | 50 | 91.85% | **423** | 2.4ms |

## 根因分析

**FUSED_SEARCH 慢 20 倍**，原因是 I/O 模式效率差异：

1. **Fine Rerank (批量)**: 收集 100 个候选 → ~60 unique pages → 排序合并 → 5~10 次大 pread(64KB)
2. **FUSED_SEARCH (inline)**: 图遍历中每次碰到竞争力候选 → 1 次独立 pread(4KB) → ~200 次分散 pread

inline pread 的 I/O 是**随机、分散、无法合并**的。图遍历按 best-first 顺序展开，
相邻步骤访问的节点在 vecblocks 中可能相距很远。

Fine Rerank 的批量 I/O 天然有合并优势（候选在 BFS 重排下有空间局部性）。

## 结论

**两阶段分离不是性能瓶颈，而是 I/O 效率优势。**

PQ_HYBRID 已经在 Phase A 中为 cache-hit 节点提供精确距离（~30-40% 的候选），
Phase B 只需对 cache-miss 候选（~60-70%）做批量 pread，效率远高于 inline 精确化。

**DEC-045 P1 (PhaseA+Fine 融合) 正式否决。**

## 替代方向

全量放开性能差距 (4.6x) 的解决应转向：
1. **PQ SIMD dsub=4** — 减少 Phase A 计算时间 (57%)
2. **flat_vec_cache 增大** — 提高 Phase A 精确距离覆盖率
3. **ef_search 自适应** — 根据查询难度动态调整 ef
