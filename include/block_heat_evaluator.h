// block_heat_evaluator.h - Block 热度评价器
// 在线追踪每个 block 的访问热度，用于指导预取和淘汰
#ifndef BLOCK_HEAT_EVALUATOR_H
#define BLOCK_HEAT_EVALUATOR_H

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

class BlockHeatEvaluator {
public:
    // 构造：num_blocks 个 block，衰减因子默认 0.995
    explicit BlockHeatEvaluator(uint32_t num_blocks, float decay = 0.995f)
        : block_heat_(num_blocks), decay_factor_(decay), query_counter_(0) {}

    // 查询开始时调用（触发全局衰减）
    void onQueryStart() {
        query_counter_++;
    }

    // 访问 block 时更新热度（轻量级，~5 条指令）
    void onBlockAccess(uint32_t block_id) {
        if (block_id >= block_heat_.size()) return;
        auto& h = block_heat_[block_id];
        h.heat = h.heat * decay_factor_ + 1.0f;
        h.access_count++;
    }

    // 查询结束时全局衰减（让旧的访问逐渐遗忘）
    void onQueryEnd() {
        // 轻量衰减：只衰减，不重置
        // 如果担心开销，可以每 N 次查询才衰减一次
        for (auto& h : block_heat_) {
            h.heat *= decay_factor_;
        }
    }

    // 获取 block 热度
    float getHeat(uint32_t block_id) const {
        if (block_id >= block_heat_.size()) return 0.0f;
        return block_heat_[block_id].heat;
    }

    // 获取访问次数
    uint32_t getAccessCount(uint32_t block_id) const {
        if (block_id >= block_heat_.size()) return 0;
        return block_heat_[block_id].access_count;
    }

    // 判断是否为冷 block（热度低于阈值）
    bool isCold(uint32_t block_id, float threshold) const {
        return getHeat(block_id) < threshold;
    }

    // 计算自适应阈值（中位数）
    float getMedianHeat() const {
        if (block_heat_.empty()) return 0.0f;
        std::vector<float> heats;
        heats.reserve(block_heat_.size());
        for (const auto& h : block_heat_) {
            if (h.heat > 0.01f) heats.push_back(h.heat);
        }
        if (heats.empty()) return 0.0f;
        std::sort(heats.begin(), heats.end());
        return heats[heats.size() / 2];
    }

    // 统计信息
    struct Stats {
        uint32_t num_hot_blocks;    // heat > 10
        uint32_t num_warm_blocks;   // 1 < heat <= 10
        uint32_t num_cold_blocks;   // heat <= 1
        uint32_t num_never_accessed;// access_count == 0
        float median_heat;
    };

    Stats getStats() const {
        Stats s = {};
        s.median_heat = getMedianHeat();
        for (const auto& h : block_heat_) {
            if (h.access_count == 0) s.num_never_accessed++;
            else if (h.heat > 10.0f) s.num_hot_blocks++;
            else if (h.heat > 1.0f) s.num_warm_blocks++;
            else s.num_cold_blocks++;
        }
        return s;
    }

    // 重置（新配置时调用）
    void reset() {
        std::fill(block_heat_.begin(), block_heat_.end(), BlockHeat{});
        query_counter_ = 0;
    }

    uint32_t getQueryCount() const { return query_counter_; }

private:
    struct BlockHeat {
        float heat = 0.0f;
        uint32_t access_count = 0;
    };

    std::vector<BlockHeat> block_heat_;
    float decay_factor_;
    uint32_t query_counter_;
};

#endif // BLOCK_HEAT_EVALUATOR_H
