#!/usr/bin/env python3
"""
K-means 聚类分区（替代 BFS 割边）

关键: 向量空间聚类，保证同一分区的向量在空间上邻近
"""
import numpy as np
import struct
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

def write_fvecs(path, vectors):
    with open(path, 'wb') as f:
        num, dim = vectors.shape
        for vec in vectors:
            f.write(struct.pack('i', dim))
            f.write(struct.pack(f'{dim}f', *vec))

def main():
    vectors_path = "data/test_1m.fvecs"
    output_dir = "output/partitions_kmeans200"
    num_partitions = 200

    print("=" * 60)
    print("K-means 聚类分区")
    print("=" * 60)

    print(f"\n1. Reading vectors...")
    vectors, dim = read_fvecs(vectors_path)
    num_nodes = len(vectors)
    print(f"   {num_nodes} vectors, dim={dim}")

    # K-means 聚类
    print(f"\n2. K-means clustering (K={num_partitions})...")
    t0 = time.time()

    try:
        import faiss
        print("   Using faiss K-means")
        kmeans = faiss.Kmeans(dim, num_partitions, niter=20, verbose=True, seed=42)
        kmeans.train(vectors)
        centroids = kmeans.centroids
        _, labels = kmeans.index.search(vectors, 1)
        labels = labels.flatten()
    except ImportError:
        print("   Using sklearn K-means")
        from sklearn.cluster import MiniBatchKMeans
        km = MiniBatchKMeans(n_clusters=num_partitions, random_state=42,
                             batch_size=10000, n_init=3, max_iter=100)
        labels = km.fit_predict(vectors)
        centroids = km.cluster_centers_

    print(f"   K-means done: {time.time()-t0:.1f}s")

    # 统计分区大小
    unique, counts = np.unique(labels, return_counts=True)
    print(f"   Partition sizes: min={counts.min()}, max={counts.max()}, "
          f"mean={counts.mean():.0f}, std={counts.std():.0f}")

    # 导出分区
    print(f"\n3. Exporting to {output_dir}...")
    output_path = Path(output_dir)
    output_path.mkdir(exist_ok=True)

    # 保存质心（用于路由）
    centroids = np.array(centroids, dtype=np.float32)
    write_fvecs(output_path / "centroids.fvecs", centroids)

    for pid in range(num_partitions):
        mask = labels == pid
        old_ids = np.where(mask)[0].astype(np.uint32)
        pvecs = vectors[old_ids]

        write_fvecs(output_path / f"partition_{pid:04d}.fvecs", pvecs)
        old_ids.tofile(output_path / f"partition_{pid:04d}.ids")

    # 全局映射
    labels.astype(np.uint32).tofile(output_path / "partition_mapping.bin")

    print(f"   Exported {num_partitions} partitions + centroids")
    print("\nDone!")

if __name__ == "__main__":
    main()