# Makefile for HNSW Predictor Phase 1 & 2

CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall -Wextra -I./hnswlib
LDFLAGS = -pthread

BUILD_DIR = build
SRC_DIR = src

# 默认目标: 编译所有工具
all: $(BUILD_DIR)/build_index $(BUILD_DIR)/extract_graph $(BUILD_DIR)/bfs_reorder $(BUILD_DIR)/write_blocks $(BUILD_DIR)/gen_route $(BUILD_DIR)/verify $(BUILD_DIR)/test_block_cache $(BUILD_DIR)/test_disk_hnsw

# 创建 build 目录
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# 构建 HNSW 索引
$(BUILD_DIR)/build_index: $(SRC_DIR)/build_index.cpp $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/build_index.cpp $(LDFLAGS)

# Task 1.1: 提取图结构
$(BUILD_DIR)/extract_graph: $(SRC_DIR)/extract_graph.cpp $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/extract_graph.cpp $(LDFLAGS)

# Task 1.2: BFS 全局重排
$(BUILD_DIR)/bfs_reorder: $(SRC_DIR)/bfs_reorder.cpp $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/bfs_reorder.cpp $(LDFLAGS)

# Task 1.3: 切分 Block 并落盘
$(BUILD_DIR)/write_blocks: $(SRC_DIR)/write_blocks.cpp $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/write_blocks.cpp $(LDFLAGS)

# Task 1.4: 生成路由表
$(BUILD_DIR)/gen_route: $(SRC_DIR)/gen_route.cpp $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/gen_route.cpp $(LDFLAGS)

# Task 2.1: BlockCache 管理器（可插拔接口版本）
$(BUILD_DIR)/test_block_cache: $(SRC_DIR)/test_block_cache.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/test_block_cache.cpp $(SRC_DIR)/block_cache.cpp $(LDFLAGS)

# Task 2.2: DiskHNSW 测试（Phase 3: with graph prefetch support）
$(BUILD_DIR)/test_disk_hnsw: $(SRC_DIR)/test_disk_hnsw.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/test_disk_hnsw.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# 验证工具
$(BUILD_DIR)/verify: $(SRC_DIR)/verify.cpp $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/verify.cpp $(LDFLAGS)

# 完整流程: build -> extract -> bfs -> blocks -> route -> verify
pipeline: all
	@echo "=== Running Phase 1 Pipeline ==="
	./build/build_index data/sift_base.fvecs output/sift_index.bin 16 200
	./build/extract_graph output/sift_index.bin output/graph_structure.bin 128
	./build/bfs_reorder output/graph_structure.bin output/bfs_order.bin
	./build/write_blocks output/graph_structure.bin output/bfs_order.bin output/blocks.bin
	./build/gen_route output/blocks.bin output/route_table.bin
	./build/verify output/graph_structure.bin output/bfs_order.bin output/blocks.bin output/route_table.bin
	@echo "=== Phase 1 Pipeline Complete ==="

# Benchmark 1M
$(BUILD_DIR)/benchmark_1m: $(SRC_DIR)/benchmark_1m.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/benchmark_1m.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# Phase 3a: collect traces v2
$(BUILD_DIR)/collect_traces_v2: $(SRC_DIR)/collect_traces_v2.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/collect_traces_v2.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# Phase 3a: inter-query prefetch benchmark
$(BUILD_DIR)/benchmark_interquery: $(SRC_DIR)/benchmark_interquery.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/benchmark_interquery.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# Phase 3b: mmap benchmark
$(BUILD_DIR)/benchmark_mmap: $(SRC_DIR)/benchmark_mmap.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/benchmark_mmap.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# O_DIRECT cache sweep benchmark
$(BUILD_DIR)/benchmark_odirect_sweep: $(SRC_DIR)/benchmark_odirect_sweep.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/benchmark_odirect_sweep.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# D4 Prefetch benchmark (OD c1024 + graph prefetch)
$(BUILD_DIR)/benchmark_d4_prefetch: $(SRC_DIR)/benchmark_d4_prefetch.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/benchmark_d4_prefetch.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# Phase 3 Redesign: benchmark_phase3 (O_DIRECT + io_uring + graph prefetch)
$(BUILD_DIR)/benchmark_phase3: $(SRC_DIR)/benchmark_phase3.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/benchmark_phase3.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

# Phase 3 v2: cached I/O + fadvise benchmark
$(BUILD_DIR)/benchmark_fadvise: $(SRC_DIR)/benchmark_fadvise.cpp $(SRC_DIR)/disk_hnsw.h $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.h $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/layout_provider.h $(SRC_DIR)/replacement_policy.h $(SRC_DIR)/graph_prefetcher.h $(SRC_DIR)/graph_prefetcher.cpp $(SRC_DIR)/io_uring_wrapper.h $(SRC_DIR)/common.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC_DIR)/benchmark_fadvise.cpp $(SRC_DIR)/disk_hnsw.cpp $(SRC_DIR)/block_cache.cpp $(SRC_DIR)/graph_prefetcher.cpp $(LDFLAGS)

clean:
	rm -f $(BUILD_DIR)/*

.PHONY: all pipeline clean
