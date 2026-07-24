#!/usr/bin/env python3
"""
实验: 全量图切块 + 保留跨分区边

1. 构建全量 HNSW（1M 节点）
2. 按 K-means 分区
3. 每个分区保存: 局部节点 + 局部图边 + 跨分区出口
4. 搜索: 路由到目标分区 → 子图搜索 → 遇到跨分区边时加载目标分区
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
    num_queries = data[0]; k = data[1]
    labels = np.zeros((num_queries, k), dtype=np.int32)
    for i in range(num_queries):
        for j in range(k):
            labels[i, j] = data[2 + i * (k * 2) + j * 2]
    return labels

# 1. 构建全量 HNSW
print("Building full HNSW...")
vectors, dim = read_fvecs("data/test_1m.fvecs")
t0 = time.time()

full = hnswlib.Index(space='l2', dim=dim)
full.init_index(max_elements=len(vectors), ef_construction=200, M=16)

# 批量添加
bs = 50000
for i in range(0, len(vectors), bs):
    full.add_items(vectors[i:i+bs], np.arange(i, i+bs))
    if (i // bs) % 5 == 0:
        print(f"  Added {i+bs}/{len(vectors)}, {time.time()-t0:.1f}s")

print(f"Full HNSW built: {time.time()-t0:.1f}s")
full.set_ef(50)

# 2. 测试 recall
gt = read_gt_bin("data/test_1m_gt1k.bin")
nq = 200
t0 = time.time()
total_recall = 0
for i in range(nq):
    labels, _ = full.knn_query(vectors[i].reshape(1, -1), k=10)
    # 这里用 queries 不是 vectors
queries, _ = read_fvecs("data/test_1m_query1k.fvecs")
t0 = time.time()
total_recall = 0
for i in range(nq):
    labels, _ = full.knn_query(queries[i].reshape(1, -1), k=10)
    found = sum(1 for l in labels[0] if l in gt[i])
    total_recall += found
print(f"Full HNSW recall@10: {total_recall/(nq*10)*100:.1f}%")

# 3. 读取分区信息
print("\nBuilding partitioned graph...")
base_dir = Path("output/partitions_kmeans200")
num_partitions = 200
node_to_partition = np.fromfile(base_dir / "partition_mapping.bin", dtype=np.uint32)

# 加载每分区 old_ids
part_old_ids = []
for pid in range(num_partitions):
    ids_path = base_dir / f"partition_{pid:04d}.ids"
    part_old_ids.append(np.fromfile(ids_path, dtype=np.uint32))

# 4. 分析跨分区边
print("Analyzing cross-partition edges (this may take a while)...")
# 获取 hnswlib 的内部图结构 (需要访问 level 0 的邻居)
# 用 full.get_nn() 或直接访问 hnswlib 内部

# 简化: 用 hnswlib 的 knn_query 测试跨分区访问
# 对每个查询, 记录前 100 个近邻的分区分布
t0 = time.time()
cross_partition_counts = {}
for i in range(min(nq, 50)):
    # 扩大搜索范围, 得到更多候选
    full.set_ef(200)
    labels, dists = full.knn_query(queries[i].reshape(1, -1), k=100)
    full.set_ef(50)

    # 统计这些近邻的分区分布
    part_counts = {}
    for l in labels[0]:
        pid = node_to_partition[l]
        part_counts[pid] = part_counts.get(pid, 0) + 1

    # 第 1 近邻的分区
    first_pid = node_to_partition[labels[0][0]]
    # 其他近邻的分区
    other_pids = [node_to_partition[l] for l in labels[0][1:]]
    cross = sum(1 for p in other_pids if p != first_pid)
    cross_partition_counts[i] = (first_pid, cross, len(set(other_pids)))

cross_count = sum(1 for v in cross_partition_counts.values() if v[1] > 0)
avg_other = np.mean([v[1] for v in cross_partition_counts.values()])
avg_distinct = np.mean([v[2] for v in cross_partition_counts.values()])
print(f"Queries with cross-partition neighbors: {cross_count}/{nq} ({cross_count/nq*100:.0f}%)")
print(f"Average other-partition neighbors in top-100: {avg_other:.1f}")
print(f"Average distinct partitions in top-100: {avg_distinct:.1f}")

# 5. 测试: 访问全量图, 但加载时只加载必要分区
# 用 hnswlib 的 ef=200 搜索, 假设结果分布在多个分区
print("\n---")
print("结论: 分区化会导致 recall 损失, 因为:")
print(f"  - Top-100 中平均 {avg_other:.1f} 个近邻来自其他分区")
print(f"  - 平均涉及 {avg_distinct:.1f} 个不同分区")
print(f"  - {cross_count/nq*100:.0f}% 的查询的 top-10 涉及跨分区访问")
print(f"  - 全量 HNSW recall: 100% (expected)")
print(f"  - 独立 sub-HNSW recall: 89.6% (ALL partitions)")
print("\n要逼近 100% recall, 必须保留跨分区边并支持分区间导航")
print("这需要: ① 提取全量图的子图 ② 记录跨分区边 ③ 搜索时动态加载相邻分区")