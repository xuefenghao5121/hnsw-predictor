#!/bin/bash
# run_full_benchmark.sh - Run all 4 query sets × 7 configs = 28 tests
set -e

cd /home/huawei/hnsw-predictor

GRAPH=output/test1m_graph.bin
BFS=output/test1m_bfs.bin
BLOCKS=output/test1m_blocks.bin
ROUTE=output/test1m_route.bin
DATA=data/test_1m.fvecs
QUERY=data/test_1m_query1k.fvecs
GT=data/test_1m_gt1k.bin
K=10
EF=50

mkdir -p logs

echo "========================================================"
echo "  Full Benchmark: 4 query sets × 7 configs = 28 tests"
echo "========================================================"

# Query set 1: 100 queries
echo ""
echo "=========================================="
echo "  Query Set 1/4: 100 queries (no shuffle)"
echo "=========================================="
./build/benchmark_full $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF 100 0

# Query set 2: 500 queries
echo ""
echo "=========================================="
echo "  Query Set 2/4: 500 queries (no shuffle)"
echo "=========================================="
./build/benchmark_full $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF 500 0

# Query set 3: 1000 queries (no shuffle)
echo ""
echo "=========================================="
echo "  Query Set 3/4: 1000 queries (no shuffle)"
echo "=========================================="
./build/benchmark_full $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF 1000 0

# Query set 4: 1000 queries (shuffled, seed=42)
echo ""
echo "=========================================="
echo "  Query Set 4/4: 1000 queries (shuffled, seed=42)"
echo "=========================================="
./build/benchmark_full $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF 1000 1

echo ""
echo "========================================================"
echo "  All 28 tests complete!"
echo "  Results in logs/full_benchmark_*.jsonl"
echo "========================================================"
