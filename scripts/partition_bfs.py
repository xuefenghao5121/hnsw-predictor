#!/usr/bin/env python3
"""
BFS 割边分区工具

将 1M 向量按 BFS 顺序切分成 N 个分区
"""

import numpy as np
import struct
from pathlib import Path
from collections import defaultdict

def read_fvecs(path):
    """读取 fvecs 格式文件"""
    with open(path, 'rb') as f:
        dim = struct.unpack('i', f.read(4))[0]
        f.seek(0, 2)  # 移到文件末尾
        file_size = f.tell()
        f.seek(0, 0)
        
        # 每个向量: 4 字节维度 + dim * 4 字节浮点数
        vec_size = 4 + dim * 4
        num_vectors = file_size // vec_size
        
        vectors = []
        for _ in range(num_vectors):
            vec_dim = struct.unpack('i', f.read(4))[0]
            assert vec_dim == dim
            vec = struct.unpack(f'{dim}f', f.read(dim * 4))
            vectors.append(vec)
        
        return np.array(vectors, dtype=np.float32)

def read_bfs_order(path):
    """读取 BFS 顺序"""
    return np.fromfile(path, dtype=np.uint32)

def read_graph_structure(path, num_nodes):
    """读取图结构（自定义二进制格式）"""
    with open(path, 'rb') as f:
        # Header: num_nodes (uint32), max_level (uint32), entry_point (uint32)
        num_nodes_read = struct.unpack('I', f.read(4))[0]
        max_level = struct.unpack('I', f.read(4))[0]
        entry_point = struct.unpack('I', f.read(4))[0]

        print(f"Graph: num_nodes={num_nodes_read}, max_level={max_level}, entry_point={entry_point}")

        assert num_nodes_read == num_nodes

        # 读取每层的边
        layers = {}
        for level in range(max_level + 1):
            # 每层格式: node_count (uint32), 然后 [node_id, neighbor_count, neighbors...]
            layer_nodes = struct.unpack('I', f.read(4))[0]
            layer_data = {}

            for _ in range(layer_nodes):
                node_id = struct.unpack('I', f.read(4))[0]
                neighbor_count = struct.unpack('I', f.read(4))[0]
                neighbors = list(struct.unpack(f'{neighbor_count}I', f.read(4 * neighbor_count)))
                layer_data[node_id] = neighbors

            layers[level] = layer_data
            print(f"  Layer {level}: {len(layer_data)} nodes")

        return layers, max_level, entry_point

def partition_bfs(bfs_order, num_partitions=200):
    """按 BFS 顺序切分分区"""
    num_nodes = len(bfs_order)
    nodes_per_partition = num_nodes // num_partitions

    partitions = defaultdict(list)
    node_to_partition = {}

    for i, node_id in enumerate(bfs_order):
        partition_id = i // nodes_per_partition
        partitions[partition_id].append(node_id)
        node_to_partition[node_id] = partition_id

    print(f"Partitioned {num_nodes} nodes into {num_partitions} partitions")
    for pid in range(min(5, num_partitions)):
        print(f"  Partition {pid}: {len(partitions[pid])} nodes")

    return partitions, node_to_partition

def export_partitions(partitions, node_to_partition, graph_layers, vectors, output_dir):
    """导出分区数据"""
    output_dir = Path(output_dir)
    output_dir.mkdir(exist_ok=True)

    dim = vectors.shape[1]

    # 全局节点 ID -> 局部节点 ID 映射（每个分区独立）
    # 跨分区边：{local_node_id: {partition_id: [neighbor_local_ids]}}

    partition_metadata = {}

    for partition_id, global_node_ids in partitions.items():
        print(f"\nExporting partition {partition_id} ({len(global_node_ids)} nodes)...")

        # 建立全局 -> 局部映射
        global_to_local = {gid: i for i, gid in enumerate(global_node_ids)}
        local_to_global = global_node_ids

        # 提取向量
        partition_vectors = vectors[global_node_ids]

        # 提取图边（只保留内部边，跨分区边单独记录）
        partition_layers = {}
        external_edges = defaultdict(list)  # {level: [(local_id, external_partition, external_local_id)]}

        for level, layer_graph in graph_layers.items():
            partition_layer = {}
            for local_id, global_id in enumerate(global_node_ids):
                if global_id not in layer_graph:
                    continue

                neighbors = layer_graph[global_id]
                internal_neighbors = []
                external_neighbors = []

                for neighbor_global in neighbors:
                    if neighbor_global not in node_to_partition:
                        continue

                    neighbor_partition = node_to_partition[neighbor_global]

                    if neighbor_partition == partition_id:
                        # 内部边
                        neighbor_local = global_to_local[neighbor_global]
                        internal_neighbors.append(neighbor_local)
                    else:
                        # 跨分区边（暂时跳过，需要知道对方的局部 ID）
                        # 先记录，后续处理
                        external_edges[level].append((local_id, neighbor_global))

            if internal_neighbors:
                partition_layer[local_id] = internal_neighbors

            partition_layers[level] = partition_layer

        # 导出分区文件
        partition_path = output_dir / f"partition_{partition_id:04d}.bin"
        with open(partition_path, 'wb') as f:
            # Header
            f.write(struct.pack('I', len(global_node_ids)))      # num_nodes
            f.write(struct.pack('I', len(partition_layers) - 1))  # max_level (0-based)
            # f.write(struct.pack('I', 0))  # entry_point (待确定)

            # 节点向量
            for local_id in range(len(global_node_ids)):
                global_id = global_node_ids[local_id]
                f.write(struct.pack(f'{dim}f', *vectors[global_id]))

            # 图层
            for level in range(len(partition_layers)):
                layer_data = partition_layers.get(level, {})

                f.write(struct.pack('I', len(layer_data)))  # node_count

                for local_id, neighbors in layer_data.items():
                    f.write(struct.pack('I', local_id))      # node_id
                    f.write(struct.pack('I', len(neighbors)))  # neighbor_count
                    if neighbors:
                        f.write(struct.pack(f'{len(neighbors)}I', *neighbors))

        partition_metadata[partition_id] = {
            'num_nodes': len(global_node_ids),
            'max_level': len(partition_layers) - 1,
            'path': str(partition_path)
        }

        print(f"  Saved to {partition_path}")

    # 导出全局映射表
    mapping_path = output_dir / "partition_mapping.bin"
    with open(mapping_path, 'wb') as f:
        f.write(struct.pack('I', len(node_to_partition)))  # num_nodes
        for global_id in range(len(node_to_partition)):
            pid = node_to_partition.get(global_id, 0)
            f.write(struct.pack('I', pid))

    print(f"\nPartition mapping saved to {mapping_path}")

    return partition_metadata

def main():
    # 配置
    vectors_path = "data/test_1m.fvecs"
    bfs_path = "output/test1m_bfs.bin"
    graph_path = "output/test1m_graph.bin"
    output_dir = "output/partitions_bfs200"
    num_partitions = 200

    print("=" * 60)
    print("BFS 割边分区工具")
    print("=" * 60)

    # 1. 读取向量
    print(f"\n1. Reading vectors from {vectors_path}...")
    vectors = read_fvecs(vectors_path)
    num_nodes, dim = vectors.shape
    print(f"   Loaded {num_nodes} vectors, dim={dim}")

    # 2. 读取 BFS 顺序
    print(f"\n2. Reading BFS order from {bfs_path}...")
    bfs_order = read_bfs_order(bfs_path)
    print(f"   Loaded {len(bfs_order)} node IDs")

    # 3. 读取图结构
    print(f"\n3. Reading graph structure from {graph_path}...")
    graph_layers, max_level, entry_point = read_graph_structure(graph_path, num_nodes)

    # 4. 按 BFS 顺序切分
    print(f"\n4. Partitioning into {num_partitions} partitions...")
    partitions, node_to_partition = partition_bfs(bfs_order, num_partitions)

    # 5. 导出分区
    print(f"\n5. Exporting partitions to {output_dir}...")
    partition_metadata = export_partitions(partitions, node_to_partition, graph_layers, vectors, output_dir)

    print("\n" + "=" * 60)
    print("Done!")
    print("=" * 60)

if __name__ == "__main__":
    main()