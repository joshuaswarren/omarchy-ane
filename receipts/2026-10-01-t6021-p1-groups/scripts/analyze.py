#!/usr/bin/env python3
"""P1Groups analysis (AfBridgeRun analyze.py, generalized): per-arm encoder / prog_020 / prog_006
minmin and medmed, correctness lines, and the pre-registered rule of each test arm against the
first arm (A0). Arms named A-after* get the 1% drift check instead of a verdict.

usage: analyze.py NAME=RUN_DIR ...   (first = A0)"""
import re
import statistics as st
import sys
from pathlib import Path

GOLD = "golden max_abs=0 relL2=0 exact=1"
SHA16 = "fca96f1355485ec3"


def stats(blocks):
    mins, meds = [b[0] for b in blocks], [b[1] for b in blocks]
    return dict(n=len(blocks), minmin=min(mins), medmed=st.median(meds), medmin=st.median(mins), maxmed=max(meds))


def arm(run):
    enc, bad = [], []
    for line in (run / "enc.summary").read_text().splitlines():
        m = re.match(r"enc (\S+) min ([\d.]+) median ([\d.]+) (golden .*) sha16 (\w+)", line)
        tag, mn, md, g, sha = m.groups()
        if g != GOLD or not sha.startswith(SHA16):
            bad.append(tag)
        if tag.startswith("b"):
            enc.append((float(mn), float(md)))
    q = {}
    for prog in ("prog_020", "prog_006"):
        rows = (run / "q" / f"{prog}.blocks").read_text().splitlines()
        q[prog] = stats([tuple(map(float, ln.split()[1:3])) for ln in rows])
    con = (run / "console.log").read_text()
    checks = [re.sub(r"WARNING unresolved port identity: \{.*?\} ", "", ln) for ln in con.splitlines()
              if ln.startswith(("gate ", "prog_020 ", "prog_006 ", "burst ", "dmesg new"))]
    return dict(enc=stats(enc), q=q, bad=bad, n_enc=len(enc) + 1, checks=checks)


def band(drop):
    if drop >= 0.15:
        return "SUPPORTED"
    if drop >= 0.03:
        return "PARTIAL"
    return "REJECTED"


def main():
    pairs = [a.split("=", 1) for a in sys.argv[1:]]
    res = {n: arm(Path(r)) for n, r in pairs}
    for (n, r), a in zip(pairs, res.values()):
        e = a["enc"]
        print(f"== {n} ({Path(r).name})")
        print(f"encoder: blocks {e['n']}, minmin {e['minmin']:.3f}, median of block mins {e['medmin']:.3f}, "
              f"medmed {e['medmed']:.3f}, worst block median {e['maxmed']:.3f} ms")
        for prog, s in a["q"].items():
            print(f"{prog}: blocks {s['n']}, minmin {s['minmin']:.3f}, medmed {s['medmed']:.3f} ms")
        print(f"encoder processes not bit-exact / wrong sha: {a['bad'] or 'none'} (of {a['n_enc']})")
        for c in a["checks"]:
            print("  " + c)
    a0name = pairs[0][0]
    a0 = res[a0name]
    for n, b in list(res.items())[1:]:
        if n.startswith("A-after"):
            for k in ("minmin", "medmed"):
                d = b["enc"][k] / a0["enc"][k] - 1
                print(f"== drift {n} vs {a0name}: encoder {k} {d * 100:+.3f}% ({'within' if abs(d) <= 0.01 else 'OUTSIDE'} 1%)")
            continue
        print(f"== decision {n} vs {a0name}")
        v = []
        for k in ("minmin", "medmed"):
            drop = 1 - b["enc"][k] / a0["enc"][k]
            v.append(band(drop))
            print(f"encoder {k}: {a0name} {a0['enc'][k]:.3f} -> {n} {b['enc'][k]:.3f} ms, drop {drop * 100:+.2f}% -> {v[-1]}")
        print(f"VERDICT {n}: {v[0] if v[0] == v[1] else f'unresolved (minmin {v[0]}, medmed {v[1]})'}"
              f"; absolute rule minmin <= 216 ms: {b['enc']['minmin'] <= 216}")
        for prog in ("prog_020", "prog_006"):
            print(f"{prog}: minmin ratio {n}/{a0name} {b['q'][prog]['minmin'] / a0['q'][prog]['minmin']:.4f}, "
                  f"medmed ratio {b['q'][prog]['medmed'] / a0['q'][prog]['medmed']:.4f}")


if __name__ == "__main__":
    main()
