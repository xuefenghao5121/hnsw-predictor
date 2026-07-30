CXX = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra -I./hnswlib -I./include -march=native
LDFLAGS = -pthread

BUILD_DIR = build

# 核心库 (benchmark 和大部分 test 共享)
CORE_SRC = src/core/disk_hnsw.cpp src/core/block_cache.cpp src/core/graph_prefetcher.cpp

HEADERS = $(wildcard include/*.h)

# --- 构建规则 ---

# Pipeline 工具 (只依赖 common.h / hnswlib)
$(BUILD_DIR)/%: src/pipeline/%.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# 主 benchmark (依赖全部 core) —— 原 Makefile 漏了这个, 已补
$(BUILD_DIR)/benchmark_diskhnsw: src/benchmark/benchmark_diskhnsw.cpp $(CORE_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/benchmark/benchmark_diskhnsw.cpp $(CORE_SRC) $(LDFLAGS)

# hnswlib 全内存对比基线
$(BUILD_DIR)/benchmark_hnswlib_native: src/benchmark/benchmark_hnswlib_native.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/benchmark/benchmark_hnswlib_native.cpp $(LDFLAGS)

# 测试
$(BUILD_DIR)/test_block_cache: src/test/test_block_cache.cpp src/core/block_cache.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/test/test_block_cache.cpp src/core/block_cache.cpp $(LDFLAGS)

$(BUILD_DIR)/test_disk_hnsw: src/test/test_disk_hnsw.cpp $(CORE_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/test/test_disk_hnsw.cpp $(CORE_SRC) $(LDFLAGS)

$(BUILD_DIR)/test_pq_search_quality: src/test/test_pq_search_quality.cpp $(CORE_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/test/test_pq_search_quality.cpp $(CORE_SRC) $(LDFLAGS)

# 数据准备工具 (Step 1-5)
PIPELINE = $(BUILD_DIR)/build_index $(BUILD_DIR)/extract_graph $(BUILD_DIR)/bfs_reorder \
           $(BUILD_DIR)/write_blocks $(BUILD_DIR)/write_blocks_veconly \
           $(BUILD_DIR)/write_pq_blocks $(BUILD_DIR)/gen_route $(BUILD_DIR)/verify

# benchmark
BENCH = $(BUILD_DIR)/benchmark_diskhnsw $(BUILD_DIR)/benchmark_hnswlib_native

# 测试
TESTS = $(BUILD_DIR)/test_block_cache $(BUILD_DIR)/test_disk_hnsw $(BUILD_DIR)/test_pq_search_quality

all: $(PIPELINE) $(BENCH) $(TESTS)

pipeline: $(PIPELINE)
bench: $(BENCH)
test: $(TESTS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -f $(BUILD_DIR)/*

.PHONY: all pipeline bench test clean

