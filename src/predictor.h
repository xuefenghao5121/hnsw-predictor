// predictor.h - Markov 预测器：基于 Block 访问序列预测下一个 Block
//
// 设计要点：
// 1. 离线训练：从访问轨迹学习 P(next|current) 转移概率
// 2. 在线推理：搜索时输入当前 block_id，输出 Top-K 候选
// 3. 轻量级：一阶 Markov 链，O(1) 查询
#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <mutex>
#include <atomic>

class MarkovPredictor {
public:
    MarkovPredictor() = default;
    ~MarkovPredictor() = default;

    // ---- 离线训练 ----

    // 从访问轨迹文件训练
    // 格式: 每行 "query_id block_id timestamp is_hit"
    void trainFromTraces(const std::string& trace_path);

    // 手动添加转移 (current -> next)
    void addTransition(uint32_t current_block, uint32_t next_block);

    // 保存/加载模型
    void saveModel(const std::string& path) const;
    void loadModel(const std::string& path);

    // ---- 在线推理 ----

    // 预测：给定当前 block，返回 Top-K 候选 block_ids
    // 返回按概率降序排列的候选列表
    std::vector<uint32_t> predict(uint32_t current_block, size_t top_k = 3) const;

    // ---- 在线学习（可选）----
    // 搜索过程中实时更新转移概率
    void onlineUpdate(uint32_t current_block, uint32_t next_block);

    // ---- 统计 ----
    size_t getNumStates() const { return transitions_.size(); }
    size_t getTotalTransitions() const { return total_transitions_.load(); }

    // 训练状态
    bool isTrained() const { return total_transitions_ > 0; }

    // 打印模型摘要
    void printSummary() const;

private:
    // 转移表: block_id -> [(next_block_id, count)]
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> transitions_;

    // 每个状态的出度总和（用于归一化）
    std::unordered_map<uint32_t, uint32_t> out_degree_;

    std::atomic<size_t> total_transitions_{0};
    mutable std::mutex mutex_;  // 仅用于 onlineUpdate
};
