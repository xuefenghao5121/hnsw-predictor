#!/usr/bin/env python3
"""
Markov 1st order delta 预取评估

用法:
  python3 markov_eval.py [options]

选项:
  --top_k 0           Vocab 大小 (0=全量)
  --n_train 800       训练查询数
  --trace_dir         轨迹目录
  --export_dir        导出模型目录 (空=不导出)

示例:
  python3 markov_eval.py --top_k 0
  python3 markov_eval.py --top_k 1000 --export_dir logs/markov_model
"""
import argparse, json, os
from collections import Counter, defaultdict

def parse_args():
    p = argparse.ArgumentParser(description='Markov 1st order delta prediction')
    p.add_argument('--top_k', type=int, default=0, help='Vocab size (0=all)')
    p.add_argument('--n_train', type=int, default=800)
    p.add_argument('--trace_dir', type=str, default='logs/traces_oracle')
    p.add_argument('--export_dir', type=str, default='', help='Export model dir')
    return p.parse_args()

args = parse_args()

def log(msg):
    print(msg, flush=True)

# 1. Load
log("=== 1. Loading traces ===")
block_seqs = []
with open(f"{args.trace_dir}/block_sequences.txt") as f:
    for line in f:
        if line.startswith('#'): continue
        block_seqs.append([int(x) for x in line.strip().split()[1:]])

query_deltas = []
for blocks in block_seqs:
    query_deltas.append([blocks[i+1] - blocks[i] for i in range(len(blocks)-1)])

all_d = [d for s in query_deltas for d in s]
dc = Counter(all_d)

# 2. Vocab
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

# 3. Build Markov
log("\n=== 2. Building Markov 1st order ===")
m1 = defaultdict(Counter)
for s in tr_s:
    for i in range(len(s)-1):
        m1[s[i]][s[i+1]] += 1

# 4. Evaluate
log("\n=== 3. Evaluation ===")

# Delta accuracy
t1, t10, tot = 0, 0, 0
for s in te_s:
    for i in range(len(s)-1):
        c, a = s[i], s[i+1]
        if c in m1:
            ps = [d for d, _ in m1[c].most_common(10)]
            if a in ps[:1]: t1 += 1
            if a in ps: t10 += 1
        tot += 1
log(f"  Delta: Top-1={t1/tot*100:.2f}%, Top-5={(sum(1 for s in te_s for i in range(len(s)-1) if s[i] in m1 and s[i+1] in [d for d,_ in m1[s[i]].most_common(5)])/tot)*100:.2f}%, Top-10={t10/tot*100:.2f}%")

# 2nd order Markov: P(next | current, previous)
log("\n  Building 2nd order...")
m2 = defaultdict(Counter)
for s in tr_s:
    for i in range(len(s)-2):
        m2[(s[i], s[i+1])][s[i+2]] += 1

# Delta accuracy (2nd order)
t1_2, t10_2, tot_2 = 0, 0, 0
for s in te_s:
    for i in range(len(s)-2):
        key = (s[i], s[i+1])
        a = s[i+2]
        if key in m2:
            ps = [d for d, _ in m2[key].most_common(10)]
            if a in ps[:1]: t1_2 += 1
            if a in ps: t10_2 += 1
        tot_2 += 1
log(f"  2nd Delta: Top-1={t1_2/tot_2*100:.2f}%, Top-10={t10_2/tot_2*100:.2f}%")

# Block prediction (multi-step, 2nd order)
for steps in [1, 3, 5, 10, 20]:
    h, t = 0, 0
    for mb, ds in zip(te_b, te_s):
        for i in range(2, len(mb)-steps):
            key = (ds[i-2], ds[i-1])
            t += 1
            if key in m2:
                ps = [d for d, _ in m2[key].most_common(10)]
                pbs1 = {mb[i] + steps * i2d[d] for d in ps if d > 0}
                pbs2 = {mb[i] + i2d[d] for d in ps if d > 0}
                future = set(mb[i:i+steps])
                if (mb[i+steps] in pbs1) or (pbs2 & future):
                    h += 1
    log(f"  2nd Block {steps:2d}-step: {h/t*100:.2f}%")

# Block prediction (multi-step)
for steps in [1, 3, 5, 10, 20]:
    h, t = 0, 0
    for mb, ds in zip(te_b, te_s):
        for i in range(1, len(mb)-steps):
            prev_did = ds[i-1]
            t += 1
            if prev_did in m1:
                ps = [d for d, _ in m1[prev_did].most_common(10)]
                pbs1 = {mb[i] + steps * i2d[d] for d in ps if d > 0}
                pbs2 = {mb[i] + i2d[d] for d in ps if d > 0}
                future = set(mb[i:i+steps])
                if (mb[i+steps] in pbs1) or (pbs2 & future):
                    h += 1
    log(f"  Block {steps:2d}-step: {h/t*100:.2f}%")

# 5. Expected improvement
log(f"\n=== 4. Expected Improvement ===")
# Use 10-step accuracy for estimation
h10, t10_blk = 0, 0
for mb, ds in zip(te_b, te_s):
    for i in range(1, len(mb)-10):
        prev_did = ds[i-1]
        t10_blk += 1
        if prev_did in m1:
            ps = [d for d, _ in m1[prev_did].most_common(10)]
            pbs1 = {mb[i] + 10 * i2d[d] for d in ps if d > 0}
            pbs2 = {mb[i] + i2d[d] for d in ps if d > 0}
            future = set(mb[i:i+10])
            if (mb[i+10] in pbs1) or (pbs2 & future):
                h10 += 1
acc10 = h10 / t10_blk * 100
log(f"  10-step accuracy: {acc10:.1f}%")
log(f"  D4 baseline: 64 misses/q, 34.9ms (6.0x)")
log(f"  With Markov prefetch ({acc10:.0f}% coverage):")
log(f"    Saves: 64 * {acc10/100:.2f} = {64*acc10/100:.0f} misses")
log(f"    I/O saved: {64*acc10/100*0.237:.1f}ms")
log(f"    New latency: {34.9 - 64*acc10/100*0.237:.1f}ms ({(34.9 - 64*acc10/100*0.237)/5.83:.1f}x)")

# 6. Export
if args.export_dir:
    os.makedirs(args.export_dir, exist_ok=True)
    log(f"\n=== 5. Exporting model ===")

    # JSON
    transitions = {}
    for curr_id, next_counter in m1.items():
        transitions[str(curr_id)] = [
            {"delta": i2d.get(nid, 0), "count": cnt}
            for nid, cnt in next_counter.most_common(10)
        ]

    model = {
        "model_type": "markov_1st_order",
        "vocab_size": VS,
        "coverage": cov,
        "delta_to_id": {str(d): i for d, i in d2i.items()},
        "id_to_delta": {str(i): d for i, d in i2d.items()},
        "transitions": transitions,
        "accuracy": {"delta_top1": t1/tot, "delta_top10": t10/tot},
    }

    json_path = os.path.join(args.export_dir, "markov_model.json")
    with open(json_path, 'w') as f:
        json.dump(model, f, indent=2)
    log(f"  JSON: {json_path} ({os.path.getsize(json_path)/1024:.1f} KB)")

    # C++ header
    h_path = os.path.join(args.export_dir, "markov_model.h")
    with open(h_path, 'w') as f:
        f.write(f"// Auto-generated Markov model. Vocab={VS}, Coverage={cov:.1f}%\n")
        f.write("#pragma once\n#include <cstdint>\n#include <vector>\n#include <unordered_map>\n\n")
        f.write("struct MarkovEntry { int32_t delta; uint32_t count; };\n\n")
        f.write(f"static const int VOCAB_SIZE = {VS};\n\n")
        f.write("static const std::unordered_map<int32_t,int32_t> DELTA_TO_ID = {\n")
        for d, i in sorted(d2i.items(), key=lambda x: x[1]):
            f.write(f"  {{{d},{i}}},\n")
        f.write("};\n\nstatic const std::vector<int32_t> ID_TO_DELTA = {\n  0,\n")
        for i in range(1, VS):
            f.write(f"  {i2d[i]},\n")
        f.write("};\n\nstatic const std::unordered_map<int32_t,std::vector<MarkovEntry>> MARKOV = {\n")
        for curr_id in sorted(m1.keys()):
            top = m1[curr_id].most_common(10)
            entries = ", ".join(f"{{{i2d.get(n,0)},{c}}}" for n, c in top)
            f.write(f"  {{{curr_id},{{{entries}}}}},\n")
        f.write("};\n")
    log(f"  C++:  {h_path} ({os.path.getsize(h_path)/1024:.1f} KB)")
