#!/usr/bin/env python3
"""
train_pq.py - 使用 faiss 训练 PQ 编码，将 96dim 向量压缩为 PQ codes
输出:
  - pq_model.pkl: PQ 模型 (codebook)
  - pq_codes.bin: 所有训练向量的 PQ 编码 (连续存储)
  - pq_codes_dim: PQ code 维度 (每个向量的编码字节数)
"""

import numpy as np
import faiss
import sys
import os
import struct

def read_fvecs(path):
    """读取 fvecs 文件"""
    with open(path, 'rb') as f:
        while True:
            dim_bytes = f.read(4)
            if not dim_bytes:
                break
            dim = struct.unpack('i', dim_bytes)[0]
            vec = np.frombuffer(f.read(4 * dim), dtype=np.float32)
            yield vec

def load_fvecs(path):
    """加载全部 fvecs 数据为 numpy 数组"""
    print(f"Loading {path}...")
    data = list(read_fvecs(path))
    arr = np.array(data, dtype=np.float32)
    print(f"  Shape: {arr.shape}")
    return arr

def main():
    train_path = "data/deep10m_train.fvecs"
    output_dir = "output"
    
    os.makedirs(output_dir, exist_ok=True)
    
    # 加载训练数据
    train_data = load_fvecs(train_path)
    n, d = train_data.shape
    print(f"Training data: {n} vectors, {d} dims")
    
    # PQ 参数
    # 96 dim → M=8 subquantizers × dsub=12 → 每个子码本 256 centroids
    # PQ code: 8 bytes per vector (8 × 1 byte per subquantizer)
    M = 8          # 子量化器数量
    nbits = 8      # 每个子量化器的 bits (256 centroids)
    dsub = d // M  # 96 / 8 = 12
    
    print(f"\nPQ config: M={M}, nbits={nbits}, dsub={dsub}")
    print(f"PQ code size: {M} bytes per vector")
    print(f"Total PQ data: {n * M / 1e6:.1f} MB (vs {n * d * 4 / 1e9:.1f} GB original)")
    
    # 训练 PQ
    print("\nTraining PQ...")
    pq = faiss.ProductQuantizer(d, M, nbits)
    pq.train(train_data)
    
    # 编码所有向量
    print("Encoding all vectors...")
    codes = pq.compute_codes(train_data)
    print(f"Codes shape: {codes.shape}, dtype: {codes.dtype}")
    
    # 验证重建误差
    print("\nValidating reconstruction error...")
    sample_idx = np.random.choice(n, 1000, replace=False)
    sample_data = train_data[sample_idx]
    sample_codes = codes[sample_idx]
    reconstructed = pq.decode(sample_codes)
    
    # 计算距离误差
    orig_dist = np.linalg.norm(sample_data[:100], axis=1)
    recon_dist = np.linalg.norm(reconstructed[:100], axis=1)
    rel_error = np.mean(np.abs(orig_dist - recon_dist) / orig_dist)
    print(f"  Mean relative distance error: {rel_error:.4f} ({rel_error*100:.2f}%)")
    
    # 保存 PQ codes (在测试之前保存, 避免测试 crash 导致丢失)
    pq_codes_path = os.path.join(output_dir, "deep10m_pq_codes.bin")
    print(f"\nSaving PQ codes to {pq_codes_path}...")
    codes_contiguous = np.ascontiguousarray(codes)
    with open(pq_codes_path, 'wb') as f:
        f.write(b'PQCO')
        f.write(struct.pack('Q', n))
        f.write(struct.pack('I', M))
        f.write(struct.pack('I', nbits))
        f.write(struct.pack('I', d))
        codebook = faiss.vector_to_array(pq.centroids).reshape(M, 256, dsub)
        f.write(struct.pack('III', M, 256, dsub))  # 3 uint32s
        f.write(codebook.astype(np.float32).tobytes())
        f.write(codes_contiguous.tobytes())
    
    file_size = os.path.getsize(pq_codes_path)
    print(f"  File size: {file_size/1e6:.1f} MB")
    
    # 测试 ADC (Asymmetric Distance Computation) 精度
    print("\nTesting ADC distance accuracy...")
    n_test = 1000
    test_queries = train_data[:n_test]
    
    # 用 faiss IndexPQ 做 ADC 搜索
    index_pq = faiss.IndexPQ(d, M, nbits)
    index_pq.pq = pq
    index_pq.is_trained = True
    index_pq.add(codes[:n])  # 添加所有 PQ codes
    
    # 对前 1000 个 query 搜索
    true_dists = np.zeros((n_test, 100))
    for i in range(n_test):
        diff = train_data[i:i+100] - test_queries[i]
        true_dists[i] = np.sum(diff ** 2, axis=1)
    
    # ADC 搜索
    D_adc, I_adc = index_pq.search(test_queries, 10)
    
    # 真实 top-10
    true_overlap = 0
    for i in range(n_test):
        true_order = np.argsort(true_dists[i])[:10]
        adc_order = I_adc[i][:10]
        overlap = len(set(true_order.tolist()) & set(adc_order.tolist()))
        true_overlap += overlap / 10
    
    print(f"  ADC top-10 overlap with true distance: {true_overlap/n_test*100:.1f}%")
    
    print(f"\n✅ PQ training complete!")
    print(f"  M={M}, nbits={nbits}, code_size={M} bytes/vector")
    print(f"  Compression: {d*4}B → {M}B ({d*4/M:.0f}x)")
    
    return M, nbits, dsub

if __name__ == "__main__":
    os.chdir("/home/huawei/hnsw-predictor")
    main()
