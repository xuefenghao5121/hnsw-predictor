CXX = g++
CXXFLAGS = -O3 -std=c++17 -Wall -Wextra -I./hnswlib -I./include -march=native
LDFLAGS = -pthread

BUILD_DIR = build

# Core source files (shared by benchmark and test targets)
CORE_SRC = src/core/disk_hnsw.cpp src/core/block_cache.cpp src/core/graph_prefetcher.cpp

# All header files
HEADERS = $(wildcard include/*.h)

# --- Build rules ---

# Pipeline tools (only depend on common.h / hnswlib)
$(BUILD_DIR)/%: src/pipeline/%.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

# Benchmark (depends on all core)
$(BUILD_DIR)/benchmark_overlap: src/benchmark/benchmark_overlap.cpp $(CORE_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/benchmark/benchmark_overlap.cpp $(CORE_SRC) $(LDFLAGS)

# Tests
$(BUILD_DIR)/test_block_cache: src/test/test_block_cache.cpp src/core/block_cache.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/test/test_block_cache.cpp src/core/block_cache.cpp $(LDFLAGS)

$(BUILD_DIR)/test_disk_hnsw: src/test/test_disk_hnsw.cpp $(CORE_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/test/test_disk_hnsw.cpp $(CORE_SRC) $(LDFLAGS)

# Default target
all: $(BUILD_DIR)/build_index $(BUILD_DIR)/extract_graph $(BUILD_DIR)/bfs_reorder \
     $(BUILD_DIR)/write_blocks $(BUILD_DIR)/write_pq_blocks $(BUILD_DIR)/gen_route $(BUILD_DIR)/verify \
     $(BUILD_DIR)/benchmark_overlap $(BUILD_DIR)/test_block_cache $(BUILD_DIR)/test_disk_hnsw

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -f $(BUILD_DIR)/*

.PHONY: all clean
