# Block 热度评价器设计（Block Heat Evaluator）

> 设计日期: 2026-07-24 | 状态: 待审核

## 一、问题分析

### 当前 block 级缓存的浪费

每个 256KB block 包含 ~450 个 SIFT 节点。搜索只访问其中一小部分。

VeloANN 数据：47% 顶点从不被访问，但只有 0.1% 的 page 不被访问。
推算到我们的场景：一个 block 被 I/O 加载到缓存，但其中可能只有 5-20 个节点被实际访问（1-4%利用率）。

**问题本质**：不是缓存大小不够，是 **缓存空间利用率太低**。

### 你的设计思路

不改变 block 级 I/O 基础设施，而是：
1. **在线追踪**每个 block 内节点的访问频率
2. 计算 block **热度分数**（heat score）
3. 用热度分数指导：
   - **预取优先级**：热 block 优先预取，冷 block 延迟或跳过
   - **淘汰策略**：冷 block 优先淘汰，热 block 保留更久
   - **预取跳过**：历史冷的 block 不预取，等真正需要时才同步加载

### 优势（对比 VeloANN record 级缓存）

| | VeloANN record 级 | 我们的 Block 热度评价器 |
|--|---|---|
| 需要训练数据 | 是（离线分析） | **否（在线学习）** |
| 适应实时变化 | 否（固定缓存布局） | **是（指数衰减）** |
| I/O 粒度 | record 级（需改 I/O 栈） | **block 级（现有基础设施）** |
| 缓存粒度 | record 级（精细） | **block 级 + 热度权重** |
| 实现复杂度 | 高（重写缓存层） | **低（在现有缓存上加一层）** |

---

## 二、评价器设计

### 2.1 数据结构

```cpp
// 每个 block 的热度信息（轻量级，~16 bytes per block）
struct BlockHeat {
    float heat;              // 指数衰减热度分数（核心指标）
    uint16_t access_count;   // 累计访问次数（用于计算密度）
    uint8_t  node_hits;      // 被访问的不同节点数（估计值，用 bitmap 压缩）
    uint8_t  pad;
};

// 全局热度表（2225 个 block × 16 bytes ≈ 35KB）
class BlockHeatEvaluator {
    std::vector<BlockHeat> block_heat_;   // index = block_id
    float decay_factor_;                   // 衰减因子（默认 0.995）
    uint32_t total_accesses_;              // 全局访问计数
};
```

### 2.2 热度计算

**在线更新（每次访问 block 时）：**

```cpp
void onBlockAccess(uint32_t block_id, uint32_t node_id) {
    auto& h = block_heat_[block_id];
    
    // 指数衰减：旧的访问逐渐遗忘
    h.heat = h.heat * decay_factor_ + 1.0f;
    
    // 访问计数
    h.access_count++;
    
    // 节点命中（用采样估计，避免精确 bitmap 的开销）
    // 简化版：不追踪具体节点，只看访问密度
}
```

**周期性全局衰减（每次查询结束后）：**

```cpp
void decayAll() {
    for (auto& h : block_heat_) {
        h.heat *= decay_factor_;
    }
}
```

### 2.3 热度分数的含义

- `heat > 10`：**热 block** — 近期被频繁访问，应优先缓存和预取
- `1 < heat < 10`：**温 block** — 偶尔被访问，正常缓存
- `heat < 1`：**冷 block** — 很久没被访问或只被访问过一次，可安全淘汰

### 2.4 关键设计：访问密度评估

核心洞察：**一个 block 被加载后，里面有多少节点被实际访问？**

用轻量估计（不做精确 per-node 追踪）：

```cpp
// 方案 A：访问次数 / block 大小（粗略估计密度）
float accessDensity(uint32_t block_id) {
    auto& h = block_heat_[block_id];
    // 每次 searchLayer0 展开 candidate 时访问 1 次 block
    // 但同一个 block 可能在一次搜索中被多次访问（不同 candidate 在同一 block）
    // access_count 高 = block 内多个节点被访问 = 高密度
    return h.access_count / (float)estimated_nodes_per_block_;
}

// 方案 B：用 hash 采样（更精确但稍重）
// 用 8-bit hash bitmap 估计不同节点数
uint8_t estimateDistinctNodes(uint32_t block_id) {
    return block_heat_[block_id].node_hits;
}
```

**推荐方案 A**（最轻量）：不追踪 per-node，只用 block 级访问次数。访问次数高 = 被多次展开 = 热门 block。这是最简单且有效的。

---

## 三、热度评价器如何优化预取

### 3.1 预取优先级排序

当前 `submitPrefetch` 无差别提交所有邻居 block。改进：

```cpp
int GraphPrefetcher::submitPrefetchWithPriority(
    const std::vector<uint32_t>& block_ids,
    const BlockHeatEvaluator& heat_eval) {
    
    // 按热度排序（热 block 优先）
    std::vector<std::pair<float, uint32_t>> scored;
    for (uint32_t bid : block_ids) {
        scored.push_back({heat_eval.getHeat(bid), bid});
    }
    std::sort(scored.begin(), scored.end(),
              [](auto& a, auto& b) { return a.first > b.first; });
    
    // 只预取热度 > 阈值的 block（冷 block 跳过）
    std::vector<uint32_t> to_submit;
    for (auto& [heat, bid] : scored) {
        if (heat < COLD_THRESHOLD && !cache_->isInCache(bid)) {
            // 冷 block 且不在缓存：跳过预取，等真正需要时同步加载
            stats_.prefetch_skipped_cold++;
            continue;
        }
        to_submit.push_back(bid);
    }
    
    return submitPrefetch(to_submit, true);
}
```

### 3.2 自适应阈值

```cpp
// 冷热阈值不是固定的，根据当前工作集自适应
float adaptiveThreshold() {
    // 取所有 block 热度的中位数作为阈值
    // 上半部分 = 热，下半部分 = 冷
    // 这样总能预取"相对热"的 block
}
```

### 3.3 冷启动处理

前几次查询没有热度数据（所有 block heat = 0），此时：
- **不跳过任何预取**（默认全部预取，等于当前行为）
- 随着查询积累，热度数据逐渐建立
- 从第 5-10 次查询开始，热度评价器开始发挥作用

---

## 四、热度评价器如何优化缓存淘汰

### 4.1 热度加权 LRU

当前用纯 LRU（最近最少使用）。改进为 **Heat-weighted LRU**：

```cpp
// 淘汰评分：heat 高的 block 不容易被淘汰
float evictionScore(const BlockHeat& h, uint64_t last_access_time) {
    float recency = (now - last_access_time) / TIME_WINDOW;
    return recency - h.heat * HEAT_WEIGHT;  // heat 高 -> score 低 -> 不容易被淘汰
}
```

效果：
- 一个 block 刚被访问（recency 低）但热度也低（只访问过一次）→ 正常淘汰
- 一个 block 访问时间稍早（recency 中等）但热度很高（经常被访问）→ 保留

### 4.2 实现

修改 `LRUPolicy` 为 `HeatWeightedLRUPolicy`：

```cpp
class HeatWeightedLRUPolicy : public ReplacementPolicy {
    BlockHeatEvaluator* heat_eval_;
    
    uint32_t selectVictim(const std::unordered_map<uint32_t, CacheEntry>& cache) {
        // 找到淘汰评分最高的 block（最应该被淘汰的）
        float max_score = -INF;
        uint32_t victim = INVALID;
        for (auto& [bid, entry] : cache) {
            float score = entry.recency() - heat_eval_->getHeat(bid) * 0.5f;
            if (score > max_score) {
                max_score = score;
                victim = bid;
            }
        }
        return victim;
    }
};
```

---

## 五、预期效果

### 5.1 命中率提升

当前 86.8% 命中率意味着 13.2% 的访问需要 I/O。其中一部分是**真 miss**（block 从未被加载），另一部分是**淘汰 miss**（block 被加载过但已被淘汰）。

热度加权淘汰可以：
- 减少热 block 的淘汰 → 减少淘汰 miss → 命中率提升
- 预估：命中率从 86.8% 提升到 90-93%（减少 30-50% 的淘汰 miss）

### 5.2 I/O 量减少

冷 block 跳过预取 → 减少无效 I/O → PF submitted 降低
预估：PF submitted 从 48873 降低到 25000-30000（减少 40-50%）

### 5.3 性能提升预估

- 命中率提升 + I/O 量减少 → Mean 从 41ms 降到 25-30ms
- QPS 从 24 提升到 35-45

---

## 六、实现计划

### Phase 1: BlockHeatEvaluator 基础版
- 数据结构：per-block heat counter（16 bytes × 2225 = 35KB）
- 在 searchLayer0 的 getCachedBlockById 和 getNodeVector 路径更新热度
- 每次查询结束做全局衰减
- **不改变预取和淘汰逻辑**，只收集数据

### Phase 2: 热度引导预取
- submitPrefetch 加入热度过滤：冷 block 跳过预取
- 自适应阈值（中位数）
- 验证 PF submitted 降低

### Phase 3: 热度加权淘汰
- LRUPolicy 改为 HeatWeightedLRUPolicy
- 验证命中率提升

### 工作量估算
- Phase 1: ~50 行新代码
- Phase 2: ~30 行修改
- Phase 3: ~40 行修改
- 总计: ~120 行

---

## 七、与 ML 预取的关系

Block 热度评价器是 **Phase 4 ML 预取的基础设施**：
- 热度数据可以作为 ML 模型的特征
- "这个 block 的热度" + "当前搜索路径" → 预测下一跳需要哪些 block
- 热度评价器提供了**在线学习的信号**，不需要离线训练数据
