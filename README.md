# HNSW-Predictor

Disk-based HNSW vector search with I/O overlap optimization.

## Requirements

- Linux 5.1+ (io_uring)
- g++ with C++17
- 1GB+ RAM

## Quick Start

### 1. Build

```bash
make all
```

### 2. Prepare Data

Download SIFT1M dataset into `data/`:
- `data/test_1m.fvecs` (base vectors)
- `data/test_1m_query200.fvecs` (query vectors)
- `data/test_1m_gt200.bin` (ground truth)

### 3. Build Index (Pipeline)

```bash
# Step 1: Build HNSW graph
./build/build_index data/test_1m.fvecs output/test1m_index.bin 16 200

# Step 2: Extract graph structure
./build/extract_graph output/test1m_index.bin output/test1m_graph.bin 128

# Step 3: BFS reorder (locality optimization)
./build/bfs_reorder output/test1m_graph.bin output/test1m_bfs.bin

# Step 4: Write blocks (8KB per block)
./build/write_blocks output/test1m_graph.bin output/test1m_bfs.bin output/test1m_blocks_8k.bin 8192

# Step 5: Generate route table
./build/gen_route output/test1m_blocks_8k.bin output/test1m_route_8k.bin

# Step 6: Verify
./build/verify output/test1m_graph.bin output/test1m_bfs.bin output/test1m_blocks_8k.bin output/test1m_route_8k.bin
```

### 4. Run Benchmark

```bash
# Warmup-aligned benchmark (recommended)
bash scripts/warmup_all_approaches.sh

# Or directly
./build/benchmark_overlap \
  output/test1m_graph.bin \
  output/test1m_bfs.bin \
  output/test1m_blocks_8k.bin \
  output/test1m_route_8k.bin \
  data/test_1m.fvecs \
  data/test_1m_query200.fvecs \
  data/test_1m_gt200.bin \
  10 50 200
```

## Architecture

### Core Components

- **DiskHNSW**: Disk-based HNSW search engine
- **BlockCache**: LRU block cache with O_DIRECT
- **GraphPrefetcher**: io_uring async I/O with graph-guided prefetch

### Search Modes

- **Blocking**: Standard best-first search (sequential I/O)
- **Beam Search**: Cache-aware beam search with lowerBound freezing
- **Event-Driven**: Multi-query concurrent search (single thread, no locks)
- **Multi-thread**: N threads with shared cache

### Configurations

- Block size: 8KB (optimal)
- Cache size: 256MB (c32768 slots)
- I/O: O_DIRECT + io_uring

### Environment Variables

- `BEAM_WIDTH`: Beam search width (0=standard, >0=beam)
- `NONBLOCK`: Non-blocking search (0=blocking, 1=non-blocking)

## Key Results (SIFT1M, 200 queries)

| Config | QPS | Recall | Memory |
|--------|-----|--------|--------|
| F0: Full-mem | 302 | 100% | 482MB |
| F2-single (blocking) | 109 | 100% | 375MB |
| F2-event-8 (event-driven) | 208 | 99.7% | 387MB |

## Project Structure

```
hnsw-predictor/
├── README.md              # This file
├── Makefile               # Build system
├── hnswlib/               # HNSW library (external)
├── include/               # Public headers
├── src/
│   ├── core/              # Core library (DiskHNSW, BlockCache, GraphPrefetcher)
│   ├── pipeline/          # Index build tools (build, extract, reorder, blocks, route, verify)
│   ├── benchmark/         # Benchmarks
│   └── test/              # Unit tests
├── scripts/               # Shell scripts
├── approaches/            # Design documents for each approach
├── data/                  # Data (gitignored)
├── output/                # Output (gitignored)
├── build/                 # Build artifacts (gitignored)
└── logs/                  # Logs (gitignored)
```
