#!/usr/bin/env python3
"""
导出 Markov 1st order 转移概率表为 JSON，供 C++ 加载使用

产出文件:
  markov_model.json - 转移概率表 (delta -> top-10 next deltas)
  markov_vocab.json - delta vocab (delta_value -> id, id -> delta_value)

用法:
  python3 export_markov.py [--top_k 0] [--trace_dir logs/traces_oracle] [--output_dir logs/markov_model]
"""
import argparse
import json
from collections import Counter, defaultdict
import os

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('--top_k', type=int, default=0, help='Vocab size (0=all)')
    p.add_argument('--trace_dir', type=str, default='logs/traces_oracle')
    p.add_argument('--output_dir', type=str, default='logs/markov_model')
    p.add_argument('--n_train', type=int, default=800)
    return p.parse_args()

args = parse_args()
os.makedirs(args.output_dir, exist_ok=True)

# 1. Load traces
print("Loading traces...", flush=True)
block_seqs = []
with open(f"{args.trace_dir}/block_sequences.txt") as f:
    for line in f:
        if line.startswith('#'): continue
        parts = line.strip().split()
        block_seqs.append([int(x) for x in parts[1:]])

# 2. Build deltas
query_deltas = []
for blocks in block_seqs:
    query_deltas.append([blocks[i+1] - blocks[i] for i in range(len(blocks)-1)])

all_d = [d for s in query_deltas for d in s]
dc = Counter(all_d)

# 3. Build vocab
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

print(f"Vocab: {VS}, Coverage: {cov:.1f}%", flush=True)

enc = [[d2i.get(d, 0) for d in s] for s in query_deltas]
n_tr = args.n_train
tr_s = enc[:n_tr]
te_s = enc[n_tr:]
tr_b = block_seqs[:n_tr]
te_b = block_seqs[n_tr:]

# 4. Build Markov 1st order: P(next_delta_id | current_delta_id)
print("Building Markov model...", flush=True)
m1 = defaultdict(Counter)
for s in tr_s:
    for i in range(len(s)-1):
        m1[s[i]][s[i+1]] += 1

# 5. Evaluate
print("\n=== Evaluation ===", flush=True)

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
print(f"  Delta: Top-1={t1/tot*100:.2f}%, Top-10={t10/tot*100:.2f}%", flush=True)

# Block prediction (multi-step)
for steps in [1, 3, 5, 10]:
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
    print(f"  Markov {steps}-step: {h/t*100:.2f}%", flush=True)

# 6. Export model to JSON
print(f"\nExporting model...", flush=True)

# Export vocab: {delta_value: id} and {id: delta_value}
vocab_data = {
    "top_k": args.top_k,
    "vocab_size": VS,
    "coverage": cov,
    "total_deltas": len(all_d),
    "unique_deltas": len(dc),
    "delta_to_id": {str(d): i for d, i in d2i.items()},  # JSON keys must be strings
    "id_to_delta": {str(i): d for i, d in i2d.items()},
}

# Export Markov transitions: {current_delta_id: [(next_delta_id, count), ...]}
# Only save Top-10 next deltas for each current delta (enough for prediction)
transitions = {}
for curr_id, next_counter in m1.items():
    top_next = next_counter.most_common(10)
    transitions[str(curr_id)] = [
        {"next_delta_id": nid, "next_delta_value": i2d.get(nid, 0), "count": cnt}
        for nid, cnt in top_next
    ]

markov_data = {
    "description": "Markov 1st order transition table for HNSW block delta prediction",
    "version": 1,
    "model_type": "markov_1st_order",
    "vocab": vocab_data,
    "transitions": transitions,
    "stats": {
        "total_train_deltas": sum(len(s) for s in tr_s),
        "total_test_deltas": sum(len(s) for s in te_s),
        "delta_top1_accuracy": t1 / tot,
        "delta_top10_accuracy": t10 / tot,
    },
}

# Save
vocab_path = os.path.join(args.output_dir, "markov_model.json")
with open(vocab_path, 'w') as f:
    json.dump(markov_data, f, indent=2)
print(f"  Saved to {vocab_path} ({os.path.getsize(vocab_path) / 1024:.1f} KB)", flush=True)

# Also save a simple C++ header-friendly format
cpp_path = os.path.join(args.output_dir, "markov_model.h")
with open(cpp_path, 'w') as f:
    f.write("// Auto-generated Markov 1st order model for HNSW delta prediction\n")
    f.write(f"// Vocab size: {VS}, Coverage: {cov:.1f}%\n")
    f.write(f"// Delta Top-10 accuracy: {t10/tot*100:.2f}%\n\n")
    f.write("#pragma once\n#include <cstdint>\n#include <vector>\n#include <unordered_map>\n\n")
    f.write("struct MarkovTransition {\n")
    f.write("    int32_t next_delta;     // delta value\n")
    f.write("    uint32_t count;         // transition count\n")
    f.write("};\n\n")
    f.write(f"// Vocab: {VS} entries (id 0 = UNK/pad)\n")
    f.write(f"static const int VOCAB_SIZE = {VS};\n\n")
    f.write("// delta_value -> id\n")
    f.write("static const std::unordered_map<int32_t, int32_t> DELTA_TO_ID = {\n")
    for d, i in sorted(d2i.items(), key=lambda x: x[1]):
        f.write(f"    {{{d}, {i}}},\n")
    f.write("};\n\n")
    f.write("// id -> delta_value\n")
    f.write("static const std::vector<int32_t> ID_TO_DELTA = {\n    0,\n")  # 0 = UNK
    for i in range(1, VS):
        f.write(f"    {i2d[i]},\n")
    f.write("};\n\n")
    f.write("// Markov transitions: current_delta_id -> Top-10 (next_delta_value, count)\n")
    f.write("static const std::unordered_map<int32_t, std::vector<MarkovTransition>> MARKOV_TABLE = {\n")
    for curr_id in sorted(m1.keys()):
        top_next = m1[curr_id].most_common(10)
        f.write(f"    {{{curr_id}, {{")
        f.write(", ".join(f"{{{i2d.get(nid, 0)}, {cnt}}}" for nid, cnt in top_next))
        f.write("}},\n")
    f.write("};\n")

print(f"  Saved C++ header to {cpp_path} ({os.path.getsize(cpp_path) / 1024:.1f} KB)", flush=True)

print(f"\n=== Done ===", flush=True)
print(f"  Model files in: {args.output_dir}/", flush=True)
print(f"  JSON:  {vocab_path}", flush=True)
print(f"  C++:   {cpp_path}", flush=True)
print(f"  To use in C++: #include \"markov_model.h\" then look up MARKOV_TABLE[delta_id]", flush=True)
