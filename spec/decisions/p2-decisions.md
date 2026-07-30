# P2 决策记录: DEEP10M 10M 规模验证

> 日期: 2026-07-30
> 关联: DEC-029 (瓶颈转移), DEC-026 (图方法 P2 确认)
> 取代: validation-deep10m-p2.md 中的初步结论

---

## D-034: VisitedList uint32 -> uint8 内存优化 {#DEC-034}
<!-- ndf: kind=decision date=2026-07-30 affects=BEH-002,CON-002 source=observed -->

**Context.** DEEP10M 10M 节点的 VisitedList 使用 `std::vector<uint32_t>` (40MB/实例)。
多线程搜索中, 每个线程每查询创建/销毁一个 VisitedList, 大块 malloc/free 成为隐藏瓶颈。

P2 初步测试 (ndf spec, 2026-07-29): QPS 仅 74.9, 远低于 500 目标。
性能分析显示 PQ 计算占 80%, 但未识别 VisitedList 分配开销。

**Decision.** 将 VisitedList 从 `uint32_t` 改为 `uint8_t`:

```cpp
// Before: 40MB per instance (10M nodes)
struct VisitedList {
    std::vector<uint32_t> mass;
    uint32_t curV;
};

// After: 10MB per instance (10M nodes)
struct VisitedList {
    std::vector<uint8_t> mass;
    uint8_t curV;  // 1-255, overflow -> memset
};
```

版本计数器用 uint8_t (max 255 次搜索后 memset 一次)。每次 `searchKnn` 创建新 VisitedList,
curV 从 1 开始, 单次搜索内不变, 无溢出风险。

**实测效果:**

| 指标 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| QPS (1T, EF=300) | 283 | 590 | **2.1x** |
| QPS (T=20, EF=300) | 528 | 2374 | **4.5x** |
| VisitedList 内存 (T=20) | 800MB | 200MB | -600MB |

**根因分析:** malloc/free 40MB 块的开销在单线程下与 PQ 计算时间可比 (~0.5ms),
多线程下因分配器锁竞争而放大。uint8 将块大小降 4x, 分配开销线性下降。

> rationale: 这是"隐藏瓶颈"的典型案例--profiling 显示 PQ 占 80%,
> 但 PQ 的时间包含等待 VisitedList 分配的延迟。缩小分配量后, PQ 占比自然下降。
> 启示: 大规模场景下的内存分配模式比算法复杂度更重要。

## D-035: FINE_PREAD 作为多线程默认路径 {#DEC-035}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-003,DEC-009,BEH-007 source=observed -->

**Context.** P2 测试发现 io_uring fine rerank 路径在候选数 >200 时丢失 I/O 提交,
导致 recall 反降 (REFINE_EF=200 -> 94.20%, 300 -> 88.40%, 400 -> 70.80%)。

单线程下 `FINE_PREAD=1` (pread 替代 io_uring) 修复此问题:
REFINE_EF=300 -> 95.15%, REFINE_EF=400 -> 95.55%, 行为正确。

**Decision.**

1. **FINE_PREAD=1 作为多线程和大规模默认推荐**
2. **io_uring fine rerank 路径降级为实验性**, 在候选数 ≤200 时仍可用
3. **REFINE_EF=300 作为 10M 规模推荐值** (recall 95.15%, QPS 590 @1T)

**根因推测:** io_uring SQ ring 在高频提交时有竞争窗口, 部分提交被覆盖。
pread 是系统调用, 无异步状态机, 行为可预测。多线程下 pread 天然线程安全。

**影响:**
- 取代 [[DEC-003]] 中 "io_uring 作为首选 I/O 路径" 的默认推荐
- io_uring 仍用于 BlockCache 预取路径 (非 fine rerank)
- 新增约束: 多线程 MUST 设置 `FINE_PREAD=1`

> rationale: io_uring 的异步优势在 fine rerank 场景不显著
> (每查询仅 100-400 次 4KB 读, pread 批量执行也很快),
> 但其正确性风险高于 pread。稳定性 > 异步性。

## D-036: PQ dsub=3 SIMD 优化 {#DEC-036}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-014,BEH-005 source=observed -->

**Context.** DEC-014 记录了 SIFT (128D, M=32, dsub=4) 的 AVX2 LUT 构建优化。
DEEP10M (96D, M=32, dsub=3) 不匹配 dsub=4 路径, 走标量代码, PQ LUT 构建慢。

**Decision.** 添加 dsub=3 专用 SSE 路径:

1. **LUT 构建**: SSE 4-centroid 并行 (pad 3 floats -> 4, reuse sub/mul/hadd)
2. **PQ distance lookup**: AVX2 gather (`_mm256_i32gather_ps`), 8 lookups/iteration

**实测效果:** ~5% QPS 提升 (283 -> 286 @1T, 噪声级别)。

**分析:** LUT 构建是每查询一次 (O(M×ksub×dsub) = 24576 ops),
PQ distance lookup 是每邻居一次 (O(M) = 32 lookups)。
两者都在 L1 cache 内, SIMD 优化收益有限。
真正的瓶颈是 VisitedList 分配 (见 [[DEC-034]]), 不是 PQ 计算。

> rationale: 优化了"看似 80% 的瓶颈", 实际收益 <5%。
> 这是 profiling 误导的典型案例--PQ 80% 包含了等待内存分配的 idle 时间。

## D-037: DEEP10M cgroup 内存目标重新校准 {#DEC-037}
<!-- ndf: kind=decision date=2026-07-30 affects=CHR-003,CHR-004,CON-007 source=observed -->

**Context.** CHR-004 设想 P2 在 1GB cgroup 下运行。实测发现 1GB 物理不可行:

| 组件 | 大小 | 说明 |
|------|------|------|
| CSR 邻接表 | 591MB | 219M edges, delta+varint 压缩后 |
| PQ codes (M=32) | 304MB | 10M × 32 bytes |
| 上层向量 | 228MB | 624K nodes × 96D × 4B |
| BFS 映射 | 76MB | 2 × 10M × 4B |
| 路由表+槽位表 | ~116MB | route + vec_route + node_slot |
| **核心数据合计** | **~1315MB** | **> 1GB** |

**Decision.**

1. **P2 cgroup 目标从 1GB 放宽到 2GB**
2. **1GB cgroup 在 10M 规模物理不可行** (核心数据 1.3GB)
3. **2GB cgroup 下 hnswlib OOM (ef≥200), DiskHNSW 正常运行** -- 这是核心叙事
4. **最小可行 cgroup: 1.8GB** (RSS init 941MB, peak 1395MB)

**内存节省 vs hnswlib:**
- hnswlib DEEP10M: ~5960 MB (10M × 96D × 4B vectors + graph)
- DiskHNSW DEEP10M: 1612 MB (2GB cgroup peak)
- **节省: 3.7x**

> rationale: 1GB 目标是 SIFT1M (512MB cgroup) 的线性外推,
> 忽略了 CSR 和 PQ codes 随节点数线性增长。
> 2GB vs 6GB 仍是 3x 内存节省, 核心价值主张不变。

## D-038: 图裁剪在 10M 规模的负结果 {#DEC-038}
<!-- ndf: kind=decision date=2026-07-30 affects=DEC-033,VER-033 source=observed -->

**Context.** DEC-033 在 SIFT1M 180MB cgroup 下验证了 CSR 图裁剪的有效性
(CSR 47->44MB, QPS 4.7x 提升)。P2 在 DEEP10M 上测试 MRNG R_max=20 和 R_max=24。

**实测结果:**

| 配置 | CSR | Recall (EF=300) | QPS (1T) | Recall (EF=400) |
|------|-----|----------------|----------|----------------|
| 原始 | 591MB | 95.15% | 590 | 95.55% |
| R_max=24 | 522MB | 94.90% | 624 | 95.30% |
| R_max=20 | 481MB | 94.45% | 648 | - |

**Decision.** 图裁剪在 10M 规模**不合入主线**:

1. **CSR 已非 cgroup 瓶颈**: 无 cgroup RSS 2422MB 大部分是 page cache;
   cgroup 回收后匿名内存仅 941MB, CSR 591MB 在其中, 但 2GB cgroup 下不构成瓶颈
2. **Recall 损失不值**: R_max=24 在 EF=300 下 recall -0.25pp, 需 EF=400 补偿,
   QPS 反降 (624 -> 496)
3. **SIFT1M 的成功不可外推**: 1M 规模 page cache 颠簸是主要矛盾 (CSR 占 RSS 比例高),
   10M 规模 cgroup 限制才是矛盾 (CSR 在匿名内存中占比 < 50%)

**保留:** 裁剪工具 (`prune_graph.cpp`) 保留, 供 P3 (100M) 规模使用,
届时 CSR varint 4.7GB 需上磁盘, 裁剪可减少磁盘 I/O。

> rationale: 优化目标的优先级随规模变化。
> 1M: 减少 RSS (page cache 颠簸) > 减少 QPS 损失
> 10M: 维持 recall > 减少 RSS (cgroup 足够大)
> 100M: 减少 CSR 磁盘 I/O > 维持 recall (CSR 不再常驻内存)
