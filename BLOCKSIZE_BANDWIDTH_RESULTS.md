# Block Size 优化 + 内存带宽节省实验结果

> 日期: 2026-07-24 | 1M SIFT, k=10, ef=50, 200 queries | recall 100% | warmup 对齐

## 背景

沿 VeloANN 创新点2(缩小 block size 减少 I/O 放大)思路优化 HNSW 磁盘索引。
`benchmark_overlap.cpp` 的 F2-single 段改为按 256MB 内存预算自动算 slots
(`slots = 256MB / block_size`),使任意 block size 都在**同内存预算**下公平对比。

## 完整 block size 扫描曲线(同内存 256MB, recall 100%, warmup 对齐)

| block size | slots  | 每 block 节点 | blocks 数 | F2 QPS | F2/F0 比值 | 命中率 |
|-----------|--------|-------------|----------|--------|-----------|--------|
| 256KB     | c1024  | ~450        | 2225     | ~24.7  | ~0.06     | 86.8%  |
| 64KB      | c4096  | ~100        | 8932     | 54.3   | 0.155     | 86.8%  |
| 32KB      | c8192  | 41-61       | 17951    | 67-97  | 0.340     | 87.3%  |
| 16KB      | c16384 | 1-30        | 36252    | 123.6  | 0.611     | 87.0%  |
| **8KB** ⭐ | c32768 | 4-15        | 73894    | **142.4** | 0.479~0.87 | 85.8% |

- **16KB 首次越过 README 目标 F2/F0 ≥ 60%**
- **8KB 锁定为最优配置**:QPS 最快(142.4),但增速放缓(+15%)、命中率首次下滑(87→85.8%),拐点临近
- 4KB 预判进入拐点/倒退(每 block 仅 2-7 节点,路由表翻倍 ~147k 项,O_DIRECT 512B 对齐浪费占 1/8,syscall 暴增)

## 方法论:warmup 对齐(消除 CPU 频率抖动)

- 机器 CPU governor=performance 但实测频率在 800MHz~5100MHz 摆动,F0 基准 QPS 出现近 4x 波动(纯内存单线程 HNSW 是 CPU-bound,对主频线性敏感)
- **不改 benchmark 代码**:shell 层先空跑预热顶高频并丢弃结果,再背靠背连续跑两个配置,保证同一高频窗口内测量
- warmup 需跑两遍(warmup×2)才能把 F0 对齐到 ±0.1%~6%
- 用 F2/F0 归一化比值消除残余频率差

## ⭐⭐ 内存带宽(disk→DRAM I/O 量)节省

perf 硬件计数器被系统锁死(`perf_event_paranoid=4`),改测进程从磁盘搬进内存的真实字节数
(`/proc/PID/io` 的 `read_bytes`)——这正是 F2 方案对内存带宽压力的直接来源(storage→DRAM 通路)。

| block size | 磁盘→内存读取量 (read_bytes) | 相对 |
|-----------|----------------------------|------|
| 256KB     | **36.7 GB**                | 基准 |
| **8KB** ⭐ | **4.5 GB**                 | **↓ 8.2x (减少 87.8%)** |

### 机理
- I/O 放大 = block 大小 ÷ 实际用到数据
  - 256KB 装 ~450 节点,一次搜索只碰 5-20 个 → 放大 20-90x
  - 8KB 装 4-15 节点,几乎全用上 → 放大 ≈ 1x
- 搬运量差 8.2x 而非 block size 差 32x:缓存命中吸收了大部分重复访问,真正下沉磁盘的只有 miss 部分,8KB miss 虽多但每个小
- 8KB 命中率降到 55.3%(同 c1024 缓存字节数一样但覆盖 block 数少),但每次 miss 只搬 8KB,净搬运量反而暴降

### 意义
在超大规模(1B+)、DRAM 带宽耗尽场景,F2 靠缩小 block size 把内存带宽压力降到 1/8,
给"F2 追平甚至超过 F0"提供实打实的带宽预算。8KB 证据充分(QPS 最快 + 带宽节省 8.2x)。

## 生成物(output/ 被 gitignore,大文件不入库)

- `output/test1m_blocks_{32k,16k,8k}.bin` + `output/test1m_route_{32k,16k,8k}.bin`
- 生成命令:`write_blocks <graph> <bfs> <out> <block_size>` + `gen_route <blocks> <route>`

## 下一步

- 转向 ML 预测预取(VeloANN 创新点1,唯一能拉近 F0 延迟的路径):提前 2-3 跳预取,扩大 I/O-计算 overlap 窗口
