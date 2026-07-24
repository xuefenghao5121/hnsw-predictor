#!/usr/bin/env python3
"""
简化版 BFS 割边分区工具

按 BFS 顺序将向量切分到不同分区
"""

import numpy as np
import struct
from pathlib import Path

def read_fvecs(path):
    """读取 fvecs 格式文件"""
    with open(path, 'rb') as f:
        dim = struct.unpack('i', f.read(4))[0]
        f.seek(0, 2)
        file_size = f.tell()
        f.seek(0, 0)

        vec_size = 4 + dim * 4
        num_vectors = file_size // vec_size

        vectors = []
        for _ in range(num_vectors):
            vec_dim = struct.unpack('i', f.read(4))[0]
            assert vec_dim == dim
            vec = struct.unpack(f'{dim}f', f.read(dim * 4))
            vectors.append(vec)

        return np.array(vectors, dtype=np.float32), dim

def write_fvecs(path, vectors):
    """写入 fvecs 格式文件"""
    with open(path, 'wb') as f:
        num_vectors, dim = vectors.shape
        for vec in vectors:
            f.write(struct.pack('i', dim))
            f.write(struct.pack(f'{dim}f', *vec))

def read_bfs_order(path):
    """读取 BFS 顺序 new_to_old（new_id → old_id）
    文件结构: header[4] + old_to_new[N] + new_to_old[N]
    """
    data = np.fromfile(path, dtype=np.uint32)
    n = (len(data) - 4) // 2
    new_to_old = data[4 + n : 4 + 2 * n]
    return new_to_old

def partition_vectors(vectors, new_to_old, num_partitions=200):
    """按 BFS 顺序切分向量
    切分 new_id 空间（BFS 连续区域），通过 new_to_old 映射到原始节点
    """
    num_nodes = len(new_to_old)
    nodes_per_partition = num_nodes // num_partitions

    print(f"Partitioning {num_nodes} vectors into {num_partitions} partitions")

    partitions = []
    for pid in range(num_partitions):
        start = pid * nodes_per_partition
        end = (pid + 1) * nodes_per_partition if pid < num_partitions - 1 else num_nodes

        # 取 BFS 顺序中连续一段的 new_id
        new_ids = np.arange(start, end, dtype=np.uint32)
        # 映射到 old_id
        old_ids = new_to_old[new_ids]

        # 提取向量（按 old_id）
        partition_vectors = vectors[old_ids]

        partitions.append({
            'pid': pid,
            'new_ids': new_ids,
            'old_ids': old_ids,
            'vectors': partition_vectors
        })

        if pid < 5:
            print(f"  Partition {pid}: {len(partition_vectors)} vectors, new_id=[{start},{end})")

    return partitions

def main():
    vectors_path = "data/test_1m.fvecs"
    bfs_path = "output/test1m_bfs.bin"
    output_dir = "output/partitions_bfs200"
    num_partitions = 200

    print("=" * 60)
    print("BFS 向量分区工具（简化版）")
    print("=" * 60)

    # 1. 读取向量
    print(f"\n1. Reading vectors from {vectors_path}...")
    vectors, dim = read_fvecs(vectors_path)
    num_nodes = len(vectors)
    print(f"   Loaded {num_nodes} vectors, dim={dim}")

    # 2. 读取 BFS 顺序
    print(f"\n2. Reading BFS order from {bfs_path}...")
    new_to_old = read_bfs_order(bfs_path)
    print(f"   Loaded {len(new_to_old)} node IDs")

    # 3. 切分
    print(f"\n3. Partitioning...")
    partitions = partition_vectors(vectors, new_to_old, num_partitions)

    # 4. 导出
    print(f"\n4. Exporting to {output_dir}...")
    output_path = Path(output_dir)
    output_path.mkdir(exist_ok=True)

    for partition in partitions:
        pid = partition['pid']
        pvectors = partition['vectors']
        old_ids = partition['old_ids']

        # 导出 fvecs
        fvecs_path = output_path / f"partition_{pid:04d}.fvecs"
        write_fvecs(fvecs_path, pvectors)

        # 导出 old_id 映射（局部 local_id → 全局 old_id）
        mapping_path = output_path / f"partition_{pid:04d}.ids"
        old_ids.astype(np.uint32).tofile(mapping_path)

        if pid < 5 or pid == len(partitions) - 1:
            print(f"  Partition {pid}: {len(pvectors)} vectors -> {fvecs_path}")

    # 5. 导出全局映射 old_id → partition_id
    mapping_path = output_path / "partition_mapping.bin"
    node_to_partition = np.zeros(num_nodes, dtype=np.uint32)
    for partition in partitions:
        pid = partition['pid']
        for oid in partition['old_ids']:
            node_to_partition[oid] = pid

    node_to_partition.tofile(mapping_path)
    print(f"\nGlobal mapping (old_id -> partition_id) saved to {mapping_path}")

    print("\n" + "=" * 60)
    print("Done!")
    print("=" * 60)

if __name__ == "__main__":
    main()