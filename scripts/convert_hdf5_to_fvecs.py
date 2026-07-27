#!/usr/bin/env python3
"""
convert_hdf5_to_fvecs.py - 把 ann-benchmarks HDF5 转成 fvecs 格式

用法:
  python3 convert_hdf5_to_fvecs.py input.hdf5 output_prefix [--train] [--test] [--gt]

输出:
  output_train.fvecs   - 训练向量 (base vectors)
  output_test.fvecs    - 查询向量
  output_gt.bin        - ground truth (ann-benchmarks 格式: distance + indices)
"""

import h5py
import numpy as np
import struct
import sys
import os
import time

def write_fvecs(path, data):
    """写 fvecs 格式: [dim:uint32] [v[0]:float32] ... [v[dim-1]:float32]"""
    n, d = data.shape
    print(f"Writing {n} vectors (dim={d}) to {path}...")
    t0 = time.time()
    with open(path, 'wb') as f:
        # 批量写: 构造 [dim, v[0], ..., v[d-1]] 的连续 buffer
        dim_arr = np.array([d], dtype=np.int32)
        for i in range(n):
            f.write(dim_arr.tobytes())
            f.write(data[i].astype(np.float32).tobytes())
    dt = time.time() - t0
    size_mb = os.path.getsize(path) / 1e6
    print(f"  Done: {size_mb:.1f} MB, {dt:.1f}s")
    return n, d

def write_ivecs_bin(path, neighbors, distances):
    """写 ground truth 二进制格式
    格式: [n:uint32] [k:uint32] [dist[0]:float32]...[dist[k-1]:float32] [idx[0]:uint64]...[idx[k-1]:uint64] × n
    与我们 benchmark_overlap.cpp 的读取格式一致
    """
    n, k = neighbors.shape
    print(f"Writing {n} ground truth (k={k}) to {path}...")
    with open(path, 'wb') as f:
        f.write(struct.pack('I', n))
        f.write(struct.pack('I', k))
        for i in range(n):
            # distances (float32)
            f.write(distances[i].astype(np.float32).tobytes())
            # neighbor indices (uint64)
            f.write(neighbors[i].astype(np.uint64).tobytes())
    size_mb = os.path.getsize(path) / 1e6
    print(f"  Done: {size_mb:.1f} MB")

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 convert_hdf5_to_fvecs.py input.hdf5 output_prefix")
        sys.exit(1)
    
    hdf5_path = sys.argv[1]
    prefix = sys.argv[2]
    
    print(f"Opening {hdf5_path}...")
    f = h5py.File(hdf5_path, 'r')
    
    print(f"Keys: {list(f.keys())}")
    
    # ann-benchmarks HDF5 格式:
    # 'train' - base vectors (N, D)
    # 'test' - query vectors (Q, D)
    # 'neighbors' - ground truth indices (Q, K)
    # 'distances' - ground truth distances (Q, K)
    
    # 转换训练集 (base vectors)
    if 'train' in f:
        train = f['train'][:]
        print(f"Train: {train.shape}, dtype={train.dtype}")
        # ann-benchmarks 可能用 int8/float32, 转成 float32
        if train.dtype != np.float32:
            # deep-image-96-angular 用 int8
            print(f"  Converting {train.dtype} to float32...")
            train = train.astype(np.float32)
        n, d = write_fvecs(f"{prefix}_train.fvecs", train)
        print(f"  Base vectors: {n} × {d}")
    
    # 转换查询集
    if 'test' in f:
        test = f['test'][:]
        print(f"Test: {test.shape}, dtype={test.dtype}")
        if test.dtype != np.float32:
            test = test.astype(np.float32)
        n, d = write_fvecs(f"{prefix}_test.fvecs", test)
        print(f"  Query vectors: {n} × {d}")
    
    # 转换 ground truth
    if 'neighbors' in f and 'distances' in f:
        neighbors = f['neighbors'][:]
        distances = f['distances'][:]
        print(f"Neighbors: {neighbors.shape}, Distances: {distances.shape}")
        write_ivecs_bin(f"{prefix}_gt.bin", neighbors, distances)
    
    f.close()
    print("\nDone!")

if __name__ == '__main__':
    main()
