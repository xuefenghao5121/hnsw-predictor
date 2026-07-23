#!/usr/bin/env python3
"""
LSTM delta 预取原型训练与评估

用法:
  python3 lstm_final.py [options]

选项:
  --top_k 0           Vocab 大小 (0=全量)
  --epochs 10         训练轮数
  --seq_len 32        序列长度
  --batch 512         Batch size
  --embed 64          Embedding 维度
  --hidden 128        LSTM hidden 维度
  --layers 2          LSTM 层数
  --lr 0.003          学习率
  --max_train 0       最大训练样本数 (0=全量)
  --max_test 0        最大测试样本数 (0=全量)
  --n_train 800       训练查询数
  --eval_queries 50   Block prediction 评估查询数
  --trace_dir         轨迹目录

示例:
  # 全量 vocab + 全量数据
  python3 lstm_final.py --top_k 0 --epochs 10 --max_train 0

  # Top-1000 vocab
  python3 lstm_final.py --top_k 1000 --epochs 10 --embed 64 --hidden 128
"""
import argparse
import numpy as np
import torch
import torch.nn as nn
from collections import Counter

def parse_args():
    p = argparse.ArgumentParser(description='LSTM Delta Prefetch')
    p.add_argument('--top_k', type=int, default=0, help='Vocab size (0=all)')
    p.add_argument('--epochs', type=int, default=10)
    p.add_argument('--seq_len', type=int, default=32)
    p.add_argument('--batch', type=int, default=512)
    p.add_argument('--embed', type=int, default=64)
    p.add_argument('--hidden', type=int, default=128)
    p.add_argument('--layers', type=int, default=2)
    p.add_argument('--lr', type=float, default=0.003)
    p.add_argument('--max_train', type=int, default=0, help='0=all')
    p.add_argument('--max_test', type=int, default=0, help='0=all')
    p.add_argument('--n_train', type=int, default=800)
    p.add_argument('--eval_queries', type=int, default=50)
    p.add_argument('--trace_dir', type=str, default='logs/traces_oracle')
    return p.parse_args()

args = parse_args()

def log(msg):
    print(msg, flush=True)

# ============================================================
# 1. Load + encode
# ============================================================
log("=== Config ===")
log(f"  top_k={args.top_k}, epochs={args.epochs}, seq_len={args.seq_len}")
log(f"  embed={args.embed}, hidden={args.hidden}, layers={args.layers}")
log(f"  batch={args.batch}, lr={args.lr}")
log(f"  max_train={args.max_train}, max_test={args.max_test}")

log("\n=== 1. Loading traces ===")
block_seqs = []
with open(f"{args.trace_dir}/block_sequences.txt") as f:
    for line in f:
        if line.startswith('#'): continue
        parts = line.strip().split()
        block_seqs.append([int(x) for x in parts[1:]])

query_deltas = []
for blocks in block_seqs:
    query_deltas.append([blocks[i+1] - blocks[i] for i in range(len(blocks)-1)])

all_d = [d for s in query_deltas for d in s]
dc = Counter(all_d)

# Build vocab
if args.top_k == 0:
    all_unique = sorted(dc.keys())
    d2i = {d: i+1 for i, d in enumerate(all_unique)}
    i2d = {i+1: d for i, d in enumerate(all_unique)}
    VS = len(all_unique) + 1
    cov = 100.0
else:
    mc = dc.most_common(args.top_k)
    d2i = {d: i+1 for i, (d, _) in enumerate(mc)}
    i2d = {i+1: d for i, (d, _) in enumerate(mc)}
    VS = args.top_k + 1
    cov = sum(c for _, c in mc) / len(all_d) * 100

log(f"  Queries: {len(block_seqs)}, Deltas: {len(all_d)}, Vocab: {VS}, Coverage: {cov:.1f}%")

enc = [[d2i.get(d, 0) for d in s] for s in query_deltas]
n_tr = args.n_train
tr_s, te_s = enc[:n_tr], enc[n_tr:]
tr_b, te_b = block_seqs[:n_tr], block_seqs[n_tr:]

# ============================================================
# 2. Build training data
# ============================================================
log("\n=== 2. Building training data ===")
SEQ_LEN = args.seq_len

X_tr, Y_tr = [], []
for s in tr_s:
    for i in range(1, len(s)):
        c = s[max(0, i-SEQ_LEN):i]
        X_tr.append([0]*(SEQ_LEN-len(c)) + c)
        Y_tr.append(s[i])
        if args.max_train > 0 and len(X_tr) >= args.max_train:
            break
    if args.max_train > 0 and len(X_tr) >= args.max_train:
        break

X_tr = torch.tensor(X_tr, dtype=torch.long)
Y_tr = torch.tensor(Y_tr, dtype=torch.long)
log(f"  Train: {len(X_tr)}")

X_te, Y_te = [], []
for s in te_s:
    for i in range(1, len(s)):
        c = s[max(0, i-SEQ_LEN):i]
        X_te.append([0]*(SEQ_LEN-len(c)) + c)
        Y_te.append(s[i])
        if args.max_test > 0 and len(X_te) >= args.max_test:
            break
    if args.max_test > 0 and len(X_te) >= args.max_test:
        break

X_te = torch.tensor(X_te, dtype=torch.long)
Y_te = torch.tensor(Y_te, dtype=torch.long)
log(f"  Test: {len(X_te)}")

# ============================================================
# 3. LSTM Model
# ============================================================
log(f"\n=== 3. LSTM (embed={args.embed}, hidden={args.hidden}, layers={args.layers}) ===")

class DeltaLSTM(nn.Module):
    def __init__(self, vs, embed_dim, hidden_dim, num_layers):
        super().__init__()
        self.embedding = nn.Embedding(vs, embed_dim, padding_idx=0)
        self.lstm = nn.LSTM(embed_dim, hidden_dim, num_layers, batch_first=True, dropout=0.1)
        self.fc = nn.Linear(hidden_dim, vs)
    def forward(self, x):
        emb = self.embedding(x)
        out, _ = self.lstm(emb)
        return self.fc(out[:, -1, :])

model = DeltaLSTM(VS, args.embed, args.hidden, args.layers)
opt = torch.optim.Adam(model.parameters(), args.lr)
crit = nn.CrossEntropyLoss()

n_params = sum(p.numel() for p in model.parameters())
log(f"  Parameters: {n_params:,}")

# ============================================================
# 4. Training
# ============================================================
log(f"\n=== 4. Training ({args.epochs} epochs) ===")
BATCH = args.batch

for ep in range(args.epochs):
    model.train()
    perm = torch.randperm(len(X_tr))
    tl, nb = 0, 0
    for i in range(0, len(X_tr), BATCH):
        idx = perm[i:i+BATCH]
        opt.zero_grad()
        lo = crit(model(X_tr[idx]), Y_tr[idx])
        lo.backward()
        opt.step()
        tl += lo.item()
        nb += 1

    # Eval
    model.eval()
    a1, a5, a10, t = 0, 0, 0, 0
    with torch.no_grad():
        for i in range(0, len(X_te), BATCH):
            logits = model(X_te[i:i+BATCH])
            _, p = logits.topk(10, dim=1)
            y = Y_te[i:i+BATCH]
            a1 += (p[:, :1] == y.unsqueeze(1)).any(dim=1).sum().item()
            a5 += (p[:, :5] == y.unsqueeze(1)).any(dim=1).sum().item()
            a10 += (p == y.unsqueeze(1)).any(dim=1).sum().item()
            t += y.size(0)
    log(f"  Ep{ep+1}: loss={tl/nb:.4f} Top-1={a1/t*100:.2f}% Top-5={a5/t*100:.2f}% Top-10={a10/t*100:.2f}%")

# ============================================================
# 5. Block prediction
# ============================================================
log(f"\n=== 5. Block Prediction ===")
model.eval()
n_eval = min(args.eval_queries, len(te_b))

# 1-step
h1, t1 = 0, 0
with torch.no_grad():
    for mb, ds in zip(te_b[:n_eval], te_s[:n_eval]):
        for i in range(1, len(mb)):
            c = ds[max(0, i-SEQ_LEN):i]
            x = torch.tensor([[0]*(SEQ_LEN-len(c)) + c], dtype=torch.long)
            _, p = model(x).topk(10, dim=1)
            pids = p[0].tolist()
            pred_blks = {mb[i-1] + i2d[d] for d in pids if d > 0}
            if mb[i] in pred_blks: h1 += 1
            t1 += 1
log(f"  LSTM 1-step: {h1/t1*100:.2f}% ({h1}/{t1})")

# Multi-step
for steps in [3, 5, 10, 20]:
    h, t = 0, 0
    with torch.no_grad():
        for mb, ds in zip(te_b[:n_eval], te_s[:n_eval]):
            for i in range(1, len(mb)-steps):
                c = ds[max(0, i-SEQ_LEN):i]
                x = torch.tensor([[0]*(SEQ_LEN-len(c)) + c], dtype=torch.long)
                _, p = model(x).topk(10, dim=1)
                pids = p[0].tolist()
                pred_blks = {mb[i-1] + i2d[d] for d in pids if d > 0}
                future = set(mb[i:i+steps])
                if pred_blks & future: h += 1
                t += 1
    log(f"  LSTM {steps:2d}-step: {h/t*100:.2f}%")

# ============================================================
# 6. Summary
# ============================================================
log(f"\n=== Summary ===")
log(f"  Config: top_k={args.top_k} (cov={cov:.1f}%), epochs={args.epochs}, embed={args.embed}, hidden={args.hidden}, seq_len={args.seq_len}")
log(f"  Train: {len(X_tr)}, Test: {len(X_te)}, Params: {n_params:,}")
log(f"  LSTM: Top-1 delta={a1/t*100:.1f}%, Top-10 delta={a10/t*100:.1f}%, Block 1-step={h1/t1*100:.1f}%")
