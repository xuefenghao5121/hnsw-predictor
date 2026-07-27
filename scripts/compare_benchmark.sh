#!/bin/bash
# ==============================================================================
# compare_benchmark.sh —— DiskHNSW vs hnswlib 正式对比测试脚本 (唯一维护版本)
# ==============================================================================
#
# 目的:
#   在同一台机器、同一份数据上, 直观对比:
#     [A] DiskHNSW  : 512MB cgroup 内存限制 + 多线程 (磁盘向量搜索, 省内存)
#     [B] hnswlib   : 放开 cgroup 限制 (全内存基线, 性能天花板)
#
# 保证公平对比的三要素:
#   1. 充分 warmup —— CPU 升频 + page cache 预热 + 搜索预跑, 排除冷启动噪声
#   2. 相同 query / GT / k / ef —— 两者用完全一样的输入
#   3. DiskHNSW 在 cgroup 内跑, hnswlib 放开限制 —— 体现"省内存"的代价与收益
#
# 用法:
#   bash scripts/compare_benchmark.sh                    # 默认参数
#   THREADS=8 bash scripts/compare_benchmark.sh          # 改线程数
#   MEM=512M K=10 EF=50 NQ=200 bash scripts/compare_benchmark.sh
#
# 依赖: make bench 已编译; output/ 下有配套的 graph/blocks/route/vecblocks/PQ/index
# ==============================================================================

set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# ---------------- 可调参数 (环境变量覆盖) ----------------
MEM=${MEM:-512M}          # DiskHNSW cgroup 内存上限
THREADS=${THREADS:-4}     # DiskHNSW 并发线程数
CPUQUOTA=${CPUQUOTA:-$((THREADS * 100))%}  # cgroup CPU 配额 (每线程 100%)
K=${K:-10}                # top-K
EF=${EF:-50}              # 搜索候选队列大小
NQ=${NQ:-200}             # query 数量
RUNS=${RUNS:-3}           # 每个 benchmark 跑几轮取最优 QPS (排除调频抖动)

# ---------------- 文件路径 (配套, 必须同批生成) ----------------
GRAPH=output/sift1m_graph.bin
BFS=output/sift1m_bfs.bin
BLOCKS=output/sift1m_blocks_64k.bin
ROUTE=output/sift1m_route_64k.bin
VECBLOCKS=output/sift1m_vecblocks_64k.bin
PQ=output/pqco_sift1m_M32_correct.bin
INDEX=output/sift1m_index.bin        # hnswlib 原生 index
BASE=data/sift_base.fvecs
QUERY=data/sift1m_query200.fvecs
GT=data/sift1m_gt200.bin

# DiskHNSW 单线程用 FINE_BUFFERED (page cache 命中最快),
# 多线程用 FINE_PREAD (io_uring 非线程安全, 多线程必须绕过).
if [ "$THREADS" -gt 1 ]; then
    FINE_MODE="FINE_PREAD=1"
    MODE_DESC="FINE_PREAD (多线程)"
else
    FINE_MODE="FINE_BUFFERED=1"
    MODE_DESC="FINE_BUFFERED (单线程)"
fi

# ---------------- 前置检查 ----------------
for f in "$GRAPH" "$BFS" "$BLOCKS" "$ROUTE" "$VECBLOCKS" "$PQ" "$INDEX" "$BASE" "$QUERY" "$GT"; do
    if [ ! -e "$f" ]; then
        echo "❌ 缺文件: $f"
        echo "   请先跑 scripts/build_pipeline.sh 生成配套文件, 或 symlink 已有资产。"
        exit 1
    fi
done
[ -x build/benchmark_diskhnsw ] || { echo "❌ 缺 build/benchmark_diskhnsw, 请 make bench"; exit 1; }
[ -x build/benchmark_hnswlib_native ] || { echo "❌ 缺 build/benchmark_hnswlib_native, 请 make bench"; exit 1; }

echo "=============================================================="
echo " DiskHNSW vs hnswlib 对比测试"
echo " 数据: SIFT1M (100万 x 128维) | query=$NQ | k=$K | ef=$EF"
echo " DiskHNSW: cgroup MemMax=$MEM CPUQuota=$CPUQUOTA threads=$THREADS | $MODE_DESC"
echo " hnswlib : 放开 cgroup (全内存基线)"
echo "=============================================================="

# ---------------- Warmup: 预热 page cache ----------------
echo ""
echo ">>> [Warmup] 预热 page cache (把 DiskHNSW 要读的文件加载进内存)..."
cat "$GRAPH" "$BFS" "$BLOCKS" "$ROUTE" "$VECBLOCKS" "$PQ" > /dev/null 2>&1
cat "$INDEX" "$BASE" > /dev/null 2>&1
echo "    page cache 预热完成。"
# 注: benchmark 内部还会做 CPU 升频 spin + 全 query 预跑一遍(第二个warmup层)

# best_qps <log>  —— 从多轮日志里取最高 QPS (调频/后台抢占会让某些轮偏慢, 峰值最能代表真实上限)
best_of() {
    grep -h "QPS:" "$1" | grep -oE "[0-9]+\.[0-9]+" | sort -rn | head -1
}

# ==============================================================
# [A] DiskHNSW —— 512MB cgroup 内存限制 + 多线程
# ==============================================================
echo ""
echo "=============================================================="
echo " [A] DiskHNSW  (cgroup $MEM, $THREADS 线程) —— $RUNS 轮取峰值"
echo "=============================================================="
A_LOG=$(mktemp)
for r in $(seq 1 "$RUNS"); do
    systemd-run --user --scope --quiet \
        -p MemoryMax="$MEM" -p MemorySwapMax=0 -p CPUQuota="$CPUQUOTA" \
        env TWO_STAGE=1 PQ_HYBRID=1 FINE_RERANK=1 $FINE_MODE \
            NUM_THREADS="$THREADS" REFINE_EF=100 CACHE_MB=32 FLAT_VEC_MB=64 \
            PQ_CODES_PATH="$PQ" VEC_BLOCKS_PATH="$VECBLOCKS" \
        ./build/benchmark_diskhnsw \
            "$GRAPH" "$BFS" "$BLOCKS" "$ROUTE" \
            "$BASE" "$QUERY" "$GT" "$K" "$EF" "$NQ" \
        2>/dev/null | tee -a "$A_LOG" | grep -E "Recall|QPS|RSS" | sed "s/^/  [run $r] /"
done
A_QPS=$(best_of "$A_LOG")
A_RECALL=$(grep -h "Recall:" "$A_LOG" | head -1 | grep -oE "[0-9]+\.[0-9]+")
A_RSS=$(grep -h "^RSS:" "$A_LOG" | grep -oE "[0-9]+" | sort -rn | head -1)
rm -f "$A_LOG"

# ==============================================================
# [B] hnswlib 全内存基线 —— 放开 cgroup 限制
# ==============================================================
echo ""
echo "=============================================================="
echo " [B] hnswlib native  (全内存, 无 cgroup 限制) —— $RUNS 轮取峰值"
echo "=============================================================="
B_LOG=$(mktemp)
for r in $(seq 1 "$RUNS"); do
    ./build/benchmark_hnswlib_native \
        "$INDEX" "$QUERY" "$GT" "$K" "$EF" "$NQ" \
        2>/dev/null | tee -a "$B_LOG" | grep -E "Recall|QPS|RSS" | sed "s/^/  [run $r] /"
done
B_QPS=$(best_of "$B_LOG")
B_RECALL=$(grep -h "Recall:" "$B_LOG" | head -1 | grep -oE "[0-9]+\.[0-9]+")
B_RSS=$(grep -h "^RSS:" "$B_LOG" | grep -oE "[0-9]+" | sort -rn | head -1)
rm -f "$B_LOG"

# ==============================================================
# 汇总对比表
# ==============================================================
echo ""
echo "=============================================================="
echo " 对比汇总 ($RUNS 轮峰值 QPS)"
echo "=============================================================="
printf "  %-22s | %-10s | %-8s | %-8s\n" "方案" "Recall%" "RSS(MB)" "QPS"
printf "  %-22s-+-%-10s-+-%-8s-+-%-8s\n" "----------------------" "----------" "--------" "--------"
printf "  %-22s | %-10s | %-8s | %-8s\n" "DiskHNSW ($MEM/$THREADS线程)" "${A_RECALL:-?}" "${A_RSS:-?}" "${A_QPS:-?}"
printf "  %-22s | %-10s | %-8s | %-8s\n" "hnswlib (全内存)" "${B_RECALL:-?}" "${B_RSS:-?}" "${B_QPS:-?}"
echo ""
if [ -n "$A_RSS" ] && [ -n "$B_RSS" ]; then
    echo "  内存节省: DiskHNSW $A_RSS MB vs hnswlib $B_RSS MB"
fi
echo "  说明: DiskHNSW 用 cgroup $MEM 限制做磁盘搜索(省内存); hnswlib 全量常驻内存(QPS 天花板)。"
echo "       多线程模式下 P50/P95/P99 为 total/n 近似值, 不代表真实分布, 以 QPS 为准。"
echo "=============================================================="
