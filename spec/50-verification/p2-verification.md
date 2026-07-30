# P2 DEEP10M 验证用例

## VER-034: VisitedList uint32 -> uint8 {#VER-034}
<!-- ndf: kind=verif verifies=DEC-034 source=observed -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| Recall 不变 | EF=300, T=1 | 95.15% ± 0.0pp | 95.15% | ✅ |
| QPS 提升 (1T) | EF=300, T=1 | ≥ 1.5x | 2.1x (283->590) | ✅ |
| QPS 提升 (T=20) | EF=300, T=20 | ≥ 3x | 4.5x (528->2374) | ✅ |
| Recall 梯度正确 | EF=100/200/300/400 | 单调递增 | 90.85/94.20/95.15/95.55% | ✅ |
| 内存节省 (T=20) | VisitedList 总量 | ≤ 200MB | 200MB (vs 800MB) | ✅ |

## VER-035: FINE_PREAD io_uring bug 修复 {#VER-035}
<!-- ndf: kind=verif verifies=DEC-035 source=observed -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| io_uring EF=200 | FINE_PREAD=0 | 94.20% | 94.20% | ✅ (正常) |
| io_uring EF=300 | FINE_PREAD=0 | ≥94% | 88.40% | ❌ (bug) |
| io_uring EF=400 | FINE_PREAD=0 | ≥94% | 70.80% | ❌ (bug) |
| pread EF=200 | FINE_PREAD=1 | 94.20% | 94.20% | ✅ |
| pread EF=300 | FINE_PREAD=1 | ≥95% | 95.15% | ✅ |
| pread EF=400 | FINE_PREAD=1 | ≥95% | 95.55% | ✅ |
| 多线程安全 | FINE_PREAD=1, T=12 | recall 不变 | 95.15% | ✅ |

## VER-036: PQ dsub=3 SIMD {#VER-036}
<!-- ndf: kind=verif verifies=DEC-036 source=observed -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| Recall 不变 | EF=300, SIMD on | 95.15% ± 0.0pp | 95.15% | ✅ |
| QPS 提升 | EF=300, T=1 | ≥ 0% (不退化) | +1.4% (283->286) | ✅ |
| QPS 提升 (T=12) | EF=300, T=12 | ≥ 0% | +1.4% (480->487) | ✅ |

## VER-037: DEEP10M cgroup 内存 {#VER-037}
<!-- ndf: kind=verif verifies=DEC-037 source=observed -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| 2GB cgroup recall | T=4, EF=300 | ≥95% | 95.15% | ✅ |
| 2GB cgroup QPS | T=4 | ≥500 | 1484 | ✅ |
| 2GB cgroup QPS (T=12) | T=12 | ≥500 | 2340 | ✅ |
| 2GB cgroup RSS | peak | ≤2GB | 1612MB | ✅ |
| 1.8GB cgroup | T=4 | 正常运行 | 941/1395MB, 1483 QPS | ✅ |
| 1.5GB cgroup | T=4 | - | OOM (SIGKILL) | ❌ (预期) |
| 1GB cgroup | T=4 | - | OOM (SIGKILL) | ❌ (预期) |
| hnswlib 2GB ef=50 | ef=50, 50q | 存活 | RSS 2025MB, 85.50% recall | ✅ |
| hnswlib 2GB ef=200 | ef=200, 200q | OOM | SIGKILL | ✅ (DiskHNSW 优势) |

## VER-038: 图裁剪 10M 负结果 {#VER-038}
<!-- ndf: kind=verif verifies=DEC-038 source=observed -->

| 用例 | 配置 | 预期 | 实测 | 判定 |
|------|------|------|------|------|
| R_max=24 recall | EF=300 | ≥95% | 94.90% | ⚠️ -0.25pp |
| R_max=24 recall | EF=400 | ≥95% | 95.30% | ✅ (需更高 EF) |
| R_max=24 CSR | - | <591MB | 522MB (-69MB) | ✅ |
| R_max=24 QPS | EF=300 | ≥590 | 624 (+6%) | ✅ |
| R_max=20 recall | EF=300 | ≥94% | 94.45% | ⚠️ -0.7pp |
| R_max=20 CSR | - | <522MB | 481MB (-110MB) | ✅ |
| 结论 | - | 不合入主线 | recall 损失 > QPS 收益 | ✅ 决策 |
