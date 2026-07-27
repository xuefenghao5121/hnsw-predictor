// layout_provider.h - 可插拔布局编排器接口
//
// 功能：
//   定义 LayoutProvider 抽象接口，将 NodeID -> BlockID 的映射逻辑解耦
//   支持多种布局策略：BFS重排、随机分配、（后续可扩展）局部性优化等
//
// 设计文档: hnsw-research/phase2-design.md

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <random>
#include <stdexcept>
#include <cstring>
#include <iostream>

#include "common.h"

// ============================================================
// LayoutProvider: 布局编排器抽象接口
// ============================================================

class LayoutProvider {
public:
    virtual ~LayoutProvider() = default;

    // 获取节点所在的 Block ID
    virtual uint32_t getBlockId(uint32_t node_id) const = 0;

    // 获取 Block 总数
    virtual uint32_t getNumBlocks() const = 0;

    // 获取节点总数
    virtual uint32_t getNumNodes() const = 0;

    // 布局名称（用于日志和调试）
    virtual std::string name() const = 0;
};

// ============================================================
// BfsLayoutProvider: 从 route_table.bin 加载的 BFS 重排布局
// ============================================================

class BfsLayoutProvider : public LayoutProvider {
public:
    // 从 route_table.bin 文件加载路由表
    // route_path: route_table.bin 文件路径
    // expected_num_blocks: 预期的 Block 数量（从 blocks.bin 头读取，0 表示不检查）
    explicit BfsLayoutProvider(const std::string& route_path,
                                uint32_t expected_num_blocks = 0)
        : num_blocks_(expected_num_blocks)
    {
        std::ifstream in(route_path, std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error("BfsLayoutProvider: Cannot open route file: " + route_path);
        }

        RouteHeader hdr;
        in.read(reinterpret_cast<char*>(&hdr), sizeof(RouteHeader));
        if (hdr.magic != MAGIC_ROUTE) {
            throw std::runtime_error("BfsLayoutProvider: Invalid route file magic");
        }

        route_table_.resize(hdr.num_entries);
        in.read(reinterpret_cast<char*>(route_table_.data()),
                hdr.num_entries * sizeof(uint32_t));
        in.close();

        if (num_blocks_ == 0) {
            // 从路由表中推导 num_blocks
            for (uint32_t v : route_table_) {
                if (v + 1 > num_blocks_) num_blocks_ = v + 1;
            }
        }

        std::cout << "[BfsLayoutProvider] Loaded route table: "
                  << route_table_.size() << " entries, "
                  << num_blocks_ << " blocks" << std::endl;
    }

    // 从内存中的路由表构造（用于测试或程序内构造）
    explicit BfsLayoutProvider(std::vector<uint32_t> route_table,
                                uint32_t num_blocks)
        : route_table_(std::move(route_table))
        , num_blocks_(num_blocks)
    {
        std::cout << "[BfsLayoutProvider] Constructed from memory: "
                  << route_table_.size() << " entries, "
                  << num_blocks_ << " blocks" << std::endl;
    }

    uint32_t getBlockId(uint32_t node_id) const override {
        if (node_id >= route_table_.size()) {
            return UINT32_MAX;
        }
        return route_table_[node_id];
    }

    uint32_t getNumBlocks() const override {
        return num_blocks_;
    }

    uint32_t getNumNodes() const override {
        return route_table_.size();
    }

    std::string name() const override { return "bfs"; }

    // 获取路由表（供 BlockCache 内部使用）
    const std::vector<uint32_t>& getRouteTable() const { return route_table_; }

private:
    std::vector<uint32_t> route_table_;  // node_id -> block_id
    uint32_t num_blocks_;
};

// ============================================================
// RandomLayoutProvider: 随机分配节点到 Block（对照组用）
// ============================================================

class RandomLayoutProvider : public LayoutProvider {
public:
    // 随机分配 num_nodes 个节点到 num_blocks 个 Block
    // seed: 随机种子（确保可复现）
    RandomLayoutProvider(uint32_t num_nodes,
                         uint32_t num_blocks,
                         uint64_t seed = 42)
        : route_table_(num_nodes)
        , num_blocks_(num_blocks)
    {
        std::mt19937_64 rng(seed);
        std::uniform_int_distribution<uint32_t> dist(0, num_blocks - 1);

        for (uint32_t i = 0; i < num_nodes; i++) {
            route_table_[i] = dist(rng);
        }

        std::cout << "[RandomLayoutProvider] Random layout: "
                  << num_nodes << " nodes, "
                  << num_blocks_ << " blocks, seed=" << seed << std::endl;
    }

    uint32_t getBlockId(uint32_t node_id) const override {
        if (node_id >= route_table_.size()) {
            return UINT32_MAX;
        }
        return route_table_[node_id];
    }

    uint32_t getNumBlocks() const override {
        return num_blocks_;
    }

    uint32_t getNumNodes() const override {
        return route_table_.size();
    }

    std::string name() const override { return "random"; }

    const std::vector<uint32_t>& getRouteTable() const { return route_table_; }

private:
    std::vector<uint32_t> route_table_;
    uint32_t num_blocks_;
};
