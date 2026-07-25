#!/bin/bash
# cgroup_bench.sh - 在 cgroup 内存限制下跑 benchmark
# 测试 1024MB 和 2048MB 两档, 每档 Q=200 + Q=1000

cd /home/huawei/hnsw-predictor

GRAPH=output/deep10m_graph.bin
BFS=output/deep10m_bfs.bin
BLOCKS=output/deep10m_blocks_8k.bin
ROUTE=output/deep10m_route_8k.bin
DATA=data/deep10m_train.fvecs
QUERY=data/deep10m_test.fvecs
GT=data/deep10m_gt.bin

export INDEX_PATH=output/deep10m_index.bin
export SKIP_CONFIGS="batch,beam,concurrent,event"

for MEM_MB in 1024 2048; do
    for NQ in 200 1000; do
        echo ""
        echo "============================================"
        echo "  Memory=${MEM_MB}MB, Q=${NQ}"
        echo "============================================"
        
        CACHE_MB=$MEM_MB \
        systemd-run --user --scope \
            --property=MemoryMax=${MEM_MB}M \
            --property=MemorySwapMax=0 \
            ./build/benchmark_overlap \
                $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT \
                10 50 $NQ 2>&1 | tee logs/benchmark_10m_mem${MEM_MB}_q${NQ}.log
        
        echo "=== mem=${MEM_MB} q=${NQ} done ==="
    done
done

echo ""
echo "=== ALL DONE ==="
