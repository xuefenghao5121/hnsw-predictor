#!/bin/bash
# warmup_all_approaches.sh - warmup 对齐的三方案统一 benchmark
# 先空跑两遍预热顶高频，再背靠背在同一高频窗口内跑完整 benchmark
set -e

cd /home/huawei/hnsw-predictor

GRAPH=output/test1m_graph.bin
BFS=output/test1m_bfs.bin
BLOCKS=output/test1m_blocks_8k.bin
ROUTE=output/test1m_route_8k.bin
DATA=data/test_1m.fvecs
QUERY=data/test_1m_query200.fvecs
GT=data/test_1m_gt200.bin
K=10
EF=50
NQ=200

echo "=========================================="
echo "  Warmup 对齐 - 三方案统一 Benchmark"
echo "  8KB block, c32768 (256MB), 200 queries"
echo "=========================================="

# Step 1: 空跑预热 CPU 频率 (跑两遍)
echo ""
echo "[Warmup 1/2] Pre-heating CPU frequency..."
./build/benchmark_overlap $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF 10 > /dev/null 2>&1

echo "[Warmup 2/2] Pre-heating CPU frequency..."
./build/benchmark_overlap $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF 10 > /dev/null 2>&1

# Step 2: 背靠背跑正式 benchmark
echo ""
echo "[Benchmark] Running 200 queries (warmup aligned)..."
./build/benchmark_overlap $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF $NQ 2>&1 | tee logs/warmup_all_approaches.log

echo ""
echo "Done. Results saved to logs/warmup_all_approaches.log"
