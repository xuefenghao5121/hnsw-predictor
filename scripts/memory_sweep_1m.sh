#!/bin/bash
# memory_sweep_1m.sh - 1M 数据集内存受限对比测试
# hnswlib native vs DiskHNSW, 不同 cgroup 内存限制

set -e
cd /home/huawei/hnsw-predictor

GRAPH=output/test1m_graph.bin
BFS=output/test1m_bfs.bin
BLOCKS=output/test1m_blocks_64k.bin
ROUTE=output/test1m_route_64k.bin
DATA=data/test_1m.fvecs
QUERY=data/test_1m_query1k.fvecs
GT=data/test_1m_gt1k.bin
INDEX=output/test1m_index.bin
K=10
EF=50
NQ=200

mkdir -p logs/mem_sweep

echo "========================================================"
echo "  1M Memory Sweep: hnswlib vs DiskHNSW"
echo "  Queries: $NQ, K=$K, EF=$EF"
echo "========================================================"

# Memory limits to test (MB)
MEM_LIST="150 200 256 300 400 512 700"

for MEM_MB in $MEM_LIST; do
    echo ""
    echo "============================================"
    echo "  Memory Limit: ${MEM_MB}MB"
    echo "============================================"
    
    # 1. hnswlib native (F0 only) under cgroup
    echo ""
    echo "--- [1] hnswlib native @ ${MEM_MB}MB ---"
    set +e
    systemd-run --user --scope \
        --property=MemoryMax=${MEM_MB}M \
        --property=MemorySwapMax=0 \
        env INDEX_PATH=$INDEX \
            CACHE_MB=$MEM_MB \
            SKIP_CONFIGS="batch,beam,concurrent,event,single-256mb" \
            ./build/benchmark_overlap \
                $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF $NQ \
                2>&1 | tee logs/mem_sweep/hnswlib_${MEM_MB}mb.log
    HNSW_EXIT=$?
    set -e
    
    if [ $HNSW_EXIT -ne 0 ]; then
        echo ">>> hnswlib FAILED at ${MEM_MB}MB (likely OOM)"
    fi
    
    # 2. DiskHNSW (skip F0) under same cgroup
    echo ""
    echo "--- [2] DiskHNSW @ ${MEM_MB}MB ---"
    set +e
    systemd-run --user --scope \
        --property=MemoryMax=${MEM_MB}M \
        --property=MemorySwapMax=0 \
        env SKIP_F0=1 \
            CACHE_MB=$MEM_MB \
            ./build/benchmark_overlap \
                $GRAPH $BFS $BLOCKS $ROUTE $DATA $QUERY $GT $K $EF $NQ \
                2>&1 | tee logs/mem_sweep/diskhnsw_${MEM_MB}mb.log
    DISK_EXIT=$?
    set -e
    
    if [ $DISK_EXIT -ne 0 ]; then
        echo ">>> DiskHNSW FAILED at ${MEM_MB}MB"
    fi
    
    echo "=== ${MEM_MB}MB done ==="
done

echo ""
echo "========================================================"
echo "  All tests complete!"
echo "  Results in logs/mem_sweep/"
echo "========================================================"
