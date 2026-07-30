# Proposal: O_DIRECT io_uring 零 Page Cache 架构 {#PROP-DIRECT-IO}

> 关联决策: DEC-030
> 关联条款: DEC-009 (Fine Rerank), DEC-027 (用户态 I/O)
> 参考论文: HELMSMAN (OSDI 2026) — 用户态 I/O + 批量提交

## 动机

| 当前 | 目标 |
|------|------|
| FINE_BUFFERED=1, pread + page cache | O_DIRECT, io_uring 批量提交 |
| RSS 273MB (page cache 100-200MB) | RSS ≤ 180MB |
| 热态 2080 QPS | 冷态 ≥ 500 QPS |

## 技术方案

### 架构变化

```
当前 FINE_RERANK (FINE_BUFFERED=1):
  候选集 → pread 4KB pages → page cache → 零 I/O (热态)
                          ↑ 消耗 100-200MB 内存

新 FINE_DIRECT=1:
  候选集 → 收集唯一页号 → io_uring batch submit → O_DIRECT → NVMe
                                                    ↑ 绕过 page cache
                                                    ↑ RSS 降 100MB
```

### 实现改动

1. `buildFineRerank()`: `FINE_DIRECT=1` 时用 `O_DIRECT` 打开 vecblocks
2. Fine Rerank 搜索路径:
   - 收集所有候选的 `(page0, page0+1 if cross)` 页号
   - 去重 → 批量 io_uring `submitReadNF`
   - 等待完成 → 所有页数据就绪
   - 计算 L2 距离 → 维持精排逻辑不变

### 关键参数

| 参数 | 值 | 说明 |
|------|-----|------|
| io_uring queue depth | 256 | 最大 256 并发 I/O |
| 4KB 页大小 | 4096 | O_DIRECT 对齐 |
| 批量提交窗口 | 所有候选页一次提交 | 最大化并发 |

## 预期效果

| 指标 | FINE_BUFFERED | FINE_DIRECT | 变化 |
|------|-------------|-------------|------|
| SIFT1M QPS (200 queries) | 2080 | 800-1200 | -40~60% |
| SIFT1M RSS | 273MB | 150-180MB | -50-100MB |
| DEEP10M QPS | 75 | 60-70 | -5~20% (I/O 非瓶颈) |
| DEEP10M RSS | 2480MB | 2350-2400MB | -50-100MB |
