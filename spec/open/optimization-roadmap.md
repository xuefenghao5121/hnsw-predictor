# 优化路线图 v2.0 (P2 更新)

> 基准: SIFT1M 2,067 QPS (1T) / 95.70% recall / 269MB RSS
> 基准: DEEP10M 590 QPS (1T) / 2,340 QPS (12T) / 95.15% recall / 1,612MB RSS (2GB cgroup)
> 日期: 2026-07-30
> 关联: DEC-034, DEC-035, DEC-036, DEC-037, DEC-038

## P2 完成状态

| # | 优化项 | 预期 | 实际 | 状态 | 条款 |
|---|--------|------|------|------|------|
| P0 | CSR 图裁剪 (Degree Cap) | RSS -22MB | recall -0.7pp, 不值 | ❌ 负结果 | DEC-038 |
| P1 | VisitedList uint32->uint8 | - | **2x QPS** | ✅ 最大收益 | DEC-034 |
| P2 | FINE_PREAD 修复 | - | recall 70%->95% | ✅ 关键修复 | DEC-035 |
| P3 | PQ dsub=3 SIMD | +20-50% QPS | +5% QPS | ✅ 小幅 | DEC-036 |

## P2 最终成绩

```
DEEP10M (10M, 96D), 2GB cgroup, T=12:
  Recall: 95.15% ✅
  QPS:    2,340  ✅ (目标 >500)
  RSS:    1,612MB (2GB cgroup)
  vs hnswlib: 3.7x 内存节省, hnswlib OOM@2GB
```

## P3 路线图 (100M 规模)

| # | 优化项 | 目标 | 复杂度 | 优先级 | 条款 |
|---|--------|------|--------|--------|------|
| P3-1 | CSR 上磁盘 + 分页加载 | CSR 不占内存 | 高 | 🔴 | - |
| P3-2 | 1-hop CSR 预取 | 掩盖 CSR I/O | 中 | 🔴 | - |
| P3-3 | PQ codes mmap | -304MB RSS | 低 | 🟡 | - |
| P3-4 | 图裁剪 R_max=16-20 | 减 CSR 磁盘 I/O | 低 (已有工具) | 🟡 | DEC-038 保留 |
| P3-5 | SPDK 评估 | I/O 带宽 | 高 | ⚪ | DEC-027 |

## 关键教训

1. **隐藏瓶颈 > 显式瓶颈**: VisitedList 分配 (隐藏) 比 PQ 计算 (显式 80%) 影响更大
2. **规模改变瓶颈**: 1M I/O 主导 -> 10M PQ 计算主导 -> 100M CSR 内存主导
3. **cgroup RSS ≠ 真实内存**: 无 cgroup RSS 2422MB 含大量 page cache,
   cgroup 回收后仅 941MB
4. **io_uring 不总可靠**: 大候选量时丢提交, pread 更稳定
5. **图裁剪规模相关**: 1M 有效 (减 page cache 颠簸), 10M 无效 (CSR 非瓶颈)
