# Comparison: OBS- vs INTENT- 分歧分析 (Status: Resolved on 2026-07-29)

> 本文件不包含规范性条款，仅记录观测事实与设计意图之间的差距，供审查参考。
> 所有 14 条冲突已于 2026-07-29 裁决完毕，条款 ID 已统一去前缀（OBS-/INTENT- 移除）。
> 本文件保留在 `open/` 下作为裁决归档。
>
> 差距类型：`事实偏差`（INTENT 与 OBS 记录的客观事实不符）/ `文档意淫`（INTENT 的主张无代码或数据支撑）/ `技术债务`（OBS 记录的现状被 INTENT 标记为需修复）/ `未来目标`（INTENT 描述了 OBS 尚未实现的目标状态）

---

## 1. flat_vec_cache 默认值

> **裁决**: ✅ 已修正（选项 B）— 保留 INTENT-DEC-015，明确区分代码默认值（4MB）和推荐配置（64MB），标注为 benchmark 观测经验

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-CON-002`: flat_vec_cache 默认 = **4MB**（`block_cache.cpp:244`），由 `FLAT_VEC_MB` 环境变量覆盖 | `INTENT-DEC-015`: 声称 flat_vec_cache = **64MB** 是"第一级热区"设计意图，覆盖 63K 上层节点 | `事实偏差` |

**说明**：INTENT 将 benchmark 配置参数（`FLAT_VEC_MB=64`）误述为代码默认值。代码默认 4MB 只能装 ~8K 向量，远不够覆盖上层节点。64MB 是用户手动配置的结果，不是代码内建的"设计意图"。

---

## 2. 业务价值与用户画像

> **裁决**: ✅ 维持现状（选项 A）- INTENT-CHR-001/002 已删除，不再处理

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-CHR-001`: 系统目标从代码归纳--"在 cgroup 限额下用磁盘向量实现 ≥95% recall"，列出 6 个核心实体，无业务价值描述 | `INTENT-CHR-001` + `INTENT-CHR-002`: 声称解决"容器化部署/边缘计算/成本降低 50%"问题，定义 4 类目标用户 | `文档意淫` |

**说明**：代码中无任何部署配置（无 Dockerfile、无 K8s manifest、无 cgroup 除 benchmark 外的使用）、无用户研究数据。"50% 成本降低"和"K8s sidecar"是未经验证的市场假设。

---

## 3. Scope OUT 项变为未来目标

> **裁决**: ✅ 已修正（选项 B）- 保留 INTENT-CHR-004，加"探索性设想"标签，metadata 改为 level=may / status=exploratory，明确标注 P2-P5 无设计文档支撑

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-CHR-002`: 明确 OUT -- 增量插入/删除、多租户 QoS、分布式部署、持久内存、GPU 加速 | `INTENT-CHR-004`: P4 规划增量索引 + 多租户 QoS；P5 规划 GPU + PMEM | `未来目标` |

**说明**：OBS 记录当前不做，INTENT 将其纳入演进路线。两者不矛盾（OUT 不等于永远不做），但 INTENT 的 P4-P5 规划无对应代码或设计支撑。

---

## 4. God Class DiskHNSW

> **裁决**: ✅ 已降级（选项 B）- 删除 INTENT-ARCH-002（DDD 聚合根拆分方案），简化 INTENT-ARCH-004 为技术债务事实观测，保留“DiskHNSW 4874 行过大”的记录

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-ARCH-003`: 模块分层 L0-L5，`disk_hnsw.h/.cpp` 在 L4（搜索引擎），4874 行 | `INTENT-ARCH-002` + `INTENT-ARCH-004`: 划分 4 个 Bounded Context（15+ 聚合根），标注 God Class 为"高严重度技术债务" | `技术债务` |

**说明**：OBS 中性记录了 DiskHNSW 的大小和职责。INTENT 将其标记为债务并提议 DDD 拆分。当前代码无任何聚合根抽象。

---

## 5. friend class 紧耦合

> **裁决**: ✅ 已修正（选项 B）- INTENT-ARCH-005 保留依赖关系现状表（有价值的事实），删除 Port 接口目标列和六边形架构引用，INTENT-ARCH-004 已在上条简化为事实观测

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-ARCH-002` + `OBS-ARCH-004`: `friend class DiskHNSW` 在 `block_cache.h:293,389`，OBS 标注为"警告"但承认无 #include 循环 | `INTENT-ARCH-004` + `INTENT-ARCH-005`: 标注为"高严重度"债务，提议用 `IVectorStore` Port 接口替代，消除反向耦合 | `技术债务` |

**说明**：OBS 认为 friend 是"紧耦合但无循环"。INTENT 认为 friend 是"语义上的循环依赖"需消除。Port 接口当前不存在。

---

## 6. 六边形架构 vs 扁平分层

> **裁决**: ✅ 已精简（选项 B）- 删除 ASCII 架构图和 Port/Adapter 设计细节，只保留一句远期方向说明

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-ARCH-003`: 6 层扁平结构（L0 数据格式 -> L5 应用），依赖方向自上而下，无 Port/Adapter 概念 | `INTENT-ARCH-001`: 目标为六边形架构（Application/Domain/Port/Infrastructure），依赖指向内部 | `未来目标` |

**说明**：当前代码是传统的分层架构（header -> core -> pipeline -> benchmark）。六边形架构是 INTENT 提出的重构目标，无任何代码实现。

---

## 7. Port 接口存在性

> **裁决**: ✅ 维持现状（选项 A）- INTENT-ARCH-003 已删除，不再处理

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-ARCH-004`: 6 个耦合点全部通过 `unique_ptr` 直接持有，`LayoutProvider` 和 `ReplacementPolicy` 是仅有的接口抽象 | `INTENT-ARCH-003`: 定义 4 个 Port 接口（`IVectorStore`/`IAsyncIO`/`IRouteTable`/`IReplacementPolicy`），声称"逻辑职责已存在" | `未来目标` |

**说明**：`IReplacementPolicy` 已存在（`replacement_policy.h`）。其余 3 个 Port 接口在代码中完全不存在。INTENT 的"逻辑职责已存在"是对现有代码的过度解读。

---

## 8. 双路由表的性质

> **裁决**: ✅ 已修正（选项 B）- 保留“不依赖隐式对齐”原则，删除第 2 条 IRouteTable Port 引用（接口不存在）

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-DEC-004` + `OBS-ARCH-004 #6`: `vec_route_table_` 是修复 FINE_RERANK bug 的补丁，4MB 额外内存 | `INTENT-DEC-012`: 将双路由表提升为架构原则"不依赖隐式对齐"，提议 `IRouteTable` Port 统一 | `未来目标` |

**说明**：OBS 记录的是一个 bug fix。INTENT 将其上升为设计哲学。两者不矛盾，但 INTENT 的架构原则尚未在代码中体现（无 IRouteTable 接口，路由表仍分散在 BlockCache 和 DiskHNSW 中）。

---

## 9. delta+varint 压缩的选型理由

> **裁决**: ✅ 已修正（选项 B）- 保留 4 条事后技术观测，标题从“事后架构分析”改为“事后技术观测”，删除六边形架构兼容性声明

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-DEC-005`: 选择 delta+varint 是因为 BFS 相邻节点 Jaccard = 0.023 太低，BVGraph 不适用；纯数据驱动 | `INTENT-DEC-010`: 补充"解码独立性/随机访问/线程安全/架构兼容"4 条理由，声称与六边形架构兼容 | `文档意淫` |

**说明**：原始决策（OBS）完全基于数据特征分析（Jaccard 值）。INTENT 的 4 条架构理由是事后合理化--决策时六边形架构概念不存在，`CSRAdjacency` 聚合根也未实现。

---

## 10. "零外部依赖"约束

> **裁决**: ✅ 维持现状（选项 A）- INTENT-CHR-005 #1 已删除，不再处理

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-ARCH-001`: 代码依赖 hnswlib（header-only，编译时引入）和 faiss（Python 脚本，运行时不依赖） | `INTENT-CHR-005` + `INTENT-DEC-013`: 声称"零外部运行时依赖"和"单二进制部署"为设计约束 | `文档意淫` |

**说明**：代码确实没有运行时 .so 依赖，但这是 hnswlib header-only 特性的副产物，而非主动设计的约束。`Makefile` 中 `-I./hnswlib` 说明 hnswlib 是编译时依赖。INTENT 将事实上升为意图。

---

## 11. "每个优化有 benchmark 数据支撑"

> **裁决**: ✅ 已修正（选项 B）- 从 MUST 降级为 SHOULD，承认是理想目标而非强制纪律，补充 FINE_MERGE/SPEC_PREFETCH 反例

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-CON-003`: 15+ 个环境变量散落在 `disk_hnsw.cpp` 各处，通过 `std::getenv` 直接读取，无集中配置 | `INTENT-CHR-005 #5`: 声称"每个优化 MUST 有 benchmark 数据支撑，不做无数据的感觉更快" | `文档意淫` |

**说明**：README 中确实有 benchmark 数据，但代码中无任何机制强制"无 benchmark 不合入"。部分优化（如 `FINE_MERGE`、`SPEC_PREFETCH`）在 README 中标注"收益不大"或"默认关闭"，说明并非所有优化都通过数据验证才保留。

---

## 12. 硬编码 4096 vs 设计纪律

> **裁决**: ✅ 已补充（选项 B）- 在 INTENT-ARCH-004 技术债务表中新增“硬编码 4096ull”行，引用 OBS-CONFLICT-002

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-CONFLICT-002`: Fine Rerank 路径硬编码 `4096ull`（`disk_hnsw.cpp:1725`），未引用 `BLOCKS_FILE_HEADER_SIZE` 常量 | `INTENT-CHR-005 #5`: 声称设计约束包含"可测量性"和设计纪律 | `技术债务` |

**说明**：硬编码 magic number 违反了 INTENT 声称的设计纪律。OBS 已标记为"低风险但代码脆弱"。INTENT 的纪律主张与实际代码行为不一致。

---

## 13. 两阶段搜索的设计意图

> **裁决**: ✅ 已修正（选项 A）- 强调“5 种模式之一，非唯一架构路径”，修正 metadata affects 引用，明确不是“核心机制”

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-BEH-002`: `searchKnn()` 通过环境变量分支选择 5 种搜索模式，`TWO_STAGE=1` 是其中之一，非唯一路径 | `INTENT-DEC-008`: 将两阶段搜索描述为"内存卸载的核心机制"，做了详细的 tradeoff 分析（3.5x 延迟换 2.7x 内存） | `事实偏差` |

**说明**：OBS 显示两阶段搜索是 5 种模式之一（Beam/NonBlock/BatchIO/TwoStage/Default），由环境变量控制。INTENT 将其提升为"核心机制"，但代码中 `BEAM_WIDTH`/`NONBLOCK`/`BATCH_IO_N` 等模式同样存在且非两阶段。两阶段是当前最优配置，但不是唯一的架构路径。

---

## 14. io_uring vs pread 的角色

> **裁决**: ✅ 维持现状（选项 B）- INTENT-DEC-013 是对 io_uring 选型的补充分析，pread 作为 fallback 不需要详细记录

| 观测事实（OBS-） | 设计意图（INTENT-） | 差距类型 |
|---|---|---|
| `OBS-BEH-001`: Phase B 精排有两条路径--`FINE_PREAD=1`（pread）和 `FINE_PREAD=0`（io_uring），多线程必须用 pread | `INTENT-DEC-013`: 强调 io_uring 是核心选择，补充 `IAsyncIO` Port 作为未来 SPDK 适配器的基础 | `未来目标` |

**说明**：OBS 显示 io_uring 和 pread 是并行的两条路径，io_uring 仅单线程可用。INTENT 聚焦 io_uring 的架构价值，但当前多线程生产路径实际用的是 pread。`IAsyncIO` Port 不存在。
