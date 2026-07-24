#!/usr/bin/env python3
"""
C 方案端到端原型（优化版）
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

def read_gt_bin(path):
    data = np.fromfile(path, dtype=np.int32)
    num_queries = data[0]
    k = data[1]
    labels = np.zeros((num_queries, k), dtype=np.int32)
    for i in range(num_queries):
        for j in range(k):
            labels[i, j] = data[2 + i * (k * 2) + j * 2]
    return labels

class PartitionedHNSW:
    def __init__(self, index_dir, base_dir, num_partitions=200, dim=128, max_cache=10):
        self.index_dir = Path(index_dir)
        self.base_dir = Path(base_dir)
        self.num_partitions = num_partitions
        self.dim = dim
        self.max_cache = max_cache

        # 加载 meta-index
        t0 = time.time()
        self.meta_index = hnswlib.Index(space='l2', dim=dim)
        self.meta_index.load_index(str(self.index_dir / "meta.hnsw"))
        self.meta_index.set_ef(64)
        self.meta_partition_ids = np.fromfile(
            self.index_dir / "meta_partition_ids.bin", dtype=np.uint32
        )

        # 预加载所有分区的 old_id 映射到内存
        self.partition_old_ids = []
        for pid in range(num_partitions):
            ids_path = self.base_dir / f"partition_{pid:04d}.ids"
            old_ids = np.fromfile(ids_path, dtype=np.uint32)
            self.partition_old_ids.append(old_ids)

        # 分区缓存
        self.partition_cache = {}
        self.cache_order = []
        print(f"Init: {time.time()-t0:.1f}s, {num_partitions} partitions, {self.num_meta} meta")

    @property
    def num_meta(self):
        return len(self.meta_partition_ids)

    def _load_partition(self, pid):
        if pid in self.partition_cache:
            self.cache_order.remove(pid)
            self.cache_order.append(pid)
            return self.partition_cache[pid]

        index_path = self.index_dir / f"partition_{pid:04d}.hnsw"
        if not index_path.exists():
            return None  # 跳过的小分区

        if len(self.partition_cache) >= self.max_cache:
            evict = self.cache_order.pop(0)
            del self.partition_cache[evict]

        index = hnswlib.Index(space='l2', dim=self.dim)
        index.load_index(str(index_path))
        self.partition_cache[pid] = index
        self.cache_order.append(pid)
        return index

    def route(self, query, k_routes=2):
        labels, _ = self.meta_index.knn_query(query.reshape(1, -1), k=k_routes)
        partitions = set()
        for idx in labels[0]:
            pid = self.meta_partition_ids[idx]
            partitions.add(pid)
        return list(partitions)

    def search(self, query, k=10, k_routes=2, ef=50):
        partitions = self.route(query, k_routes)
        candidates = []

        for pid in partitions:
            index = self._load_partition(pid)
            if index is None:
                continue
            index.set_ef(ef)
            n_items = index.get_current_count()
            kq = min(k, n_items)
            labels, distances = index.knn_query(query.reshape(1, -1), k=kq)
            for i in range(len(labels[0])):
                local_id = labels[0][i]
                dist = float(distances[0][i])
                if local_id < len(self.partition_old_ids[pid]):
                    old_id = self.partition_old_ids[pid][local_id]
                    candidates.append((dist, int(old_id)))

        candidates.sort(key=lambda x: x[0])
        return candidates[:k]

def main():
    import sys
    base_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "output/partitions_kmeans200")
    index_dir = base_dir / "indexes"
    num_partitions = 200
    dim = 128

    print("Loading queries...")
    queries, _ = read_fvecs("data/test_1m_query1k.fvecs")
    gt = read_gt_bin("data/test_1m_gt1k.bin")
    nq = 200

    print(f"Queries: {len(queries)}, GT: {gt.shape}")

    print("\nInitializing PartitionedHNSW...")
    engine = PartitionedHNSW(index_dir, base_dir, num_partitions, dim)

    # 测试不同 k_routes
    print("\n=== Recall 测试 ===")
    for k_routes in [1, 2, 3]:
        engine.cache_order.clear()
        engine.partition_cache.clear()
        t0 = time.time()
        total_recall = 0
        for i in range(nq):
            results = engine.search(queries[i], k=10, k_routes=k_routes, ef=50)
            found = sum(1 for (_, old_id) in results if old_id in gt[i])
            total_recall += found
        recall = total_recall / (nq * 10) * 100
        elapsed = time.time() - t0
        print(f"  k_routes={k_routes}: recall={recall:.1f}%, "
              f"{elapsed:.2f}s ({nq} queries), {nq/elapsed:.0f} QPS")

    # 跨分区统计
    print("\n=== 跨分区分析 ===")
    cross = 0
    for i in range(nq):
        parts = engine.route(queries[i], 2)
        if len(parts) > 1:
            cross += 1
    print(f"  Cross-partition: {cross}/{nq} ({cross/nq*100:.0f}%)")

    # 各分区访问频率
    print("\n=== 分区访问频率 ===")
    freq = {}
    for i in range(nq):
        parts = engine.route(queries[i], 2)
        for p in parts:
            freq[p] = freq.get(p, 0) + 1
    top = sorted(freq.items(), key=lambda x: -x[1])[:10]
    print("  Top 10 partitions:")
    for pid, cnt in top:
        print(f"    Partition {pid}: {cnt} queries ({cnt/nq*100:.0f}%)")

    # 冷启动 vs 热缓存
    print("\n=== 缓存效果 ===")
    engine.cache_order.clear()
    engine.partition_cache.clear()
    t0 = time.time()
    for i in range(nq):
        _ = engine.search(queries[i], k=10, k_routes=2, ef=50)
    cold_elapsed = time.time() - t0

    t0 = time.time()
    for i in range(nq):
        _ = engine.search(queries[i], k=10, k_routes=2, ef=50)
    hot_elapsed = time.time() - t0
    print(f"  Cold: {cold_elapsed:.2f}s ({nq/cold_elapsed:.0f} QPS)")
    print(f"  Hot:  {hot_elapsed:.2f}s ({nq/hot_elapsed:.0f} QPS)")

    print("\nDone!")

if __name__ == "__main__":
    main()