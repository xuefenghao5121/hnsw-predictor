# 方案 B：多线程 + 非阻塞 search (Recall ~95-96%)

## 思路
每个查询内用非阻塞搜索（跳过 miss 候选，不等 I/O），多查询间共享 cache。

## 改动点
1. GraphPrefetcher 加锁
2. searchLayer0NonBlocking 适配 8KB block
3. batchSearchConcurrent + 非阻塞 search
4. Recall 调优: ef 参数可调

## 预期
- QPS: ~440 (N=4), ~628 (N=8)
- Recall: ~95-96%
- 改动量: 较大
