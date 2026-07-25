#!/bin/bash
# multi_query_bench.sh - 多查询数量 benchmark
# 200, 1000, 10000 queries
# 每轮只测核心配置: F0, F2-single(8MB), F2-single(cache_mb), F2-event
# 跳过: beam, concurrent, batch (太慢)

cd /home/huawei/hnsw-predictor

GRAPH=output/deep10m_graph.bin
BFS=output/deep10m_bfs.bin
BLOCKS=output/deep10m_blocks_8k.bin
ROUTE=output/deep10m_route_8k.bin
DATA=data/deep10m_train.fvecs
QUERY=data/deep10m_test.fvecs
GT=data/deep10m_gt.bin
CACHE_MB=${CACHE_MB:-2048}

export SKIP_CONFIGS="batch,beam,concurrent"

for NQ in 200 1000 10000; do
    echo ""
    echo "============================================"
    echo "  Benchmark: ${NQ} queries, cache=${CACHE_MB}MB"
    echo "============================================"
    
    CACHE_MB=$CACHE_MB ./build/benchmark_overlap \
        $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT \
        10 50 $NQ 2>&1 | tee logs/benchmark_10m_q${NQ}_c${CACHE_MB}.log
    
    echo "=== Q=${NQ} done ==="
done

echo ""
echo "=== ALL DONE ==="
