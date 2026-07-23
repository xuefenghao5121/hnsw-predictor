#!/usr/bin/env python3
"""LSTM access-delta 预取原型 - 分步输出"""
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from collections import Counter, defaultdict
import sys

TRACE_DIR = "logs/traces_oracle"  # 全访问序列, not miss-only

# ============================================================
# 1. Load + encode
# ============================================================
print("=== 1. Loading oracle traces ===", flush=True)
block_seqs = []
with open(f"{TRACE_DIR}/block_sequences.txt") as f:
    for line in f:
        if line.startswith('#'): continue
        parts = line.strip().split()
        block_seqs.append([int(x) for x in parts[1:]])

query_deltas = []
for blocks in block_seqs:
    query_deltas.append([blocks[i+1] - blocks[i] for i in range(len(blocks)-1)])

all_d = [d for s in query_deltas for d in s]
dc = Counter(all_d)
all_unique = sorted(dc.keys())
d2i = {d: i+1 for i, d in enumerate(all_unique)}
i2d = {i+1: d for i, d in enumerate(all_unique)}
VS = len(all_unique) + 1

print(f"  Queries: {len(block_seqs)}, Deltas: {len(all_d)}, Vocab: {VS}", flush=True)
print(f"  Delta=0: {dc.get(0,0)/len(all_d)*100:.1f}%", flush=True)
for k in [10, 50, 100, 500]:
    print(f"  Top-{k} coverage: {sum(c for _,c in dc.most_common(k))/len(all_d)*100:.1f}%", flush=True)

enc = [[d2i[d] for d in s] for s in query_deltas]
n_tr = 800
tr_s, te_s = enc[:n_tr], enc[n_tr:]
tr_b, te_b = block_seqs[:n_tr], block_seqs[n_tr:]

# ============================================================
# 2. Markov 1st (CORRECT block prediction)
# ============================================================
print("\n=== 2. Markov 1st order ===", flush=True)
m1 = defaultdict(Counter)
for s in tr_s:
    for i in range(len(s)-1):
        m1[s[i]][s[i+1]] += 1

# Delta accuracy: predict ds[i+1] from ds[i]
t1, t10, tot = 0, 0, 0
for s in te_s:
    for i in range(len(s)-1):
        c, a = s[i], s[i+1]
        if c in m1:
            ps = [d for d,_ in m1[c].most_common(10)]
            if a in ps[:1]: t1 += 1
            if a in ps: t10 += 1
        tot += 1
print(f"  Delta: Top-1={t1/tot*100:.2f}%, Top-10={t10/tot*100:.2f}%", flush=True)

# CORRECT block prediction: use ds[i] to predict ds[i+1], then mb[i+1]+pred_delta = pred mb[i+2]
# Or: use ds[i-1] to predict ds[i], then mb[i]+pred_delta = pred mb[i+1]
hits, total = 0, 0
for mb, ds in zip(te_b, te_s):
    for i in range(1, len(mb)-1):
        prev_did = ds[i-1]   # delta that led to mb[i]
        actual_next = mb[i+1]  # block we want to predict
        total += 1
        if prev_did in m1:
            ps = [d for d,_ in m1[prev_did].most_common(10)]
            pbs = {mb[i] + i2d[d] for d in ps}
            if actual_next in pbs:
                hits += 1
print(f"  Block (Top-10): {hits}/{total} = {hits/total*100:.2f}%", flush=True)

# ============================================================
# 3. Multi-step: predict block k steps ahead
# ============================================================
print("\n=== 3. Multi-step block prediction ===", flush=True)
for steps in [1, 3, 5, 10]:
    h, t = 0, 0
    for mb, ds in zip(te_b, te_s):
        for i in range(1, len(mb)-steps):
            # Use ds[i-1] to predict ds[i], ds[i+1], ..., ds[i+steps-1]
            # Simple: predict next delta, assume it repeats for 'steps' steps
            prev_did = ds[i-1]
            actual = mb[i+steps]
            t += 1
            if prev_did in m1:
                ps = [d for d,_ in m1[prev_did].most_common(10)]
                # Method 1: predict mb[i+steps] = mb[i] + steps * predicted_delta
                pbs1 = {mb[i] + steps * i2d[d] for d in ps}
                # Method 2: predict mb[i+1], check if it's in future window
                pbs2 = {mb[i] + i2d[d] for d in ps}
                future_blks = set(mb[i+1:i+1+steps])
                if (actual in pbs1) or (pbs2 & future_blks):
                    h += 1
    print(f"  {steps}-step: {h/t*100:.2f}%", flush=True)

# ============================================================
# 4. LSTM Training
# ============================================================
print("\n=== 4. LSTM Training ===", flush=True)

SEQ_LEN = 16
BATCH = 1024
EPOCHS = 8

class DS(Dataset):
    def __init__(self, seqs, L=SEQ_LEN):
        self.s = []
        for s in seqs:
            for i in range(1, len(s)):
                c = s[max(0,i-L):i]
                self.s.append(([0]*(L-len(c))+c, s[i]))
    def __len__(self): return len(self.s)
    def __getitem__(self, i):
        return torch.tensor(self.s[i][0],dtype=torch.long), torch.tensor(self.s[i][1],dtype=torch.long)

class M(nn.Module):
    def __init__(self, vs):
        super().__init__()
        self.e = nn.Embedding(vs, 64, padding_idx=0)
        self.l = nn.LSTM(64, 128, 2, batch_first=True, dropout=0.1)
        self.f = nn.Linear(128, vs)
    def forward(self, x):
        o,_ = self.l(self.e(x))
        return self.f(o[:,-1,:])

tr_ds = DS(tr_s)
te_ds = DS(te_s)
print(f"  Train: {len(tr_ds)}, Test: {len(te_ds)}", flush=True)

tr_dl = DataLoader(tr_ds, BATCH, shuffle=True, num_workers=0)
te_dl = DataLoader(te_ds, BATCH, shuffle=False, num_workers=0)

dev = torch.device('cpu')
model = M(VS).to(dev)
opt = torch.optim.Adam(model.parameters(), 0.002)
crit = nn.CrossEntropyLoss()

for ep in range(EPOCHS):
    model.train()
    tl = 0
    nb = 0
    for x, y in tr_dl:
        x, y = x.to(dev), y.to(dev)
        opt.zero_grad()
        lo = crit(model(x), y)
        lo.backward()
        opt.step()
        tl += lo.item()
        nb += 1
    avg_loss = tl / nb
    
    if (ep+1) % 2 == 0:
        model.eval()
        a1, a5, a10, t = 0, 0, 0, 0
        with torch.no_grad():
            for x, y in te_dl:
                x, y = x.to(dev), y.to(dev)
                _, p = model(x).topk(10, dim=1)
                for i in range(y.size(0)):
                    ac = y[i].item()
                    pp = p[i].cpu().tolist()
                    if ac in pp[:1]: a1 += 1
                    if ac in pp[:5]: a5 += 1
                    if ac in pp: a10 += 1
                    t += 1
        print(f"  Ep{ep+1}: loss={avg_loss:.4f} Top-1={a1/t*100:.2f}% Top-5={a5/t*100:.2f}% Top-10={a10/t*100:.2f}%", flush=True)

# ============================================================
# 5. LSTM Block prediction
# ============================================================
print("\n=== 5. LSTM Block prediction ===", flush=True)
model.eval()

# 1-step: predict next block
h1, t1 = 0, 0
with torch.no_grad():
    for mb, ds in zip(te_b, te_s):
        for i in range(1, len(mb)):
            c = ds[max(0,i-SEQ_LEN):i]
            x = torch.tensor([[0]*(SEQ_LEN-len(c))+c], dtype=torch.long).to(dev)
            _, p = model(x).topk(10, dim=1)
            pids = p[0].cpu().tolist()
            pred_blks = {mb[i-1] + i2d[d] for d in pids if d > 0}
            if i < len(mb):
                if mb[i] in pred_blks: h1 += 1
                t1 += 1
print(f"  1-step: {h1/t1*100:.2f}% ({h1}/{t1})", flush=True)

# Multi-step: predict block k steps ahead (predict delta now, prefetch, check if ready later)
for steps in [3, 5, 10]:
    h, t = 0, 0
    with torch.no_grad():
        for mb, ds in zip(te_b, te_s):
            for i in range(1, len(mb)-steps):
                c = ds[max(0,i-SEQ_LEN):i]
                x = torch.tensor([[0]*(SEQ_LEN-len(c))+c], dtype=torch.long).to(dev)
                _, p = model(x).topk(10, dim=1)
                pids = p[0].cpu().tolist()
                # Predict next delta, prefetch mb[i-1]+delta now
                pred_blks = {mb[i-1] + i2d[d] for d in pids if d > 0}
                # Check if predicted block appears in next 'steps' blocks
                future = set(mb[i:i+steps])
                if pred_blks & future:
                    h += 1
                t += 1
    print(f"  {steps}-step window: {h/t*100:.2f}% ({h}/{t})", flush=True)

# ============================================================
# 6. Summary
# ============================================================
print(f"\n=== 6. Summary ===", flush=True)
print(f"  Markov 1st: Delta Top-10={t10/tot*100:.1f}%, Block={hits/total*100:.1f}%", flush=True)
print(f"  LSTM: Delta Top-10={a10/t*100:.1f}%, Block 1-step={h1/t1*100:.1f}%", flush=True)
print(f"  D4 baseline: 64 misses/q, 34.9ms, 6.0x", flush=True)
lstm_blk = h1/t1*100
print(f"  With LSTM prefetch (if {lstm_blk:.0f}% of misses predicted):", flush=True)
print(f"    Saves {64*lstm_blk/100:.0f} misses, {64*lstm_blk/100*0.237:.1f}ms", flush=True)
print(f"    New latency: {34.9-64*lstm_blk/100*0.237:.1f}ms ({(34.9-64*lstm_blk/100*0.237)/5.83:.1f}x)", flush=True)
