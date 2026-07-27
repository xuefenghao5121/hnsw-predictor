#!/usr/bin/env python3
"""
gen_gt.py - 生成 Ground Truth (精确 top-K 最近邻)

作用（在 pipeline 中的位置）:
  benchmark 用 GT 文件计算 recall。GT 是对每个 query, 在 base 向量里用暴力精确
  L2 距离算出的真实 top-K 邻居 id。search 返回的 id 与 GT 的交集比例 = recall。

  ⚠️ GT 里的 id 是 base 向量的**原始行号** (0..n-1), 与 graph/PQ 的 node id 同一
     空间。所以 base / query 必须与建图、训练 PQ 用的是同一套数据。

用法:
  python3 scripts/gen_gt.py <base.fvecs> <query.fvecs> <gt_out.bin> [K]
  # 例 (SIFT1M, K=10):
  python3 scripts/gen_gt.py data/sift_base.fvecs data/sift1m_query200.fvecs data/sift1m_gt200.bin 10

输出格式 (与 benchmark read_gt 对齐):
  header 8B: n_queries(u32) + K(u32)
  然后 n_queries * K 个 uint64 邻居 id (仅 id, 无距离)

  ⚠️ benchmark 运行时的 K 必须 <= 这里的 K, 且**最好完全相等**。read_gt 按运行时 K
     逐行读取, 若运行 K < 生成 K 会读串行 (recall 全错)。建议生成 K 与测试 K 一致。
"""

import numpy as np
import faiss
import sys
import os
import struct


def load_fvecs(path):
    with open(path, 'rb') as f:
        dim = struct.unpack('i', f.read(4))[0]
    raw = np.fromfile(path, dtype=np.int32)
    row = dim + 1
    assert raw.size % row == 0, f"{path} 大小与 dim={dim} 不符"
    n = raw.size // row
    data = raw.reshape(n, row)[:, 1:].view(np.float32)
    return np.ascontiguousarray(data, dtype=np.float32), dim


def main():
    if len(sys.argv) < 4:
        print("Usage: python3 scripts/gen_gt.py <base.fvecs> <query.fvecs> <gt_out.bin> [K]")
        sys.exit(1)

    base_path = sys.argv[1]
    query_path = sys.argv[2]
    gt_path = sys.argv[3]
    K = int(sys.argv[4]) if len(sys.argv) >= 5 else 10

    print(f"Loading base {base_path} ...")
    base, d = load_fvecs(base_path)
    print(f"  base: {base.shape}")
    print(f"Loading query {query_path} ...")
    query, dq = load_fvecs(query_path)
    print(f"  query: {query.shape}")
    assert d == dq, f"维度不匹配: base={d} query={dq}"

    print(f"\n暴力精确 L2 搜索 top-{K} (faiss IndexFlatL2)...")
    index = faiss.IndexFlatL2(d)
    index.add(base)
    _, I = index.search(query, K)          # I: (n_query, K) int64, 原始行号

    nq = query.shape[0]
    os.makedirs(os.path.dirname(gt_path) or ".", exist_ok=True)
    with open(gt_path, 'wb') as f:
        f.write(struct.pack('I', nq))
        f.write(struct.pack('I', K))
        f.write(I.astype(np.uint64).tobytes())

    print(f"\n✅ GT 写入 {gt_path}")
    print(f"  n_queries={nq}, K={K}, file={os.path.getsize(gt_path)} bytes")
    print(f"  样例 query0 top-{K}: {I[0].tolist()}")


if __name__ == "__main__":
    main()
