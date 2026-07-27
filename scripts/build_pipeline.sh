#!/bin/bash
# build_pipeline.sh - 一键跑完整数据准备 pipeline (Step 1-7)
#
# 保证 graph / blocks / route / vecblocks / PQ 全部同批生成、互相配套,
# 避免手动分步时因文件混用/M 用错/GT K 不一致导致的 recall 崩溃。
#
# 用法:
#   bash scripts/build_pipeline.sh <base.fvecs> <前缀> <M> [query.fvecs] [K]
#
# 例:
#   bash scripts/build_pipeline.sh data/sift_base.fvecs sift1m 32 \
#        data/sift1m_query200.fvecs 10
#
# 产出 (output/ 下):
#   <前缀>_index.bin  <前缀>_graph.bin  <前缀>_bfs.bin
#   <前缀>_blocks_64k.bin  <前缀>_route_64k.bin
#   <前缀>_vecblocks_64k.bin  <前缀>_vecblocks_64k_route.bin
#   pqco_<前缀>_M<M>.bin
#   (若给了 query) data 同目录下 <前缀>_gtK.bin

set -e
cd "$(dirname "$0")/.."

BASE=${1:?"缺 base.fvecs"}
PREFIX=${2:?"缺前缀 (如 sift1m)"}
M=${3:?"缺 M (SIFT=32, Deep=8)"}
QUERY=${4:-}
K=${5:-10}

BS=65536   # 64KB block
O=output
mkdir -p "$O"

# 读维度 (fvecs 首 4 字节)
DIM=$(python3 -c "import struct;print(struct.unpack('i',open('$BASE','rb').read(4))[0])")
echo "=========================================="
echo " base=$BASE  dim=$DIM  M=$M  prefix=$PREFIX"
echo "=========================================="
if [ $((DIM % M)) -ne 0 ]; then
    echo "❌ M=$M 不能整除 dim=$DIM，请换 M"; exit 1
fi

echo "[1/7] build_index ..."
./build/build_index "$BASE" "$O/${PREFIX}_index.bin" 16 200

echo "[2/7] extract_graph ..."
./build/extract_graph "$O/${PREFIX}_index.bin" "$O/${PREFIX}_graph.bin" "$DIM"

echo "[3/7] bfs_reorder ..."
./build/bfs_reorder "$O/${PREFIX}_graph.bin" "$O/${PREFIX}_bfs.bin"

echo "[4/7] write_blocks_veconly ..."
./build/write_blocks_veconly "$O/${PREFIX}_graph.bin" "$O/${PREFIX}_bfs.bin" \
    "$O/${PREFIX}_vecblocks_64k.bin" $BS

echo "[5/7] write_blocks + gen_route ..."
./build/write_blocks "$O/${PREFIX}_graph.bin" "$O/${PREFIX}_bfs.bin" \
    "$O/${PREFIX}_blocks_64k.bin" $BS
./build/gen_route "$O/${PREFIX}_blocks_64k.bin" "$O/${PREFIX}_route_64k.bin"

echo "[6/7] train_pq (M=$M) ..."
python3 scripts/train_pq.py "$BASE" "$O/pqco_${PREFIX}_M${M}.bin" "$M"

if [ -n "$QUERY" ]; then
    echo "[7/7] gen_gt (K=$K) ..."
    python3 scripts/gen_gt.py "$BASE" "$QUERY" "data/${PREFIX}_gt${K}.bin" "$K"
else
    echo "[7/7] 跳过 GT (未提供 query.fvecs)"
fi

echo ""
echo "✅ pipeline 完成。全套配套文件已生成:"
ls -la "$O/${PREFIX}"_*.bin "$O/pqco_${PREFIX}_M${M}.bin" 2>/dev/null
