# 方案 C：事件驱动单线程 (最高效但最复杂)

## 思路
单线程 + io_uring + 协程/state machine。查询 A 阻塞 I/O 时切换到查询 B。无锁竞争。

## 改动点
1. 查询状态机 (QueryState): 保存每个查询的 candidate_set, top_candidates, visited
2. 调度器: I/O 完成回调 -> 恢复对应查询
3. searchLayer0 改为可中断的 state machine
4. benchmark 集成

## 预期
- QPS: ~440-628 (与方案 B 相当，无锁开销)
- Recall: 可调 (100% 如果阻塞等待, ~96% 如果非阻塞)
- 改动量: 很大
