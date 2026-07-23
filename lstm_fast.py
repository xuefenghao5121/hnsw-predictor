#!/usr/bin/env python3
"""LSTM miss-Delta 预取原型 - 快速版"""
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from collections import Counter, defaultdict
import sys

TRACE_DIR = "logs/traces_c1024"
TOP_K_VOCAB = 500  # Top-500 deltas + UNK
SEQ_LEN = 16
BATCH_SIZE = 1024
EPOCHS = 10

# 1. Load miss sequences
print("=== 1. Loading ===")
query_miss = []
with open(f"{TRACE_DIR}/block_traces.txt") as f:
    cur_q, cur_m = -1, []
    for line in f:
        if line.startswith('#'): continue
        p = line.strip().split()
        q, b, h = int(p[0]), int(p[1]), int(p[2])
        if q != cur_q:
            if cur_q >= 0: query_miss.append(cur_m)
            cur_q, cur_m = q, []
        if not h: cur_m.append(b)
    if cur_q >= 0: query_miss.append(cur_m)

print(f"  Queries: {len(query_miss)}, Avg misses/q: {np.mean([len(s) for s in query_miss]):.0f}")

# 2. Build deltas + vocab
print("=== 2. Deltas ===")
query_deltas = []
for ms in query_miss:
    query_deltas.append([ms[i+1]-ms[i] for i in range(len(ms)-1)])

all_d = [d for s in query_deltas for d in s]
dc = Counter(all_d)
mc = dc.most_common(TOP_K_VOCAB)
d2i = {d: i+1 for i, (d, _) in enumerate(mc)}  # 0=UNK/pad
i2d = {i+1: d for i, (d, _) in enumerate(mc)}
VS = TOP_K_VOCAB + 1
cov = sum(c for _, c in mc) / len(all_d) * 100
print(f"  Deltas: {len(all_d)}, Vocab: {VS}, Coverage: {cov:.1f}%")

enc = [[d2i.get(d, 0) for d in s] for s in query_deltas]

# 3. Split
n_tr = 800
tr_s, te_s = enc[:n_tr], enc[n_tr:]
tr_b, te_b = query_miss[:n_tr], query_miss[n_tr:]
print(f"  Train: {sum(len(s) for s in tr_s)}, Test: {sum(len(s) for s in te_s)}")

# 4. Markov
print("=== 3. Markov ===")
m1 = defaultdict(Counter)
for s in tr_s:
    for i in range(len(s)-1): m1[s[i]][s[i+1]] += 1

# Delta-level accuracy
t1,t10,tot = 0,0,0
for s in te_s:
    for i in range(len(s)-1):
        c,a = s[i],s[i+1]
        if c in m1:
            ps = [d for d,_ in m1[c].most_common(10)]
            if a in ps[:1]: t1+=1
            if a in ps: t10+=1
        tot+=1
print(f"  1st: Top-1={t1/tot*100:.1f}%, Top-10={t10/tot*100:.1f}%")

# Block-level: predict next miss block from current miss block + predicted delta
# Only count non-UNK predictions (the ones that actually prefetch a specific block)
hits_nz, total_nz, unk_rate = 0, 0, 0
for mb, ds in zip(te_b, te_s):
    for i in range(len(mb)-1):
        curr_blk = mb[i]
        actual_blk = mb[i+1]
        did = ds[i]
        if did == 0:  # actual delta is UNK
            unk_rate += 1
            total_nz += 1
            continue
        total_nz += 1
        if did in m1:
            ps = [d for d,_ in m1[did].most_common(10)]
            pred_blks = set()
            for pd in ps:
                if pd > 0:
                    pred_blks.add(curr_blk + i2d[pd])
            if actual_blk in pred_blks:
                hits_nz += 1

blk_acc = hits_nz / max(1, total_nz) * 100
print(f"  Block prediction (Top-10): {hits_nz}/{total_nz} = {blk_acc:.1f}%")
print(f"  Actual delta is UNK rate: {unk_rate/total_nz*100:.1f}%")
print(f"  Effective block coverage: {hits_nz/total_nz*100:.1f}% of all miss transitions")

# 5. LSTM
print("=== 4. LSTM ===")
class DS(Dataset):
    def __init__(self, seqs, L=SEQ_LEN):
        self.s = []
        for s in seqs:
            for i in range(1, len(s)):
                c = s[max(0,i-L):i]
                self.s.append(([0]*(L-len(c))+c, s[i]))
    def __len__(self): return len(self.s)
    def __getitem__(self, i):
        return torch.tensor(self.s[i][0], dtype=torch.long), torch.tensor(self.s[i][1], dtype=torch.long)

class M(nn.Module):
    def __init__(self, vs):
        super().__init__()
        self.e = nn.Embedding(vs, 64, padding_idx=0)
        self.l = nn.LSTM(64, 128, 2, batch_first=True, dropout=0.1)
        self.f = nn.Linear(128, vs)
    def forward(self, x):
        o,_ = self.l(self.e(x))
        return self.f(o[:,-1,:])

tr_ds, te_ds = DS(tr_s), DS(te_s)
print(f"  Samples: train={len(tr_ds)}, test={len(te_ds)}")
tr_dl = DataLoader(tr_ds, BATCH_SIZE, shuffle=True)
te_dl = DataLoader(te_ds, BATCH_SIZE)

dev = torch.device('cpu')
model = M(VS).to(dev)
opt = torch.optim.Adam(model.parameters(), 0.001)
crit = nn.CrossEntropyLoss()

for ep in range(EPOCHS):
    model.train()
    tl = 0
    for x, y in tr_dl:
        x, y = x.to(dev), y.to(dev)
        opt.zero_grad()
        lo = crit(model(x), y)
        lo.backward()
        opt.step()
        tl += lo.item() * x.size(0)
    if (ep+1) % 2 == 0:
        model.eval()
        a1,a10,t = 0,0,0
        with torch.no_grad():
            for x, y in te_dl:
                x, y = x.to(dev), y.to(dev)
                _, p = model(x).topk(10, dim=1)
                for i in range(y.size(0)):
                    ac = y[i].item()
                    pp = p[i].cpu().tolist()
                    if ac in pp[:1]: a1+=1
                    if ac in pp: a10+=1
                    t+=1
        print(f"  Ep{ep+1}: loss={tl/len(tr_ds):.4f} Top-1={a1/t*100:.1f}% Top-10={a10/t*100:.1f}%")

# 6. LSTM block prediction
print("=== 5. LSTM Block Prediction ===")
model.eval()
h_l, t_l = 0, 0
with torch.no_grad():
    for mb, ds in zip(te_b, te_s):
        for i in range(1, len(mb)):
            c = ds[max(0,i-SEQ_LEN):i]
            x = torch.tensor([[0]*(SEQ_LEN-len(c))+c], dtype=torch.long).to(dev)
            _, p = model(x).topk(10, dim=1)
            pids = p[0].cpu().tolist()
            cb = mb[i-1]
            ab = mb[i]
            pbs = {cb + i2d[d] for d in pids if d > 0}
            if ab in pbs: h_l += 1
            t_l += 1

lstm_acc = h_l / max(1, t_l) * 100
print(f"  LSTM Block (Top-10): {h_l}/{t_l} = {lstm_acc:.1f}%")
print(f"  Markov Block (Top-10): {blk_acc:.1f}%")

# 7. Summary
print(f"\n=== 6. Summary ===")
print(f"  D4 baseline: 64 misses/q, 34.9ms, 6.0x")
print(f"  Markov: Block={blk_acc:.1f}%, saves {64*blk_acc/100:.0f} misses, {64*blk_acc/100*0.237:.1f}ms -> {34.9-64*blk_acc/100*0.237:.1f}ms ({(34.9-64*blk_acc/100*0.237)/5.83:.1f}x)")
print(f"  LSTM:   Block={lstm_acc:.1f}%, saves {64*lstm_acc/100:.0f} misses, {64*lstm_acc/100*0.237:.1f}ms -> {34.9-64*lstm_acc/100*0.237:.1f}ms ({(34.9-64*lstm_acc/100*0.237)/5.83:.1f}x)")
