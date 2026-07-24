#!/usr/bin/env python3
"""
构建 sub-HNSW 索引 for K-means 分区
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
            f.read(4)
            vecs[i] = struct.unpack(f'{dim}f', f.read(dim * 4))
        return vecs, dim

def main():
    part_dir = Path("output/partitions_kmeans200")
    index_dir = part_dir / "indexes"
    index_dir.mkdir(exist_ok=True)
    num_partitions = 200

    M = 16
    ef_construction = 200

    print("=" * 60)
    print("构建 sub-HNSW (K-means 分区)")
    print("=" * 60)

    dim = None
    t0 = time.time()
    skipped = 0

    for pid in range(num_partitions):
        fvecs_path = part_dir / f"partition_{pid:04d}.fvecs"
        vecs, dim = read_fvecs(fvecs_path)
        n = len(vecs)

        if n < 2:
            skipped += 1
            continue

        index = hnswlib.Index(space='l2', dim=dim)
        index.init_index(max_elements=n, ef_construction=ef_construction, M=M)
        index.add_items(vecs, np.arange(n))
        index.save_index(str(index_dir / f"partition_{pid:04d}.hnsw"))

        if pid < 3 or pid == num_partitions - 1:
            print(f"  Partition {pid}: {n} vectors")

    # 构建 meta-index (用质心)
    print(f"\n构建 meta-index (质心)...")
    centroids, _ = read_fvecs(part_dir / "centroids.fvecs")
    num_centroids = len(centroids)

    meta_index = hnswlib.Index(space='l2', dim=dim)
    meta_index.init_index(max_elements=num_centroids, ef_construction=200, M=16)
    meta_index.add_items(centroids, np.arange(num_centroids))
    meta_index.set_ef(64)
    meta_index.save_index(str(index_dir / "meta.hnsw"))

    # 保存 meta_partition_ids (每个质心对应一个分区)
    np.arange(num_centroids, dtype=np.uint32).tofile(
        index_dir / "meta_partition_ids.bin")

    print(f"  meta-index: {num_centroids} centroids -> {index_dir}/meta.hnsw")
    print(f"  sub-HNSW done: {time.time()-t0:.1f}s, skipped {skipped} tiny partitions")
    print("Done!")

if __name__ == "__main__":
    main()