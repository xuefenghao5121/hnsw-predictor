// predictor.cpp - Markov 预测器实现
#include "predictor.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstring>

// ============================================================
// 离线训练
// ============================================================

void MarkovPredictor::trainFromTraces(const std::string& trace_path) {
    std::ifstream in(trace_path);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open trace file: " + trace_path);
    }

    std::string line;
    uint32_t prev_block = UINT32_MAX;
    int prev_query = -1;
    size_t lines_read = 0;

    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        int query_id;
        uint32_t block_id;
        uint64_t timestamp;
        int is_hit;

        if (!(iss >> query_id >> block_id >> timestamp >> is_hit)) continue;

        // 同一查询内的连续访问形成转移
        if (prev_query == query_id && prev_block != UINT32_MAX) {
            addTransition(prev_block, block_id);
        }

        prev_query = query_id;
        prev_block = block_id;
        lines_read++;
    }

    in.close();
    std::cout << "[MarkovPredictor] Trained from " << trace_path
              << ": " << lines_read << " accesses, "
              << transitions_.size() << " states, "
              << total_transitions_ << " transitions" << std::endl;
}

void MarkovPredictor::addTransition(uint32_t current_block, uint32_t next_block) {
    auto& vec = transitions_[current_block];
    bool found = false;
    for (auto& [bid, cnt] : vec) {
        if (bid == next_block) {
            cnt++;
            found = true;
            break;
        }
    }
    if (!found) {
        vec.emplace_back(next_block, 1);
    }
    out_degree_[current_block]++;
    total_transitions_++;
}

// ============================================================
// 模型持久化
// ============================================================

#pragma pack(push, 1)
struct ModelHeader {
    uint32_t magic;       // 0x4D41524B ("MARK")
    uint32_t version;     // 1
    uint32_t num_states;  // 转移表中的状态数
    uint32_t reserved;
};
#pragma pack(pop)

static constexpr uint32_t MAGIC_MARKOV = 0x4D41524B;

void MarkovPredictor::saveModel(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("Cannot create model file: " + path);
    }

    ModelHeader hdr;
    hdr.magic = MAGIC_MARKOV;
    hdr.version = 1;
    hdr.num_states = static_cast<uint32_t>(transitions_.size());
    hdr.reserved = 0;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(ModelHeader));

    for (const auto& [block_id, transitions] : transitions_) {
        uint32_t bid = block_id;
        out.write(reinterpret_cast<const char*>(&bid), sizeof(uint32_t));

        uint32_t out_deg = out_degree_.at(block_id);
        out.write(reinterpret_cast<const char*>(&out_deg), sizeof(uint32_t));

        uint32_t num_trans = static_cast<uint32_t>(transitions.size());
        out.write(reinterpret_cast<const char*>(&num_trans), sizeof(uint32_t));

        for (const auto& [next_bid, cnt] : transitions) {
            out.write(reinterpret_cast<const char*>(&next_bid), sizeof(uint32_t));
            out.write(reinterpret_cast<const char*>(&cnt), sizeof(uint32_t));
        }
    }

    out.close();
    std::cout << "[MarkovPredictor] Model saved to " << path
              << " (" << transitions_.size() << " states)" << std::endl;
}

void MarkovPredictor::loadModel(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("Cannot open model file: " + path);
    }

    ModelHeader hdr;
    in.read(reinterpret_cast<char*>(&hdr), sizeof(ModelHeader));
    if (hdr.magic != MAGIC_MARKOV) {
        throw std::runtime_error("Invalid model file: bad magic");
    }

    transitions_.clear();
    out_degree_.clear();
    total_transitions_ = 0;

    for (uint32_t i = 0; i < hdr.num_states; i++) {
        uint32_t bid, out_deg, num_trans;
        in.read(reinterpret_cast<char*>(&bid), sizeof(uint32_t));
        in.read(reinterpret_cast<char*>(&out_deg), sizeof(uint32_t));
        in.read(reinterpret_cast<char*>(&num_trans), sizeof(uint32_t));

        std::vector<std::pair<uint32_t, uint32_t>> trans;
        trans.reserve(num_trans);
        for (uint32_t j = 0; j < num_trans; j++) {
            uint32_t next_bid, cnt;
            in.read(reinterpret_cast<char*>(&next_bid), sizeof(uint32_t));
            in.read(reinterpret_cast<char*>(&cnt), sizeof(uint32_t));
            trans.emplace_back(next_bid, cnt);
        }
        transitions_[bid] = std::move(trans);
        out_degree_[bid] = out_deg;
        total_transitions_ += out_deg;
    }

    in.close();
    std::cout << "[MarkovPredictor] Model loaded from " << path
              << ": " << transitions_.size() << " states, "
              << total_transitions_ << " transitions" << std::endl;
}

// ============================================================
// 在线推理
// ============================================================

std::vector<uint32_t> MarkovPredictor::predict(uint32_t current_block, size_t top_k) const {
    std::vector<uint32_t> result;
    result.reserve(top_k);

    auto it = transitions_.find(current_block);
    if (it == transitions_.end() || it->second.empty()) {
        return result;  // 无数据，返回空
    }

    const auto& trans = it->second;

    // 按计数降序排序
    std::vector<std::pair<uint32_t, uint32_t>> sorted_trans = trans;
    std::sort(sorted_trans.begin(), sorted_trans.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    for (size_t i = 0; i < std::min(top_k, sorted_trans.size()); i++) {
        result.push_back(sorted_trans[i].first);
    }

    return result;
}

// ============================================================
// 在线学习
// ============================================================

void MarkovPredictor::onlineUpdate(uint32_t current_block, uint32_t next_block) {
    std::lock_guard<std::mutex> lock(mutex_);
    addTransition(current_block, next_block);
}

// ============================================================
// 统计
// ============================================================

void MarkovPredictor::printSummary() const {
    std::cout << "[MarkovPredictor] Summary:" << std::endl;
    std::cout << "  States: " << transitions_.size() << std::endl;
    std::cout << "  Total transitions: " << total_transitions_ << std::endl;

    // 计算平均出度
    double avg_out = 0;
    if (!transitions_.empty()) {
        for (const auto& [bid, trans] : transitions_) {
            avg_out += trans.size();
        }
        avg_out /= transitions_.size();
    }
    std::cout << "  Avg out-degree: " << avg_out << std::endl;

    // 找 Top-5 最活跃状态
    std::vector<std::pair<uint32_t, uint32_t>> active;
    for (const auto& [bid, trans] : transitions_) {
        uint32_t total = 0;
        for (const auto& [_, cnt] : trans) total += cnt;
        active.emplace_back(bid, total);
    }
    std::sort(active.begin(), active.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::cout << "  Top-5 most active blocks:" << std::endl;
    for (size_t i = 0; i < std::min(5UL, active.size()); i++) {
        std::cout << "    Block " << active[i].first << ": " << active[i].second << " visits" << std::endl;
    }
}
