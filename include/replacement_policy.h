// replacement_policy.h - 可插拔缓存替换策略接口
//
// 功能：
//   定义 ReplacementPolicy 抽象接口，将缓存淘汰逻辑从 BlockCache 中解耦
//   支持多种策略：LRU、LFU、LRU-K
//
// 设计文档: hnsw-research/phase2-design.md

#pragma once

#include <cstdint>
#include <string>
#include <list>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <iostream>

// ============================================================
// ReplacementPolicy: 缓存替换策略抽象接口
// ============================================================

class ReplacementPolicy {
public:
    virtual ~ReplacementPolicy() = default;

    // 节点被访问时调用（更新访问记录）
    virtual void onAccess(uint32_t block_id) = 0;

    // 选择要淘汰的 block_id
    // 返回 UINT32_MAX 表示无法选择（缓存为空或策略不允许淘汰）
    virtual uint32_t selectVictim() const = 0;

    // 新 block 加入缓存时调用
    virtual void onInsert(uint32_t block_id) = 0;

    // block 被移除时调用（淘汰或手动移除）
    virtual void onRemove(uint32_t block_id) = 0;

    // 策略名称
    virtual std::string name() const = 0;

    // 当前跟踪的 block 数量
    virtual size_t size() const = 0;

    // 清空所有跟踪状态
    virtual void clear() = 0;
};

// ============================================================
// LRUPolicy: 最近最少使用淘汰策略
// ============================================================

class LRUPolicy : public ReplacementPolicy {
public:
    void onAccess(uint32_t block_id) override {
        auto it = map_.find(block_id);
        if (it == map_.end()) return;  // 不在缓存中，忽略

        // 移到链表前端
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        it->second = lru_list_.begin();
    }

    uint32_t selectVictim() const override {
        if (lru_list_.empty()) return UINT32_MAX;
        return lru_list_.back();
    }

    void onInsert(uint32_t block_id) override {
        if (map_.find(block_id) != map_.end()) return;  // 已存在
        lru_list_.push_front(block_id);
        map_[block_id] = lru_list_.begin();
    }

    void onRemove(uint32_t block_id) override {
        auto it = map_.find(block_id);
        if (it == map_.end()) return;
        lru_list_.erase(it->second);
        map_.erase(it);
    }

    std::string name() const override { return "lru"; }
    size_t size() const override { return map_.size(); }

    void clear() override {
        lru_list_.clear();
        map_.clear();
    }

private:
    std::list<uint32_t> lru_list_;  // front = 最近使用, back = 最久未使用
    std::unordered_map<uint32_t, std::list<uint32_t>::iterator> map_;
};

// ============================================================
// LFUPolicy: 最少使用频率淘汰策略
// ============================================================

class LFUPolicy : public ReplacementPolicy {
public:
    void onAccess(uint32_t block_id) override {
        auto it = freq_map_.find(block_id);
        if (it == freq_map_.end()) return;

        // 增加访问频率
        it->second.frequency++;
    }

    uint32_t selectVictim() const override {
        if (freq_map_.empty()) return UINT32_MAX;

        // 选择频率最低的 block
        uint32_t victim = UINT32_MAX;
        uint64_t min_freq = UINT64_MAX;
        uint64_t oldest_access = UINT64_MAX;

        for (const auto& [id, info] : freq_map_) {
            if (info.frequency < min_freq ||
                (info.frequency == min_freq && info.last_access < oldest_access)) {
                min_freq = info.frequency;
                oldest_access = info.last_access;
                victim = id;
            }
        }
        return victim;
    }

    void onInsert(uint32_t block_id) override {
        if (freq_map_.find(block_id) != freq_map_.end()) return;
        freq_map_[block_id] = FreqInfo{1, ++counter_};
    }

    void onRemove(uint32_t block_id) override {
        freq_map_.erase(block_id);
    }

    std::string name() const override { return "lfu"; }
    size_t size() const override { return freq_map_.size(); }

    void clear() override {
        freq_map_.clear();
        counter_ = 0;
    }

private:
    struct FreqInfo {
        uint64_t frequency;
        uint64_t last_access;  // 逻辑时间戳
    };

    std::unordered_map<uint32_t, FreqInfo> freq_map_;
    uint64_t counter_ = 0;  // 全局逻辑时钟
};

// ============================================================
// LRUKPolicy: LRU-K (K=2) 淘汰策略
// ============================================================

class LRUKPolicy : public ReplacementPolicy {
public:
    static constexpr size_t K = 2;  // 考虑最近 K 次访问

    void onAccess(uint32_t block_id) override {
        auto it = access_history_.find(block_id);
        if (it == access_history_.end()) return;

        auto& history = it->second;
        history.push_back(++clock_);

        // 只保留最近 K 次访问时间
        if (history.size() > K) {
            history.erase(history.begin());
        }
    }

    uint32_t selectVictim() const override {
        if (access_history_.empty()) return UINT32_MAX;

        uint32_t victim = UINT32_MAX;
        uint64_t oldest_kth = UINT64_MAX;  // 第 K 次访问最早的时间
        uint64_t oldest_first = UINT64_MAX; // 第一次访问最早的时间（tiebreaker）

        for (const auto& [id, history] : access_history_) {
            uint64_t kth_time;
            uint64_t first_time;

            if (history.size() < K) {
                // 不足 K 次访问的 block 优先淘汰（kth_time = 0 表示最优先）
                kth_time = 0;
                first_time = history.empty() ? 0 : history.front();
            } else {
                kth_time = history.front();   // 最早的第 K 次访问时间
                first_time = history.front();
            }

            if (kth_time < oldest_kth ||
                (kth_time == oldest_kth && first_time < oldest_first)) {
                oldest_kth = kth_time;
                oldest_first = first_time;
                victim = id;
            }
        }
        return victim;
    }

    void onInsert(uint32_t block_id) override {
        if (access_history_.find(block_id) != access_history_.end()) return;
        access_history_[block_id] = {++clock_};
    }

    void onRemove(uint32_t block_id) override {
        access_history_.erase(block_id);
    }

    std::string name() const override { return "lru-k"; }
    size_t size() const override { return access_history_.size(); }

    void clear() override {
        access_history_.clear();
        clock_ = 0;
    }

private:
    // block_id -> 访问时间戳列表（按时间递增，最多保留 K 个）
    std::unordered_map<uint32_t, std::vector<uint64_t>> access_history_;
    uint64_t clock_ = 0;  // 全局逻辑时钟
};
