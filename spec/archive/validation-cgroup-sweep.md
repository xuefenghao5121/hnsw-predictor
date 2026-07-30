# 性能验证报告 2026-07-30 (Cgroup 内存限制扫描)

> 验证日期: 2026-07-30 (凌晨)
> 关联决策: DEC-030, DEC-031
> 关联条款: CON-007 (资源约束)

## 测试环境

- 数据集: SIFT1M (128D, 1M 向量)
- Cgroup: v2, memory.max = 140-256MB
- 参数: K=10, EF=50, NQ=200, TWO_STAGE=1, FINE_RERANK=1, FINE_BUFFERED=1
- 缓存最小化: CACHE_MB=8, FLAT_VEC_MB=8 (让更多内存留给 page cache)
- PQ: M=32, REFINE_EF=100

## 实测结果

| Cgroup | RSS 运行时 | 可用 page cache | QPS | Mean | 状态 |
|--------|----------|----------------|-----|------|------|
| 256MB | 208MB | ~48MB | **1,973** | 0.51ms | 🟢 热 |
| 200MB | 195MB | ~5MB | **1,372** | 0.72ms | 🟡 部分颠簸 |
| 180MB | 178MB | ~2MB | **196** | 5.09ms | 🔴 悬崖 |
| 160MB | 156MB | ~4MB | 177 | 5.64ms | 🔴 纯磁盘 |
| 150MB | 148MB | ~2MB | 319 | 3.14ms | 🟡 波动 |
| 140MB | 141MB | ~0MB | 190 | 5.25ms | 🔴 极限 |

## 关键发现

### 1. 10× QPS 悬崖在 180MB 处

```
256MB → 200MB:  1,973 → 1,372  (-30%, 线性退化)
200MB → 180MB:  1,372 →   196  (-86%, 断崖!)
```

根因: page cache 工作集 ~24MB（400 query × 15 页 × 4KB），
180MB cgroup 只剩 2MB 可用 page cache → 页面刚读入就被驱逐 → 颠簸。

### 2. Cgroup 颠簸比 FINE_DIRECT 更差

| 场景 | 配置 | QPS | 原因 |
|------|------|-----|------|
| 无 cgroup | FINE_DIRECT=1 | 787 | 干净 O_DIRECT 读 |
| 180MB cgroup | FINE_BUFFERED | 196 | page cache 颠簸 |

page cache 颠簸时 OS 花大量 CPU 在 LRU 维护和页面驱逐上，
实际性能比直接 O_DIRECT 还差。

### 3. 工作集大小决定性能拐点

- 400 query × ~15 唯一页 = 6,000 页 = 24MB 工作集
- 256MB cgroup: 48MB 可用 → 2× 工作集 → 热
- 200MB cgroup: 5MB 可用 → 0.2× 工作集 → 温
- 180MB cgroup: 2MB 可用 → 0.08× 工作集 → 颠簸

## 启示

**需要页面级驱逐策略**：在 Fine Rerank 完成后，用 `posix_fadvise(DONTNEED)` 
仅驱逐刚读过的页，让 page cache 只保留跨 query 复用的热页。
预期效果：消除颠簸，180MB cgroup QPS 从 196 → 500+。
