# 优化路线图 v3.0 (P3 启动)

> 基准: SIFT1M 2,067 QPS (1T) / 95.70% recall / 269MB RSS
> 基准: DEEP10M 590 QPS (1T) / 2,340 QPS (12T) / 95.15% recall / 1,612MB RSS (2GB cgroup)
> 日期: 2026-07-30
> 关联: DEC-034~038 (P2), P3-INTENT-001 (P3 规范)

## P3: 100M 规模 - 规范设计阶段

### 规模推演

| 组件 | 10M (P2) | 100M (P3) | 存储 |
|------|----------|-----------|------|
| CSR varint | 591MB | 4.9GB | 磁盘分页 + 256MB 缓存 |
| PQ codes | 304MB | 3.2GB | mmap (OS 分页) |
| Upper vecs | 228MB | 2.4GB | PQ 编码 -> 200MB |
| BFS 映射 | 76MB | 800MB | 常驻 |
| Route+slot | 116MB | 1.2GB | 常驻 (Q-005 待优化) |
| VisitedList | 120MB | 120MB | 常驻 (uint8, T=12) |
| CSR offset 表 | - | 400MB | 常驻 (100M×4B) |
| **常驻合计** | ~1.4GB | **~2.9GB** | 4GB cgroup 可行 |

### P3 工作分解

| # | 工作项 | 类型 | 优先级 | 条款 | 状态 |
|---|--------|------|--------|------|------|
| P3-1 | CSR 分页文件格式设计 + 实现 | L2 机制 | 🔴 | P3-ARCH-001 | draft |
| P3-2 | CSRCache 实现 (LRU + pread) | L2 机制 | 🔴 | P3-ARCH-002 | draft |
| P3-3 | searchLayer0 适配 CSR 分页 | L2 机制 | 🔴 | P3-ARCH-003 | draft |
| P3-4 | 1-hop CSR 预取 | L1 行为 | 🟡 | P3-BEH-002 | draft |
| P3-5 | PQ codes mmap | L1 行为 | 🟡 | P3-BEH-003 | draft |
| P3-6 | 上层向量 PQ 编码 | L1 行为 | 🟡 | P3-BEH-004 | draft |
| P3-7 | 100M 数据集准备 | 基础设施 | 🔴 | Q-003 | open |
| P3-8 | 路由表内存优化 | L2 机制 | 🟢 | Q-005 | open |

### P3 执行顺序

```
P3-7 (数据准备) ──────────────────────────────────> 并行
    │
P3-1 (CSR 文件格式) -> P3-2 (CSRCache) -> P3-3 (搜索适配)
    │                                          │
P3-5 (PQ mmap) ─────────────────────────────────> 可并行
P3-6 (上层 PQ) ──────────────────────────────────> 可并行
    │
P3-4 (1-hop 预取) ──> P3-8 (路由表优化) ──> 验证
```

### P3 设计选项 (DSE)

| 选项 | default | explore | 条款 |
|------|---------|---------|------|
| CSR 页缓存大小 | 256MB | {128,256,512,1024} | P3-OPT-001 |
| CSR 页大小 | 4KB | {4K,16K,64K} | P3-OPT-002 |
| 上层 PQ M 值 | 32 | {16,24,32} | P3-OPT-003 |

### P3 SLA

| 指标 | 目标 | 说明 |
|------|------|------|
| Recall@10 | ≥95% | 与 P2 一致 |
| QPS (1T) | >100 | 100M 规模, CSR I/O 主导 |
| QPS (12T) | >500 | 多线程并行 |
| RSS | ≤4GB | 4GB cgroup |
| vs hnswlib | ≥3x 内存节省 | hnswlib 100M 需 ~50GB |

## P2 完成状态 (参考)

| # | 优化项 | 状态 | 条款 |
|---|--------|------|------|
| P2-1 | VisitedList uint32->uint8 | ✅ 2x QPS | DEC-034 |
| P2-2 | FINE_PREAD 修复 | ✅ recall 70%->95% | DEC-035 |
| P2-3 | PQ dsub=3 SIMD | ✅ +5% QPS | DEC-036 |
| P2-4 | cgroup 目标校准 | ✅ 1GB->2GB | DEC-037 |
| P2-5 | 图裁剪 10M | ❌ 负结果 | DEC-038 |
