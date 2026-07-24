#!/usr/bin/env python3
"""
构建 sub-HNSW 分区索引 + meta-index

Phase 1.2 + 1.3
"""
import numpy as np
import struct
import hnswlib
import time
from pathlib import Path

def read_fvecs(path):
    with open(path, 'rb') as f:
        dim = struct.unpack('i', f.read(4))[0]
        f.seek(0, 2)
        file_size = f.tell()
        f.seek(0, 0)
        vec_size = 4 + dim * 4
        num = file_size // vec_size
        vecs = np.zeros((num, dim), dtype=np.float32)
        for i in range(num):
            f.read(4)  # skip dim
            vecs[i] = struct.unpack(f'{dim}f', f.read(dim * 4))
        return vecs, dim

def main():
    part_dir = Path("output/partitions_bfs200")
    index_dir = Path("output/partitions_bfs200/indexes")
    index_dir.mkdir(exist_ok=True)
    num_partitions = 200

    # HNSW 参数
    M = 16
    ef_construction = 200

    print("=" * 60)
    print("构建 sub-HNSW 分区索引")
    print("=" * 60)

    dim = None
    t0 = time.time()

    for pid in range(num_partitions):
        fvecs_path = part_dir / f"partition_{pid:04d}.fvecs"
        vecs, dim = read_fvecs(fvecs_path)
        n = len(vecs)

        # 构建 sub-HNSW (label = local_id)
        index = hnswlib.Index(space='l2', dim=dim)
        index.init_index(max_elements=n, ef_construction=ef_construction, M=M)
        index.add_items(vecs, np.arange(n))
        index.set_ef(50)

        # 保存
        index_path = index_dir / f"partition_{pid:04d}.hnsw"
        index.save_index(str(index_path))

        if pid < 3 or pid == num_partitions - 1:
            print(f"  Partition {pid}: {n} vectors -> {index_path}")

    print(f"\nsub-HNSW 构建完成: {num_partitions} 分区, 耗时 {time.time()-t0:.1f}s")

    # ---- 构建 meta-index ----
    print("\n" + "=" * 60)
    print("构建 meta-index (采样点)")
    print("=" * 60)

    # 从每个分区采样一个代表点（质心最近点），作为该分区的路由入口
    # 简化：每个分区采样 num_samples_per_partition 个点
    num_samples_per_partition = 3

    meta_vectors = []
    meta_partition_ids = []

    for pid in range(num_partitions):
        fvecs_path = part_dir / f"partition_{pid:04d}.fvecs"
        vecs, dim = read_fvecs(fvecs_path)

        # 计算质心
        centroid = vecs.mean(axis=0)
        # 找最接近质心的 num_samples 个点
        dists = np.linalg.norm(vecs - centroid, axis=1)
        nearest_idx = np.argsort(dists)[:num_samples_per_partition]

        for idx in nearest_idx:
            meta_vectors.append(vecs[idx])
            meta_partition_ids.append(pid)

    meta_vectors = np.array(meta_vectors, dtype=np.float32)
    meta_partition_ids = np.array(meta_partition_ids, dtype=np.uint32)
    num_meta = len(meta_vectors)

    print(f"采样点数: {num_meta} ({num_samples_per_partition}/分区)")

    # 构建 meta-HNSW (label = meta_sample_index)
    meta_index = hnswlib.Index(space='l2', dim=dim)
    meta_index.init_index(max_elements=num_meta, ef_construction=200, M=16)
    meta_index.add_items(meta_vectors, np.arange(num_meta))
    meta_index.set_ef(64)

    meta_index.save_index(str(index_dir / "meta.hnsw"))
    meta_partition_ids.tofile(index_dir / "meta_partition_ids.bin")

    print(f"meta-index 保存到 {index_dir}/meta.hnsw")
    print(f"meta_partition_ids 保存到 {index_dir}/meta_partition_ids.bin")

    print("\n" + "=" * 60)
    print("Done!")
    print("=" * 60)

if __name__ == "__main__":
    main()