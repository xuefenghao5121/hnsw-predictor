# 方案 A：多线程 + 阻塞 search (Recall 100%)

## 思路
N 个线程跑 N 个查询，共享 BlockCache（已有 mutex）。查询 A 阻塞等 I/O 时，OS 自动调度查询 B。

## 改动点
1. GraphPrefetcher 加锁（mutex 保护 pending_requests_, completed_blocks_, ring_）
2. DiskHNSW 新增 batchSearchConcurrent() - N 线程并行
3. benchmark 新增 F2-concurrent-4/8 配置

## 预期
- QPS: ~360 (N=4), ~528 (N=8)
- Recall: 100%
- 改动量: 中等
