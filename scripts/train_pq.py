#!/usr/bin/env python3
"""
train_pq.py - 使用 faiss 训练 Product Quantization (PQ) 编码

作用（在 pipeline 中的位置）:
  DiskHNSW 的两阶段搜索里, Phase A 粗筛用 PQ ADC 近似距离在图上做 best-first
  搜索, 完全不读磁盘向量。本脚本把 base 向量压缩成 PQ codes (codebook + 每向量
  M 字节编码), 供 benchmark 通过 PQ_CODES_PATH 加载。

  ⚠️ PQ codes 按 base 向量的**原始顺序**编码, 与 graph/bfs 的 node id 一一对应,
     所以必须用与建图相同的 base 数据文件训练。

关键参数:
  M      = 子量化器数量。必须整除 dim。code 大小 = M 字节/向量。
           SIFT (dim=128): M=32 → dsub=4  (推荐, recall 95.7%)
           Deep (dim=96):  M=8  → dsub=12
  nbits  = 每个子量化器 8 bit → 256 centroids (固定)

用法:
  python3 scripts/train_pq.py <base.fvecs> <output.bin> [M]
  # 例:
  python3 scripts/train_pq.py data/sift_base.fvecs output/pqco_sift1m_M32_correct.bin 32

输出文件格式 (PQCO):
  magic 'PQCO' (4B)
  n (u64)  M (u32)  nbits (u32)  d (u32)
  M (u32)  ksub=256 (u32)  dsub (u32)
  codebook: M*256*dsub float32
  codes:    n*M uint8
"""

import numpy as np
import faiss
import sys
import os
import struct


def load_fvecs(path):
    """加载 fvecs 为 (n, d) float32 数组 (向量化读取, 快)。"""
    print(f"Loading {path} ...")
    with open(path, 'rb') as f:
        dim = struct.unpack('i', f.read(4))[0]
    # fvecs: 每条 = 4B dim + d*4B float。整体按 int32 读, 每行首列是 dim。
    raw = np.fromfile(path, dtype=np.int32)
    row = dim + 1
    assert raw.size % row == 0, f"文件大小与 dim={dim} 不匹配"
    n = raw.size // row
    data = raw.reshape(n, row)[:, 1:].view(np.float32)
    data = np.ascontiguousarray(data, dtype=np.float32)
    print(f"  Shape: {data.shape}")
    return data


def main():
    if len(sys.argv) < 3:
        print("Usage: python3 scripts/train_pq.py <base.fvecs> <output.bin> [M]")
        print("  M 缺省时自动选择: dim=128→32, dim=96→8, 其它→能整除 dim 的最大 (<=dim//4)")
        sys.exit(1)

    train_path = sys.argv[1]
    output_path = sys.argv[2]

    train_data = load_fvecs(train_path)
    n, d = train_data.shape

    # ---- 自动/手动选 M ----
    if len(sys.argv) >= 4:
        M = int(sys.argv[3])
    else:
        if d % 32 == 0 and d >= 128:
            M = 32
        elif d % 8 == 0:
            M = 8
        else:
            # 兜底: 找能整除 d 且 <= d//4 的最大值
            M = next((m for m in range(d // 4, 0, -1) if d % m == 0), 1)

    if d % M != 0:
        print(f"❌ 错误: M={M} 不能整除 dim={d}。请换一个整除 {d} 的 M "
              f"(候选: {[m for m in range(1, d+1) if d % m == 0 and m <= d//2]})")
        sys.exit(1)

    nbits = 8
    dsub = d // M
    print(f"\nPQ config: M={M}, nbits={nbits}, dsub={dsub}, ksub=256")
    print(f"PQ code size: {M} bytes/vector")
    print(f"Total PQ data: {n * M / 1e6:.1f} MB (原始 {n * d * 4 / 1e9:.2f} GB)")

    # ---- 训练 ----
    print("\nTraining PQ ...")
    pq = faiss.ProductQuantizer(d, M, nbits)
    pq.train(train_data)

    print("Encoding all vectors ...")
    codes = np.ascontiguousarray(pq.compute_codes(train_data))
    print(f"  codes shape: {codes.shape}, dtype: {codes.dtype}")

    # ---- 重建误差 (向量整体, 非 norm 差) ----
    ns = min(2000, n)
    idx = np.random.choice(n, ns, replace=False)
    recon = pq.decode(codes[idx])
    mse = np.mean(np.sum((train_data[idx] - recon) ** 2, axis=1))
    base_energy = np.mean(np.sum(train_data[idx] ** 2, axis=1))
    print(f"  重建 MSE: {mse:.2f}  (相对能量 {mse / base_energy * 100:.2f}%)")

    # ---- 写文件 (先写, 避免后续 crash 丢失) ----
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    print(f"\nSaving PQ codes to {output_path} ...")
    codebook = faiss.vector_to_array(pq.centroids).reshape(M, 256, dsub)
    with open(output_path, 'wb') as f:
        f.write(b'PQCO')
        f.write(struct.pack('Q', n))
        f.write(struct.pack('I', M))
        f.write(struct.pack('I', nbits))
        f.write(struct.pack('I', d))
        f.write(struct.pack('III', M, 256, dsub))
        f.write(codebook.astype(np.float32).tobytes())
        f.write(codes.tobytes())
    print(f"  File size: {os.path.getsize(output_path) / 1e6:.1f} MB")

    # ---- ADC 精度自检 (用 base 前若干条当 query, 对同一 base 搜索) ----
    print("\nADC 精度自检 ...")
    n_test = min(1000, n)
    index_pq = faiss.IndexPQ(d, M, nbits)
    index_pq.pq = pq
    index_pq.is_trained = True
    index_pq.add(train_data)                       # add 原始向量, faiss 内部会编码
    queries = train_data[:n_test]
    _, I_adc = index_pq.search(queries, 10)

    # 真实 top-10 (query 自身在 base 里, 会包含自身, 公平对比即可)
    overlap = 0.0
    for i in range(n_test):
        diff = train_data - queries[i]
        true_top = np.argpartition(np.sum(diff * diff, axis=1), 10)[:10]
        overlap += len(set(true_top.tolist()) & set(I_adc[i].tolist())) / 10
    print(f"  ADC top-10 overlap: {overlap / n_test * 100:.1f}%")

    print("\n✅ PQ 训练完成")
    print(f"  M={M} nbits={nbits} code={M}B/向量  压缩 {d * 4}B → {M}B ({d * 4 / M:.0f}x)")


if __name__ == "__main__":
    main()
