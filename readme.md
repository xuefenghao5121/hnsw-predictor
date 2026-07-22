# HNSW分层预取原型验证项目交接文档


## 一、项目背景

### 1.1 问题定义

HNSW（Hierarchical Navigable Small World）是当前最主流的图索引算法，在检索速度和召回率之间取得了卓越的平衡。然而，HNSW存在一个根本性约束：**所有向量数据和图索引必须完整加载至DRAM**。以128维浮点向量为例，1亿条向量需要约50GB内存，加上图索引开销（通常为数据量的2-3倍），实际内存需求超过150GB。当数据规模达到十亿级时，单机DRAM无法承载，而依赖传统DRAM扩展在技术和经济上都越来越不可持续。

业界已有两条应对路径：

1. **“为磁盘设计”的路线**：微软提出的**DiskANN**将图索引存储于SSD，内存仅保留压缩表示，在64GB内存的单机上可处理十亿级数据。DiskANN采用Vamana图算法，查询需穿透磁盘I/O。

2. **“软件定义内存”的路线**：**MEXT Predictive Memory™**通过AI持续监控DRAM中页面的活跃度，将“冷”页卸载到闪存，在应用请求前将可能变“热”的页预取回DRAM。在MoonRay渲染测试中，用**一半DRAM达到近乎相同性能**，内存相关成本降低40%，TCO降低29%。

### 1.2 项目目标

本项目将MEXT的预测性内存管理思想与HNSW检索相结合，目标是构建一个**预测驱动的HNSW冷热分层检索系统**，使得在**显著降低内存占用的前提下，仍能逼近全内存HNSW的检索延迟**，从而以SSD容量换DRAM成本，实现十亿级向量检索的经济可部署。


## 二、相关技术资料

### 2.1 核心技术文献

| 文献 | 出处 | 核心内容 | 与本项目关系 |
|------|------|----------|-------------|
| **HNSW** | Malkov & Yashunin, TPAMI 2018 | 多层可导航小世界图算法，图索引ANNS的事实标准 | 本项目的基础检索算法 |
| **DiskANN** | Subramanya et al., NeurIPS 2019 | SSD驻留图索引，Vamana图+PQ压缩，64GB内存处理十亿级数据 | 磁盘驻留图索引的参照系 |
| **MEXT Predictive Memory** | MEXT/AMD Whitepaper Q3’25 | AI预测内存页访问，冷页卸载到闪存，预取回DRAM | 预测性内存管理的核心参照 |
| **GrASP** | Zirak et al., arXiv 2025 | 基于LBA Delta的学习型预取器，用LSTM预测地址差值，从小样本泛化到大数据集 | 预取模型的架构参考 |
| **Turbocharging Vector DB** | VLDB 2025 | SSD驻留HNSW的I/O优化（io_uring、空间感知插入重排） | 磁盘I/O优化参考 |
| **Starling** | SIGMOD 2024 | I/O高效的磁盘驻留图索引框架，双层布局（内存导航图+磁盘重排序图） | 磁盘驻留图索引架构参考 |

### 2.2 代码仓库

- **hnswlib**：Header-only C++ HNSW实现，含Python绑定。项目的基础代码库，所有改造在此之上进行。
- **GrASP**：学习型预取器开源实现。预取模型的参考实现。


## 三、技术方案

### 3.1 核心设计思想

系统采用**分段级（Segment-level）的冷热分层架构**：

- 将HNSW索引切分为多个**自包含的分段（Segment）**，每个分段是一个完整的HNSW子图。
- **冷分段**驻留NVMe SSD，以磁盘块格式存储；**热分段**驻留DRAM，以标准内存HNSW格式存在。
- 通过**轻量级预测器**判断哪些冷分段即将变热，**异步预取**到DRAM。
- 查询时通过**全局路由表**定位目标分段，命中DRAM则走内存检索，未命中则触发加载。

### 3.2 数据布局：拓扑感知的磁盘块编排

**问题**：HNSW按节点插入顺序存储，拓扑相邻的节点在磁盘上可能相距甚远，导致查询时大量随机磁盘I/O。

**解决方案**：构建完成后对HNSW图执行**BFS/DFS遍历**，将遍历顺序作为磁盘写入顺序。BFS优先访问当前节点的所有邻居，因此拓扑相邻的节点在遍历序列中位置相邻，被写入同一磁盘块，将随机读转化为顺序读。

**增量场景扩展**：若数据是分批构建的（如10台机器并行各建1000万子图，再通过**Merge HNSW算法**合并为大图），Merge过程中可将跨子图新建立的边所连接的节点簇原子性地追加写入连续磁盘块，实现增量场景下的拓扑连续编排。

### 3.3 内存-磁盘两级存储

**磁盘（NVMe SSD）**：
- 存储**拓扑连续块**，每个Block包含Block Header + 连续排列的节点数据（向量 + 邻居ID列表）
- 使用紧凑格式，无内存指针，全部使用相对偏移量
- 每个Block大小固定（如256KB~1MB）

**DRAM**：
- **全局路由表**（常驻）：`NodeID → BlockID`映射，查询时O(1)定位
- **热块缓存**：固定大小缓存槽，存储已加载的热分段（展开为内存HNSW可执行格式）
- **HNSW顶层节点**（常驻）：入口点及其顶层邻居，永不允许换出

### 3.4 预测器设计

**预测目标**：下一个将被访问的**磁盘Block ID**。

**预测依据**：
- **块ID访问序列**（核心）：查询产生的Block访问序列（如`B_3 → B_7 → B_3 → B_12`），模型学习序列的转移模式
- **查询向量特征**：相似查询向量倾向于访问相似图区域
- **时间上下文**：Block在近期窗口内的访问频率变化趋势
- **块级拓扑属性**：Block内节点数量、在HNSW中的层级分布等

**模型架构**：参照**GrASP**方案——将块ID序列转化为**Delta序列**（相邻块ID的差值），用**多层LSTM**学习Delta模式。Delta建模的优势在于：
- 差值取值范围有限，将海量地址分类问题降维为有限模式识别问题
- 模型可从多样化的预训练数据集中学习通用的Delta转移规律，部署后**零样本推理**，无需针对客户数据集重新训练
- GrASP实验表明可从小样本推广到比训练数据大250倍的数据集

### 3.5 预取与缓存管理

**预取流程**：
1. 预测器输出 `Next_Block_ID`
2. 后台线程通过**io_uring**异步发起磁盘读取
3. I/O完成后将Block解析为内存格式，填入缓存槽
4. 更新路由表状态

**缓存淘汰**：采用LRU-K或LFU策略，缓存满时淘汰最冷Block（无需写回磁盘，因磁盘是原始副本）。

**降级策略**：若查询访问的Block不在缓存且未在预取队列中，主线程**同步**发起`pread()`读取（保底，保证正确性）。


## 四、原型验证方案

### 4.1 验证目标

在真实物理机上，通过修改hnswlib核心代码，验证以下核心假设：

> 通过BFS重排实现拓扑连续磁盘块 + 轻量级预测器驱动的异步预取，能否在内存减半的条件下，使缓存命中率和P99查询延迟逼近全内存HNSW。

### 4.2 实验环境

| 组件 | 规格 |
|------|------|
| CPU | 多核Xeon（如AMD Threadripper PRO或同等） |
| SSD | NVMe SSD（如三星980 Pro或同等） |
| DRAM | 限定可用容量（如16GB或32GB） |
| OS | Linux 5.x+（支持io_uring） |
| 数据集 | SIFT1M / GIST1M（百万级，便于快速迭代） |

### 4.3 分阶段任务

#### 阶段一：图重排与磁盘落盘（第1-2周）

| 任务 | 操作 | 产出 |
|------|------|------|
| 1.1 提取图结构 | 修改hnswlib的`saveIndex`，导出邻接表、层级、向量 | `graph_structure.bin` |
| 1.2 BFS全局重排 | 独立C++工具，从Entry Point开始BFS遍历 | `bfs_order`数组 |
| 1.3 切分Block并落盘 | 按`BLOCK_SIZE`切分，连续写入磁盘文件（`O_DIRECT`） | `block_0001.bin` ~ `block_N.bin` |
| 1.4 生成路由表 | 哈希表 `NodeID → BlockID` | `route_table.bin` |

#### 阶段二：搜索代码改造（第3-4周）

| 任务 | 操作 | 产出 |
|------|------|------|
| 2.1 BlockCache管理器 | 新增`BlockCache`类，固定缓存槽，每槽存`{BlockID, 节点指针, 时间戳}` | 可工作的缓存模块 |
| 2.2 修改节点访问路径 | 在`searchLayer`中，`getNodePtr(id)`改为`getNodePtrWithLoad(id)`：查路由表→若Block不在缓存→同步`pread()`加载→返回指针 | 支持按需加载的检索 |
| 2.3 LRU淘汰 | 缓存满时淘汰最久未使用Block | 内存占用可控 |

#### 阶段三：预测器与异步预取（第5-6周）

| 任务 | 操作 | 产出 |
|------|------|------|
| 3.1 访问序列采集 | 查询埋点，记录Block访问序列 | 轨迹数据集 |
| 3.2 Markov预测器 | 实现一阶Markov链：统计`P(Next\|Current)` | 轻量级预测器v1 |
| 3.3 io_uring集成 | 预测器输出候选BlockID → 提交io_uring队列 → 后台线程收割完成事件 → 插入Cache | 异步预取模块 |

#### 阶段四：端到端测试（第7-8周）

| 任务 | 操作 | 产出 |
|------|------|------|
| 4.1 基准测试 | 全内存HNSW（无磁盘I/O） | 性能上界 |
| 4.2 对照组A | 随机布局 + LRU缓存（无预取） | 基线数据 |
| 4.3 实验组B | BFS连续块 + LRU + Markov预取 | 本方案数据 |
| 4.4 数据采集 | 缓存命中率、P50/P95/P99延迟、吞吐量 | 完整评估报告 |

### 4.4 评估指标与成功标准

| 指标 | 定义 | 目标值 |
|------|------|--------|
| **缓存命中率** | `(总访问次数 - 磁盘I/O次数) / 总访问次数` | ≥ 85% |
| **P99查询延迟** | 99分位响应时间（含磁盘I/O） | 不超过全内存HNSW的2倍 |
| **内存压缩比** | 全量索引内存 / 实际DRAM占用 | ≥ 2× |
| **吞吐量** | 每秒查询数（QPS） | ≥ 全内存HNSW的60% |

### 4.5 关键技术决策点

| 决策点 | 选项 | 验证方法 |
|--------|------|----------|
| Block大小 | 64KB / 256KB / 1MB | 对比不同大小下的缓存命中率与I/O吞吐量 |
| 预测器 | Markov链 vs n-gram vs 轻量级LSTM | 对比预测准确率与推理开销 |
| 缓存策略 | LRU vs LFU vs LRU-K | 对比不同工作负载下的命中率 |
| 预取时机 | 查询间预取 vs 查询内预取 | 对比P99延迟 |


## 五、参考文献

1. Malkov, Y. A., & Yashunin, D. A. (2018). Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. *IEEE TPAMI*. 
2. Subramanya, S. J., et al. (2019). DiskANN: Fast Accurate Billion-point Nearest Neighbor Search on a Single Node. *NeurIPS*. 
3. MEXT Predictive Memory™ Technology on AMD Ryzen™ Threadripper™ PRO Workstations. MEXT/AMD Whitepaper, Q3 2025. 
4. Zirak, F., et al. (2025). GrASP: A Generalizable Address-based Semantic Prefetcher for Scalable Transactional and Analytical Workloads. *arXiv:2510.11011*. 
5. Turbocharging Vector Databases using Modern SSDs. *PVLDB*, 2025. 
6. Starling: An I/O-Efficient Disk-Resident Graph Index Framework. *SIGMOD*, 2024. 
7. hnswlib: Header-only C++/python library for fast approximate nearest neighbors. https://github.com/nmslib/hnswlib 
