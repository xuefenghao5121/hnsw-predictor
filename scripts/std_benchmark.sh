#!/bin/bash
# std_benchmark.sh - 标准测试框架
# 数据集: SIFT1M (1M × 128dim, L2)
# 查询: 200 queries (from ann-benchmarks standard)
# 对比: hnswlib 全内存 vs DiskHNSW 内存受限
#
# 用法:
#   ./scripts/std_benchmark.sh              # 跑全部测试
#   ./scripts/std_benchmark.sh baseline      # 只跑 hnswlib 基线
#   ./scripts/std_benchmark.sh diskhnsw      # 只跑 DiskHNSW

set -e
cd /home/huawei/hnsw-predictor

# === 标准数据 ===
TRAIN=sift1m_train.fvecs
QUERY=sift1m_query200.fvecs
GT=sift1m_gt200.bin
INDEX=output/sift1m_index.bin

# === DiskHNSW 数据 (需要从标准索引生成) ===
GRAPH=output/sift1m_graph.bin
BFS=output/sift1m_bfs.bin
BLOCKS=output/sift1m_blocks_64k.bin
ROUTE=output/sift1m_route_64k.bin

K=10
EF=50
NQ=200

mkdir -p logs/std_bench

MODE=${1:-all}

echo "========================================================"
echo "  Standard SIFT1M Benchmark"
echo "  K=$K, EF=$EF, Queries=$NQ"
echo "========================================================"

# ---- 1. hnswlib 基线 (全内存, 无限制) ----
if [ "$MODE" = "all" ] || [ "$MODE" = "baseline" ]; then
    echo ""
    echo "=== [1] hnswlib Native Baseline (no memory limit) ==="
    ./build/benchmark_hnswlib_native \
        $INDEX data/$QUERY data/$GT $K $EF $NQ \
        2>&1 | tee logs/std_bench/baseline.log
fi

# ---- 2. DiskHNSW 内存扫描 ----
if [ "$MODE" = "all" ] || [ "$MODE" = "diskhnsw" ]; then
    for CM in 128 192 256 384 512; do
        CG=$((CM + 120))
        echo ""
        echo "=== DiskHNSW @ CACHE=${CM}MB cgroup=${CG}MB ==="
        set +e
        systemd-run --user --collect --scope \
            -p MemoryMax=${CG}M \
            -p MemorySwapMax=0 \
            env CACHE_MB=${CM} \
            ./build/benchmark_diskhnsw \
                $GRAPH $BFS $BLOCKS $ROUTE \
                data/$TRAIN data/$QUERY data/$GT \
                $K $EF $NQ 2>&1 | tee logs/std_bench/diskhnsw_${CM}mb.log
        EXIT=${PIPESTATUS[0]}
        set -e
        [ $EXIT -ne 0 ] && echo ">>> FAILED at ${CM}MB (exit=$EXIT)"
    done
fi

# ---- 3. 汇总 ----
echo ""
echo "========================================================"
echo "  SUMMARY"
echo "========================================================"
BL=$(grep "^QPS:" logs/std_bench/baseline.log 2>/dev/null | awk '{print $2}')
BR=$(grep "^Recall:" logs/std_bench/baseline.log 2>/dev/null | awk '{print $2}')
BM=$(grep "^Mean:" logs/std_bench/baseline.log 2>/dev/null | awk '{print $2}')
echo "Baseline (hnswlib, ~726MB): QPS=$BL, Mean=${BM}ms, Recall=${BR}%"
echo ""
printf "%-10s %-10s %-10s %-10s %-10s %-8s %-8s\n" "Cache" "TotalMem" "QPS" "Mean(ms)" "P99(ms)" "Hit%" "Recall%"
printf "%-10s %-10s %-10s %-10s %-10s %-8s %-8s\n" "-----" "--------" "---" "-------" "-------" "----" "-------"
for CM in 128 192 256 384 512; do
    f="logs/std_bench/diskhnsw_${CM}mb.log"
    if [ -f "$f" ] && grep -q "QPS:" "$f"; then
        qps=$(grep "^QPS:" "$f" | awk '{print $2}')
        mean=$(grep "^Mean:" "$f" | awk '{print $2}')
        p99=$(grep "^P99:" "$f" | awk '{print $2}')
        hit=$(grep "^Hit%:" "$f" | awk '{print $2}')
        recall=$(grep "^Recall:" "$f" | awk '{print $2}')
        cg=$((CM + 120))
        printf "%-10s %-10s %-10s %-10s %-10s %-8s %-8s\n" "${CM}MB" "${cg}MB" "$qps" "$mean" "$p99" "$hit" "$recall"
    else
        printf "%-10s %-10s %s\n" "${CM}MB" "$((CM+120))MB" "OOM/FAILED"
    fi
done
