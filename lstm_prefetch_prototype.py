#!/usr/bin/env python3
"""
Phase 3b: LSTM Miss-Delta 预取原型

1. 从 c1024 轨迹提取 miss block ID 序列
2. 构建 Delta 序列 (相邻 miss block ID 差值)
3. 训练 LSTM 预测下一个 miss Delta
4. 对比 Markov / N-gram / LSTM 预测精度
"""

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from collections import Counter, defaultdict
import sys
import os

TRACE_DIR = "logs/traces_c1024"

# ============================================================
# 1. 加载 miss 序列
# ============================================================
print("=== 1. Loading miss sequences ===")

# 每个查询的 miss block ID 序列 (按访问顺序)
query_miss_seqs = []  # query_miss_seqs[q] = [block_id1, block_id2, ...]
query_all_seqs = []   # 所有 block 访问序列

with open(f"{TRACE_DIR}/block_traces.txt") as f:
    current_qid = -1
    current_all = []
    current_miss = []
    for line in f:
        if line.startswith('#'):
            continue
        parts = line.strip().split()
        qid = int(parts[0])
        bid = int(parts[1])
        is_hit = int(parts[2])
        
        if qid != current_qid:
            if current_qid >= 0:
                query_miss_seqs.append(current_miss)
                query_all_seqs.append(current_all)
            current_qid = qid
            current_all = []
            current_miss = []
        current_all.append(bid)
        if not is_hit:
            current_miss.append(bid)
    if current_qid >= 0:
        query_miss_seqs.append(current_miss)
        query_all_seqs.append(current_all)

print(f"  Queries: {len(query_miss_seqs)}")
print(f"  Total misses: {sum(len(s) for s in query_miss_seqs)}")
print(f"  Avg misses/query: {np.mean([len(s) for s in query_miss_seqs]):.1f}")

# ============================================================
# 2. 构建 Delta 序列
# ============================================================
print("\n=== 2. Building Delta sequences ===")

# 每个查询的 miss delta 序列
query_miss_deltas = []
for miss_seq in query_miss_seqs:
    if len(miss_seq) > 1:
        deltas = [miss_seq[i+1] - miss_seq[i] for i in range(len(miss_seq)-1)]
        query_miss_deltas.append(deltas)
    else:
        query_miss_deltas.append([])

all_deltas = [d for seq in query_miss_deltas for d in seq]
delta_counter = Counter(all_deltas)

print(f"  Total miss-deltas: {len(all_deltas)}")
print(f"  Unique delta values: {len(delta_counter)}")

# 构建 delta vocabulary (Top-K deltas, 其余归为 UNK)
TOP_K = 100  # vocab size
most_common = delta_counter.most_common(TOP_K)
delta_to_id = {d: i+1 for i, (d, _) in enumerate(most_common)}  # 0 = UNK
id_to_delta = {i+1: d for i, (d, _) in enumerate(most_common)}
UNK_ID = 0

# 统计 vocab 覆盖率
covered = sum(c for d, c in most_common)
print(f"  Vocab size: {TOP_K} (+UNK)")
print(f"  Vocab coverage: {covered/len(all_deltas)*100:.1f}%")

# 编码 delta 序列
def encode_deltas(deltas):
    return [delta_to_id.get(d, UNK_ID) for d in deltas]

encoded_seqs = [encode_deltas(d) for d in query_miss_deltas]

# ============================================================
# 3. 划分训练/测试集
# ============================================================
print("\n=== 3. Splitting train/test ===")
n_train = 800
n_test = len(encoded_seqs) - n_train

train_seqs = encoded_seqs[:n_train]
test_seqs = encoded_seqs[n_train:]

# 同时保留原始 block ID 序列用于评估
train_blocks = query_miss_seqs[:n_train]
test_blocks = query_miss_seqs[n_train:]

print(f"  Train: {n_train} queries, {sum(len(s) for s in train_seqs)} deltas")
print(f"  Test: {n_test} queries, {sum(len(s) for s in test_seqs)} deltas")

# ============================================================
# 4. Baseline: Markov 预测
# ============================================================
print("\n=== 4. Markov Baselines ===")

# 4a. 1st order Markov on delta
m1 = defaultdict(Counter)
for seq in train_seqs:
    for i in range(len(seq)-1):
        m1[seq[i]][seq[i+1]] += 1

# Test
top1_correct, top5_correct, top10_correct, total = 0, 0, 0, 0
for seq in test_seqs:
    for i in range(len(seq)-1):
        curr, actual = seq[i], seq[i+1]
        if curr in m1:
            preds = [d for d, _ in m1[curr].most_common(10)]
            if actual in preds[:1]: top1_correct += 1
            if actual in preds[:5]: top5_correct += 1
            if actual in preds: top10_correct += 1
        total += 1

print(f"  Markov 1st order: Top-1={top1_correct/total*100:.1f}%, Top-5={top5_correct/total*100:.1f}%, Top-10={top10_correct/total*100:.1f}%")

# 4b. 2nd order Markov on delta
m2 = defaultdict(Counter)
for seq in train_seqs:
    for i in range(len(seq)-2):
        m2[(seq[i], seq[i+1])][seq[i+2]] += 1

top1_2, top5_2, top10_2, total_2 = 0, 0, 0, 0
for seq in test_seqs:
    for i in range(len(seq)-2):
        key = (seq[i], seq[i+1])
        actual = seq[i+2]
        if key in m2:
            preds = [d for d, _ in m2[key].most_common(10)]
            if actual in preds[:1]: top1_2 += 1
            if actual in preds[:5]: top5_2 += 1
            if actual in preds: top10_2 += 1
        total_2 += 1

print(f"  Markov 2nd order: Top-1={top1_2/total_2*100:.1f}%, Top-5={top5_2/total_2*100:.1f}%, Top-10={top10_2/total_2*100:.1f}%")

# 4c. Static Top-K (always predict most common deltas)
static_preds = [d for d, _ in most_common[:10]]
top10_static = sum(1 for seq in test_seqs for d in seq if d in static_preds) / max(1, sum(len(seq) for seq in test_seqs))
print(f"  Static Top-10: {top10_static*100:.1f}%")

# ============================================================
# 5. LSTM Model
# ============================================================
print("\n=== 5. LSTM Training ===")

class DeltaDataset(Dataset):
    def __init__(self, sequences, seq_len=20):
        self.samples = []
        for seq in sequences:
            for i in range(len(seq) - seq_len):
                self.samples.append((seq[i:i+seq_len], seq[i+seq_len]))
        # Also add shorter sequences with padding
        for seq in sequences:
            if len(seq) > 1 and len(seq) <= seq_len:
                padded = [0] * (seq_len - len(seq)) + seq[:-1]
                self.samples.append((padded, seq[-1]))
    
    def __len__(self):
        return len(self.samples)
    
    def __getitem__(self, idx):
        x, y = self.samples[idx]
        return torch.tensor(x, dtype=torch.long), torch.tensor(y, dtype=torch.long)

class DeltaLSTM(nn.Module):
    def __init__(self, vocab_size, embed_dim=32, hidden_dim=64, num_layers=2):
        super().__init__()
        self.embedding = nn.Embedding(vocab_size, embed_dim, padding_idx=0)
        self.lstm = nn.LSTM(embed_dim, hidden_dim, num_layers=num_layers, batch_first=True, dropout=0.1)
        self.fc = nn.Linear(hidden_dim, vocab_size)
    
    def forward(self, x):
        emb = self.embedding(x)  # (B, L, E)
        out, _ = self.lstm(emb)  # (B, L, H)
        logits = self.fc(out[:, -1, :])  # (B, V)
        return logits

SEQ_LEN = 20
VOCAB_SIZE = TOP_K + 1  # +UNK
BATCH_SIZE = 256
EPOCHS = 20
LR = 0.001

train_dataset = DeltaDataset(train_seqs, SEQ_LEN)
test_dataset = DeltaDataset(test_seqs, SEQ_LEN)

print(f"  Train samples: {len(train_dataset)}")
print(f"  Test samples: {len(test_dataset)}")

train_loader = DataLoader(train_dataset, batch_size=BATCH_SIZE, shuffle=True)
test_loader = DataLoader(test_dataset, batch_size=BATCH_SIZE, shuffle=False)

device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
print(f"  Device: {device}")

model = DeltaLSTM(VOCAB_SIZE, embed_dim=32, hidden_dim=64, num_layers=2).to(device)
optimizer = torch.optim.Adam(model.parameters(), lr=LR)
criterion = nn.CrossEntropyLoss()

# Training
for epoch in range(EPOCHS):
    model.train()
    total_loss = 0
    for x, y in train_loader:
        x, y = x.to(device), y.to(device)
        optimizer.zero_grad()
        logits = model(x)
        loss = criterion(logits, y)
        loss.backward()
        optimizer.step()
        total_loss += loss.item() * x.size(0)
    
    if (epoch+1) % 5 == 0:
        model.eval()
        top1, top5, top10, total = 0, 0, 0, 0
        with torch.no_grad():
            for x, y in test_loader:
                x, y = x.to(device), y.to(device)
                logits = model(x)
                _, preds = logits.topk(10, dim=1)
                for i in range(y.size(0)):
                    actual = y[i].item()
                    p = preds[i].cpu().tolist()
                    if actual in p[:1]: top1 += 1
                    if actual in p[:5]: top5 += 1
                    if actual in p: top10 += 1
                    total += 1
        
        print(f"  Epoch {epoch+1}: loss={total_loss/len(train_dataset):.4f}, "
              f"Top-1={top1/total*100:.1f}%, Top-5={top5/total*100:.1f}%, Top-10={top10/total*100:.1f}%")

# Final evaluation
model.eval()
top1, top5, top10, total = 0, 0, 0, 0
with torch.no_grad():
    for x, y in test_loader:
        x, y = x.to(device), y.to(device)
        logits = model(x)
        _, preds = logits.topk(10, dim=1)
        for i in range(y.size(0)):
            actual = y[i].item()
            p = preds[i].cpu().tolist()
            if actual in p[:1]: top1 += 1
            if actual in p[:5]: top5 += 1
            if actual in p: top10 += 1
            total += 1

# ============================================================
# 6. Block-level prediction accuracy
# ============================================================
print("\n=== 6. Block-Level Prediction ===")

# 将 delta 预测转换为 block ID 预测
# 如果预测的 delta 对应的 block ID == 实际 miss block ID，则命中
# 注意：delta 预测正确 != block 预测正确（因为 delta 可以是 UNK）

# 计算实际收益：在测试集上模拟预取
def simulate_prefetch_block_accuracy(miss_seqs, delta_seqs, model, delta_to_id, id_to_delta, device, top_k=10):
    """
    对每个查询的 miss 序列，用模型预测下一个 miss block，
    看有多少能命中实际 miss 的 block。
    """
    total_misses = 0
    predicted_hits = 0
    
    for miss_seq, delta_seq in zip(miss_seqs, delta_seqs):
        if len(miss_seq) < 2:
            continue
        
        # 用前 SEQ_LEN 个 delta 预测下一个
        for i in range(len(delta_seq)):
            start = max(0, i - SEQ_LEN + 1)
            context = delta_seq[start:i+1]
            if len(context) < 1:
                continue
            
            # Padding
            padded = [0] * (SEQ_LEN - len(context)) + context
            x = torch.tensor([padded], dtype=torch.long).to(device)
            
            with torch.no_grad():
                logits = model(x)
                _, preds = logits.topk(top_k, dim=1)
                pred_delta_ids = preds[0].cpu().tolist()
            
            # 转换为 block ID
            actual_block = miss_seq[i+1] if i+1 < len(miss_seq) else None
            if actual_block is None:
                continue
            
            current_block = miss_seq[i]
            pred_blocks = []
            for did in pred_delta_ids:
                if did == 0:  # UNK
                    continue
                delta = id_to_delta.get(did)
                if delta is not None:
                    pred_blocks.append(current_block + delta)
            
            total_misses += 1
            if actual_block in pred_blocks:
                predicted_hits += 1
    
    return predicted_hits, total_misses

hits, total = simulate_prefetch_block_accuracy(
    test_blocks, test_seqs, model, delta_to_id, id_to_delta, device, top_k=10)

print(f"  LSTM Block prediction (Top-10 delta): {hits}/{total} = {hits/max(1,total)*100:.1f}%")
print(f"  (Of all miss blocks, how many were correctly predicted by LSTM)")

# ============================================================
# 7. Summary
# ============================================================
print(f"\n=== 7. Summary ===")
print(f"  D4 baseline: 64 misses/query, 15.2ms I/O")
print(f"  Delta vocab: Top-{TOP_K} covers {covered/len(all_deltas)*100:.1f}% of deltas")
print(f"  Markov 1st: Top-10 = {top10_correct/total*100:.1f}%")
print(f"  Markov 2nd: Top-10 = {top10_2/total_2*100:.1f}%")
print(f"  LSTM:        Top-1 = {top1/total*100:.1f}%, Top-5 = {top5/total*100:.1f}%, Top-10 = {top10/total*100:.1f}%")
print(f"  Block prediction: {hits/max(1,total)*100:.1f}% of misses predictable")
print(f"")
print(f"  Expected improvement:")
lstm_hit_rate = hits / max(1, total)
misses_saved = 64 * lstm_hit_rate
io_saved = misses_saved * 0.237
new_latency = 34.9 - io_saved
print(f"    Misses saved/query: {misses_saved:.1f}")
print(f"    I/O saved/query: {io_saved:.1f}ms")
print(f"    New latency: {new_latency:.1f}ms ({new_latency/5.83:.1f}x full-memory)")
