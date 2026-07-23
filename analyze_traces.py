#!/usr/bin/env python3
"""
Phase 3a: Block 访问轨迹分析 - Delta 可学习性评估

分析内容:
1. Delta 分布 (block ID 差值的频率分布)
2. Delta 自相关 (时序相关性)
3. 跨查询 Block 重叠率 (LSTM 查询间预取的可行性)
4. 跨查询 Delta 模式相似性
5. Markov vs N-gram vs LSTM 预测精度对比
"""

import os
import numpy as np
from collections import Counter, defaultdict
import sys

TRACE_DIR = sys.argv[1] if len(sys.argv) > 1 else "logs/traces_oracle"

# ============================================================
# 1. 加载轨迹
# ============================================================
print(f"=== Loading traces from {TRACE_DIR} ===")

block_sequences = []
with open(f"{TRACE_DIR}/block_sequences.txt") as f:
    for line in f:
        parts = line.strip().split()
        if parts[0] == '#':
            continue
        qid = int(parts[0])
        blocks = [int(x) for x in parts[1:]]
        block_sequences.append(blocks)

query_meta = []
with open(f"{TRACE_DIR}/query_meta.txt") as f:
    for line in f:
        if line.startswith('#'):
            continue
        parts = line.strip().split()
        query_meta.append({
            'qid': int(parts[0]),
            'num_accesses': int(parts[1]),
            'num_unique': int(parts[2]),
            'latency_us': int(parts[3])
        })

print(f"  Queries: {len(block_sequences)}")
print(f"  Avg accesses/query: {np.mean([len(s) for s in block_sequences]):.0f}")
print(f"  Avg unique blocks/query: {np.mean([m['num_unique'] for m in query_meta]):.0f}")

# ============================================================
# 2. Delta 分布分析
# ============================================================
print(f"\n=== 2. Delta Distribution ===")

all_deltas = []
query_deltas = []  # 每个查询的 delta 序列
for blocks in block_sequences:
    deltas = [blocks[i+1] - blocks[i] for i in range(len(blocks)-1)]
    all_deltas.extend(deltas)
    query_deltas.append(deltas)

delta_counter = Counter(all_deltas)
print(f"  Total deltas: {len(all_deltas)}")
print(f"  Unique delta values: {len(delta_counter)}")
print(f"  Delta range: [{min(all_deltas)}, {max(all_deltas)}]")
print(f"  Delta mean: {np.mean(all_deltas):.2f}")
print(f"  Delta std: {np.std(all_deltas):.2f}")

# Top 20 most common deltas
print(f"\n  Top 20 most common deltas:")
print(f"  {'Delta':>8} {'Count':>8} {'Freq%':>8}")
for delta, count in delta_counter.most_common(20):
    freq = count / len(all_deltas) * 100
    print(f"  {delta:>8} {count:>8} {freq:>7.2f}%")

# Delta=0 (same block) ratio
zero_ratio = delta_counter.get(0, 0) / len(all_deltas) * 100
print(f"\n  Delta=0 (same block): {zero_ratio:.1f}%")

# Small delta (|d| <= 5) ratio
small_ratio = sum(c for d, c in delta_counter.items() if abs(d) <= 5) / len(all_deltas) * 100
print(f"  |Delta| <= 5: {small_ratio:.1f}%")

# Top-K coverage
top_k_values = [10, 50, 100, 200]
for k in top_k_values:
    top_k_count = sum(c for _, c in delta_counter.most_common(k))
    coverage = top_k_count / len(all_deltas) * 100
    print(f"  Top-{k} delta coverage: {coverage:.1f}%")

# ============================================================
# 3. 跨查询 Block 重叠率
# ============================================================
print(f"\n=== 3. Cross-Query Block Overlap ===")

# 计算相邻查询的 Block 集合重叠率
overlaps_consecutive = []
overlaps_all = []

# 先收集每个查询的 unique block 集合
block_sets = [set(b for b in blocks) for blocks in block_sequences]

# 相邻查询重叠率
for i in range(len(block_sets) - 1):
    s1, s2 = block_sets[i], block_sets[i+1]
    if len(s1) == 0 or len(s2) == 0:
        continue
    overlap = len(s1 & s2) / len(s1 | s2)  # Jaccard
    overlaps_consecutive.append(overlap)

# 全局重叠: 每个查询与所有其他查询的平均重叠
# 采样 100 个查询
sample_indices = np.random.choice(len(block_sets), min(100, len(block_sets)), replace=False)
for i in sample_indices:
    overlaps_i = []
    for j in range(len(block_sets)):
        if i == j:
            continue
        s1, s2 = block_sets[i], block_sets[j]
        if len(s1) == 0 or len(s2) == 0:
            continue
        overlap = len(s1 & s2) / len(s1 | s2)
        overlaps_i.append(overlap)
    overlaps_all.append(np.mean(overlaps_i))

print(f"  Consecutive query overlap (Jaccard):")
print(f"    Mean: {np.mean(overlaps_consecutive):.4f}")
print(f"    Std: {np.std(overlaps_consecutive):.4f}")
print(f"    Min: {np.min(overlaps_consecutive):.4f}")
print(f"    Max: {np.max(overlaps_consecutive):.4f}")

print(f"\n  Random query pair overlap (Jaccard):")
print(f"    Mean: {np.mean(overlaps_all):.4f}")
print(f"    Std: {np.std(overlaps_all):.4f}")

# Block 访问频率排序 (热点分析)
block_access_count = Counter()
for blocks in block_sequences:
    for b in set(blocks):  # unique per query
        block_access_count[b] += 1

print(f"\n  Block access frequency (across {len(block_sequences)} queries):")
print(f"  Total unique blocks: {len(block_access_count)}")
freqs = sorted(block_access_count.values(), reverse=True)
print(f"  Top 10% blocks cover: {sum(freqs[:len(freqs)//10])}/{len(block_sequences)} queries ({sum(freqs[:len(freqs)//10])/len(block_sequences)*100:.1f}%)")
print(f"  Top 20% blocks cover: {sum(freqs[:len(freqs)//5])}/{len(block_sequences)} queries ({sum(freqs[:len(freqs)//5])/len(block_sequences)*100:.1f}%)")
print(f"  Top 50% blocks cover: {sum(freqs[:len(freqs)//2])}/{len(block_sequences)} queries ({sum(freqs[:len(freqs)//2])/len(block_sequences)*100:.1f}%)")

# ============================================================
# 4. Delta 序列可预测性分析
# ============================================================
print(f"\n=== 4. Delta Predictability ===")

# 4.1 一阶 Markov 预测精度 (作为 baseline)
# P(next_delta | current_delta)
markov_transitions = defaultdict(Counter)
for deltas in query_deltas:
    for i in range(len(deltas) - 1):
        markov_transitions[deltas[i]][deltas[i+1]] += 1

# 在前 800 个查询上训练, 后 200 个查询上测试
train_deltas = query_deltas[:800]
test_deltas = query_deltas[800:]

# 重新训练 Markov (只在训练集上)
markov_model = defaultdict(Counter)
for deltas in train_deltas:
    for i in range(len(deltas) - 1):
        markov_model[deltas[i]][deltas[i+1]] += 1

# 测试: Top-1 和 Top-10 预测精度
top1_correct = 0
top10_correct = 0
total_pred = 0

for deltas in test_deltas:
    for i in range(len(deltas) - 1):
        curr = deltas[i]
        actual = deltas[i+1]
        
        if curr in markov_model:
            predictions = [d for d, _ in markov_model[curr].most_common(10)]
            if actual in predictions[:1]:
                top1_correct += 1
            if actual in predictions:
                top10_correct += 1
        total_pred += 1

print(f"  Markov (1st order) on test set:")
print(f"    Total predictions: {total_pred}")
print(f"    Top-1 accuracy: {top1_correct/total_pred*100:.2f}%")
print(f"    Top-10 accuracy: {top10_correct/total_pred*100:.2f}%")

# 4.2 二阶 Markov
markov2_model = defaultdict(Counter)
for deltas in train_deltas:
    for i in range(len(deltas) - 2):
        key = (deltas[i], deltas[i+1])
        markov2_model[key][deltas[i+2]] += 1

top1_correct_2 = 0
top10_correct_2 = 0
total_pred_2 = 0

for deltas in test_deltas:
    for i in range(len(deltas) - 2):
        key = (deltas[i], deltas[i+1])
        actual = deltas[i+2]
        
        if key in markov2_model:
            predictions = [d for d, _ in markov2_model[key].most_common(10)]
            if actual in predictions[:1]:
                top1_correct_2 += 1
            if actual in predictions:
                top10_correct_2 += 1
        total_pred_2 += 1

print(f"\n  Markov (2nd order) on test set:")
print(f"    Total predictions: {total_pred_2}")
print(f"    Top-1 accuracy: {top1_correct_2/total_pred_2*100:.2f}%")
print(f"    Top-10 accuracy: {top10_correct_2/total_pred_2*100:.2f}%")

# 4.3 Block ID 直接预测 (不通过 Delta)
# P(next_block | current_block)
block_markov = defaultdict(Counter)
for blocks in block_sequences[:800]:
    for i in range(len(blocks) - 1):
        block_markov[blocks[i]][blocks[i+1]] += 1

# 测试
b_top1 = 0
b_top10 = 0
b_total = 0

for blocks in block_sequences[800:]:
    for i in range(len(blocks) - 1):
        curr = blocks[i]
        actual = blocks[i+1]
        
        if curr in block_markov:
            predictions = [b for b, _ in block_markov[curr].most_common(10)]
            if actual in predictions[:1]:
                b_top1 += 1
            if actual in predictions:
                b_top10 += 1
        b_total += 1

print(f"\n  Block-level Markov (1st order):")
print(f"    Top-1 accuracy: {b_top1/b_total*100:.2f}%")
print(f"    Top-10 accuracy: {b_top10/b_total*100:.2f}%")
print(f"    (Note: high top-10 because delta=0 -> same block is common)")

# ============================================================
# 5. 查询间 Block 重叠 -> 查询间预取可行性
# ============================================================
print(f"\n=== 5. Inter-Query Prefetch Feasibility ===")

# 如果我们预取查询 N 访问的所有 unique blocks, 查询 N+1 能命中多少?
prefetch_hit_rates = []
for i in range(len(block_sets) - 1):
    s_n = block_sets[i]
    s_n1 = block_sets[i+1]
    if len(s_n1) == 0:
        continue
    # 预取 s_n 中的所有 block, 查询 N+1 的命中率
    hit_rate = len(s_n & s_n1) / len(s_n1)
    prefetch_hit_rates.append(hit_rate)

print(f"  Prefetch query N's blocks for query N+1:")
print(f"    Mean hit rate: {np.mean(prefetch_hit_rates)*100:.1f}%")
print(f"    Std: {np.std(prefetch_hit_rates)*100:.1f}%")
print(f"    P50: {np.percentile(prefetch_hit_rates, 50)*100:.1f}%")
print(f"    P95: {np.percentile(prefetch_hit_rates, 95)*100:.1f}%")

# 预取最近 N 个查询的并集
for window in [2, 3, 5, 10]:
    union_hit_rates = []
    for i in range(window, len(block_sets)):
        # 预取最近 window 个查询的 block 并集
        prefetch_set = set()
        for j in range(i - window, i):
            prefetch_set |= block_sets[j]
        # 查询 i 的命中率
        if len(block_sets[i]) == 0:
            continue
        hit_rate = len(prefetch_set & block_sets[i]) / len(block_sets[i])
        union_hit_rates.append(hit_rate)
    print(f"  Prefetch union of last {window} queries:")
    print(f"    Mean hit rate: {np.mean(union_hit_rates)*100:.1f}%, "
          f"prefetch set size: {len(prefetch_set)} blocks")

# ============================================================
# 6. 总结
# ============================================================
print(f"\n=== 6. Summary & Recommendations ===")
print(f"  Delta distribution:")
print(f"    - {len(delta_counter)} unique delta values")
print(f"    - Top-50 deltas cover {sum(c for _, c in delta_counter.most_common(50))/len(all_deltas)*100:.1f}% of accesses")
print(f"    - |Delta|<=5 covers {small_ratio:.1f}%")
print(f"  Markov prediction (delta-level):")
print(f"    - 1st order Top-10: {top10_correct/total_pred*100:.1f}%")
print(f"    - 2nd order Top-10: {top10_correct_2/total_pred_2*100:.1f}%")
print(f"  Inter-query prefetch:")
print(f"    - Consecutive overlap: {np.mean(overlaps_consecutive)*100:.1f}%")
print(f"    - Prefetch N for N+1: {np.mean(prefetch_hit_rates)*100:.1f}% hit rate")
print(f"  Unique blocks/query: {np.mean([m['num_unique'] for m in query_meta]):.0f}")
print(f"  Total blocks: 2225")
print(f"  Working set ratio: {np.mean([m['num_unique'] for m in query_meta])/2225*100:.1f}%")
