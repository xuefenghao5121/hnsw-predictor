#!/usr/bin/env python3
"""
train_opq.py - 训练 OPQ 旋转矩阵 + PQ 编码

OPQ (Optimized Product Quantization) 通过学习正交旋转矩阵 R，
使旋转后的向量 R*x 在 PQ 量化时子空间更均衡，从而降低量化误差、提升粗筛精度。

输出文件格式 (兼容原有 PQCO 格式 + 追加旋转矩阵):
  magic[4B] = 'OPQ1'
  n[8B], M[4B], nbits[4B], d[4B]
  codebook_header[12B] = (M, ksub, dsub)
  codebook[M*ksub*dsub*4B float32]
  rotation[d*d*4B float32]    ← 新增
  codes[n*M bytes]
"""

import numpy as np
import faiss
import struct
import sys
import os

def read_fvecs(path):
    with open(path, 'rb') as f:
        while True:
            dim_bytes = f.read(4)
            if not dim_bytes:
                break
            dim = struct.unpack('i', dim_bytes)[0]
            vec = np.frombuffer(f.read(4 * dim), dtype=np.float32)
            yield vec

def load_fvecs(path):
    print(f"Loading {path}...")
    data = list(read_fvecs(path))
    arr = np.array(data, dtype=np.float32)
    print(f"  Shape: {arr.shape}")
    return arr

def main():
    data_path = "data/sift1m_train.fvecs"
    output_dir = "output"
    os.makedirs(output_dir, exist_ok=True)

    # PQ 参数 (与现有配置一致)
    M = 32
    nbits = 8
    d = 128

    # 加载数据
    train_data = load_fvecs(data_path)
    n, d_actual = train_data.shape
    assert d_actual == d, f"dim mismatch: {d_actual} vs {d}"
    print(f"Training data: {n} vectors, {d} dims")

    # ---- Step 1: 训练 OPQ 旋转矩阵 ----
    print(f"\n--- Training OPQ rotation (M={M}, d={d}) ---")
    opq = faiss.OPQMatrix(d, M)
    opq.niter = 50  # OPQ 迭代次数
    opq.verbose = True
    opq.train(train_data)

    # 提取旋转矩阵 R (d x d)
    R = faiss.vector_to_array(opq.A).reshape(d, d)
    print(f"Rotation matrix shape: {R.shape}")
    print(f"R orthogonality check: ||R*R^T - I||_F = {np.linalg.norm(R @ R.T - np.eye(d)):.6f}")

    # ---- Step 2: 旋转训练数据 ----
    print("\n--- Rotating training data ---")
    rotated_data = train_data @ R.T  # x' = R * x
    print(f"Rotated data shape: {rotated_data.shape}")

    # ---- Step 3: 在旋转后的数据上训练 PQ ----
    print(f"\n--- Training PQ on rotated data (M={M}, nbits={nbits}) ---")
    pq = faiss.ProductQuantizer(d, M, nbits)
    pq.train(rotated_data)

    # ---- Step 4: 编码所有旋转后的向量 ----
    print("Encoding all rotated vectors...")
    codes = pq.compute_codes(rotated_data)
    print(f"Codes shape: {codes.shape}, dtype: {codes.dtype}")

    # ---- Step 5: 验证重建误差对比 ----
    print("\n--- Reconstruction error comparison ---")
    sample_idx = np.random.choice(n, 2000, replace=False)
    sample_orig = train_data[sample_idx]
    sample_rot = rotated_data[sample_idx]
    sample_codes = codes[sample_idx]
    reconstructed_rot = pq.decode(sample_codes)

    # OPQ 重建误差 (在旋转空间)
    rot_err = np.mean(np.sum((sample_rot - reconstructed_rot) ** 2, axis=1))
    rot_norm = np.mean(np.sum(sample_rot ** 2, axis=1))
    print(f"  OPQ relative recon error: {rot_err / rot_norm:.6f} ({rot_err/rot_norm*100:.4f}%)")

    # 也训练一个无 OPQ 的 PQ 做对比
    print("\n--- Training baseline PQ (no OPQ) for comparison ---")
    pq_base = faiss.ProductQuantizer(d, M, nbits)
    pq_base.train(train_data)
    codes_base = pq_base.compute_codes(train_data)
    recon_base = pq_base.decode(sample_codes)  # wrong codes, re-encode
    codes_base_sample = pq_base.compute_codes(sample_orig)
    recon_base_sample = pq_base.decode(codes_base_sample)
    base_err = np.mean(np.sum((sample_orig - recon_base_sample) ** 2, axis=1))
    base_norm = np.mean(np.sum(sample_orig ** 2, axis=1))
    print(f"  Baseline PQ relative recon error: {base_err / base_norm:.6f} ({base_err/base_norm*100:.4f}%)")
    print(f"  Improvement: {(1 - rot_err/base_err)*100:.1f}%")

    # ---- Step 6: 保存 OPQ + PQ 模型 (先保存，避免后续 crash 丢失) ----
    output_path = os.path.join(output_dir, "pqco_sift1m_opq.bin")
    print(f"\n--- Saving OPQ model to {output_path} ---")
    codebook = faiss.vector_to_array(pq.centroids).reshape(M, 256, d // M)
    codes_contiguous = np.ascontiguousarray(codes)
    R_contiguous = np.ascontiguousarray(R, dtype=np.float32)

    with open(output_path, 'wb') as f:
        f.write(b'OPQ1')                                          # magic
        f.write(struct.pack('Q', n))                               # n vectors
        f.write(struct.pack('I', M))                               # M
        f.write(struct.pack('I', nbits))                           # nbits
        f.write(struct.pack('I', d))                               # dim
        f.write(struct.pack('III', M, 256, d // M))                # codebook header
        f.write(codebook.astype(np.float32).tobytes())             # codebook
        f.write(R_contiguous.tobytes())                            # rotation matrix [d*d]
        f.write(codes_contiguous.tobytes())                        # PQ codes

    file_size = os.path.getsize(output_path)
    expected = 4 + 8 + 4*3 + 12 + M*256*(d//M)*4 + d*d*4 + n*M
    print(f"  File size: {file_size/1e6:.1f} MB (expected {expected/1e6:.1f} MB)")
    print(f"\n✅ OPQ training complete!")
    print(f"  M={M}, nbits={nbits}, dsub={d//M}, rotation={d}x{d}")
    print(f"  ⚠️  OPQ recon error {rot_err/rot_norm*100:.2f}% vs baseline {base_err/base_norm*100:.2f}%")

if __name__ == "__main__":
    os.chdir("/home/huawei/hnsw-predictor")
    main()
