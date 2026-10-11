"""Compare two resident-decode runs: float32 logits (the files hold 16 generation steps back to back) and the token stream."""
import json
import sys

import numpy as np

old_dir, new_dir = sys.argv[1], sys.argv[2]
V = 248320


def load(d):
    a = np.fromfile(f"{d}/logits.f32", dtype=np.float32)
    steps = {}
    for line in open(f"{d}/results.jsonl"):
        r = json.loads(line)
        if r.get("type") == "step" and "token_out" in r:
            steps[r["gen_index"]] = r
    return a, steps


a, sa = load(old_dir)
b, sb = load(new_dir)
print("logits floats", a.size, b.size, "vocab guess", a.size // 16 if a.size % 16 == 0 else "n/a")
n = min(a.size, b.size)
d = np.abs(a[:n] - b[:n])
print("bit-identical:", bool(np.array_equal(a[:n], b[:n])))
print("max abs diff", float(d.max()), "mean abs diff", float(d.mean()), "max rel (vs max|logit|)", float(d.max() / np.abs(a[:n]).max()))
print("differing floats", int((a[:n] != b[:n]).sum()), "of", n)
ta = [sa[k]["token_out"] for k in sorted(sa)]
tb = [sb[k]["token_out"] for k in sorted(sb)]
print("tokens old", ta)
print("tokens new", tb)
print("token streams equal:", ta == tb)
ra = [sa[k].get("ref_token") for k in sorted(sa)]
print("both equal the reference tokens:", ta == ra and tb == ra if all(x is not None for x in ra) else "no ref in jsonl")
m = [(sa[k]["margin"], sb[k]["margin"]) for k in sorted(sa) if k in sb and "margin" in sa[k]]
print("top1 margin old/new first 4:", [(round(x, 4), round(y, 4)) for x, y in m[:4]])
