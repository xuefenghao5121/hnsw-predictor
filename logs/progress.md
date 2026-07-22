# HNSW Predictor 项目进度

> 最后更新：2026-07-22

## 阶段一：图重排与磁盘落盘 ✅ 完成

| Task | 状态 | 产出 |
|------|------|------|
| 1.1 提取图结构 | ✅ | `extract_graph.cpp` -> `graph_structure.bin` |
| 1.2 BFS 全局重排 | ✅ | `bfs_reorder.cpp` -> `bfs_order.bin` |
| 1.3 切分 Block 并落盘 | ✅ | `write_blocks.cpp` -> `blocks.bin` |
| 1.4 生成路由表 | ✅ | `gen_route.cpp` -> `route_table.bin` |
| 1.5 验证 | ✅ | `verify.cpp` -> 所有数据验证通过 |

数据集：
- test (1K 节点): ✅ 所有产出文件已生成
- test100k (100K 节点): ✅ 所有产出文件已生成
- test1m (1M 节点): ✅ 所有产出文件已生成

## 阶段二：搜索代码改造 ✅ 完成（含重构）

| Task | 状态 | 产出 |
|------|------|------|
| 2.1 BlockCache 管理器 | ✅ 完成 | `block_cache.h` + `block_cache.cpp` + `test_block_cache.cpp` |
| 2.2 修改节点访问路径 | ✅ 完成 | `disk_hnsw.h` + `disk_hnsw.cpp` + `test_disk_hnsw.cpp` |
| 2.3 LRU 淘汰 | ✅ 已在 2.1 中实现 | LRU list + unordered_map |
| 2.4 重构：可插拔接口 | ✅ 完成 | `layout_provider.h` + `replacement_policy.h` + IOConfig |

### Task 2.1 完成详情 (2026-07-22)

产出文件：
- 设计文档：`hnsw-research/phase2-design.md`
- 头文件：`src/block_cache.h`
- 实现：`src/block_cache.cpp`
- 单元测试：`src/test_block_cache.cpp` (6/6 测试通过 ✅)
- 执行日志：`logs/task2.1_block_cache.log`

关键设计决策：
- I/O：普通 pread（后续阶段优化为 O_DIRECT）
- 线程安全：std::mutex 粗粒度锁
- LRU：list + unordered_map，O(1) 操作
- 内存展开：指针指向 raw_data，避免二次拷贝

1M 数据集 Smoke Test 通过：初始化 6ms，数据正确性验证通过。

### Task 2.2 完成详情 (2026-07-22)

产出文件：
- 头文件：`src/disk_hnsw.h`
- 实现：`src/disk_hnsw.cpp`
- 测试程序：`src/test_disk_hnsw.cpp`
- 执行日志：`logs/task2.2_disk_hnsw.log`

关键设计决策：
- 不继承 hnswlib，独立实现 DiskHNSW 类
- 顶层（Layer 1+）：从 graph_structure.bin 加载常驻内存，使用 old_id
- Layer 0：通过 BlockCache 按需加载，使用 BFS 重排后的 new_id
- ID映射：old_to_new/new_to_old 双向映射，在两层之间转换
- 距离计算：纯L2（与 hnswlib L2Sqr 一致）
- 悬空指针修复：邻居ID列表在使用前复制到本地缓冲区，防止 BlockCache LRU淘汰导致指针失效

测试结果：
- 1K 数据集 (3 blocks, 64 cache slots): recall@10 vs HNSW = **100%**, vs GT = 99.27% ✅
- 1K 数据集 (3 blocks, 2 cache slots, 重压测试): recall@10 vs HNSW = **100%**, 无 crash ✅
- 100K 数据集 (227 blocks, 64 cache slots): recall@10 vs HNSW = **100%** ✅
- BlockCache 命中率：1K=99.99%, 100K=58.8%（BFS重排+小缓存）

### Task 2.4 重构详情 (2026-07-22)

产出文件：
- `src/layout_provider.h` - LayoutProvider 接口 + BfsLayoutProvider + RandomLayoutProvider
- `src/replacement_policy.h` - ReplacementPolicy 接口 + LRU/LFU/LRU-K 实现
- 修改 `src/block_cache.h` 和 `src/block_cache.cpp` - 可插拔接口 + IOConfig
- 修改 `src/test_disk_hnsw.cpp` - 支持 --io-mode / --latency / --layout / --policy
- 修改 `src/disk_hnsw.h` 和 `src/disk_hnsw.cpp` - 新增可插拔构造函数
- 更新 `Makefile` - 编译新文件
- 更新 `hnsw-research/phase2-design.md` - 完整重构设计文档
- 执行日志：`logs/task2_refactor.log`

重构内容：
1. **LayoutProvider 接口**：NodeID -> BlockID 映射解耦，支持 BFS（从文件加载）和 Random（随机分配）
2. **ReplacementPolicy 接口**：缓存淘汰逻辑解耦，支持 LRU / LFU / LRU-K(K=2)
3. **IOConfig**：支持 O_DIRECT / drop page cache / 模拟延迟三种 I/O 模式
4. **向后兼容**：旧构造函数保留，内部自动创建 BfsLayoutProvider + LRUPolicy

测试结果：
- 单元测试：6/6 通过 ✅
- BFS + LRU + cached: recall 100% ✅
- BFS + LFU + cached: recall 100% ✅
- BFS + LRU-K + cached: recall 100% ✅
- BFS + LRU + simulated(100us): recall 100% ✅
- Random + LRU + cached: recall 34.2%（预期行为，对照组验证布局重要性）

## 阶段三：预测器与异步预取 ⏳ 待做

| Task | 状态 |
|------|------|
| 3.1 访问序列采集 | ⏳ |
| 3.2 Markov 预测器 | ⏳ |
| 3.3 io_uring 集成 | ⏳ |

## 阶段四：端到端测试 ⏳ 待做

| Task | 状态 |
|------|------|
| 4.1 基准测试 | ⏳ |
| 4.2 对照组A | ⏳ |
| 4.3 实验组B | ⏳ |
| 4.4 数据采集 | ⏳ |
