#!/usr/bin/env python3
"""AfBridgeRun analysis: per-arm encoder / prog_020 / prog_006 minmin and medmed
(NativeVsCross method), correctness lines, and the pre-registered decision rule
(af arm vs the default arm).

usage: analyze.py DEFAULT_RUN AF_RUN [DEFAULT2_RUN]   (copied run-*/ dirs)"""
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
        q[prog] = stats([tuple(map(float, ln.split()[1:3])) for ln in (run / "q" / f"{prog}.blocks").read_text().splitlines()])
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
    runs = [Path(p) for p in sys.argv[1:]]
    names = ["default", "af_bridge_macos=1", "default (S4)"][: len(runs)]
    res = {n: arm(r) for n, r in zip(names, runs)}
    for n, a in res.items():
        e = a["enc"]
        print(f"== {n} ({runs[names.index(n)].name})")
        print(f"encoder: blocks {e['n']}, minmin {e['minmin']:.3f}, median of block mins {e['medmin']:.3f}, "
              f"medmed {e['medmed']:.3f}, worst block median {e['maxmed']:.3f} ms")
        for prog, s in a["q"].items():
            print(f"{prog}: blocks {s['n']}, minmin {s['minmin']:.3f}, medmed {s['medmed']:.3f} ms")
        print(f"encoder processes not bit-exact / wrong sha: {a['bad'] or 'none'} (of {a['n_enc']})")
        for c in a["checks"]:
            print("  " + c)
    d, b = res["default"], res["af_bridge_macos=1"]
    print("== decision (af vs default, same chain)")
    for k in ("minmin", "medmed"):
        drop = 1 - b["enc"][k] / d["enc"][k]
        print(f"encoder {k}: default {d['enc'][k]:.3f} -> af {b['enc'][k]:.3f} ms, drop {drop * 100:+.2f}% -> {band(drop)}")
    v = [band(1 - b["enc"][k] / d["enc"][k]) for k in ("minmin", "medmed")]
    print(f"VERDICT {v[0] if v[0] == v[1] else f'unresolved (minmin {v[0]}, medmed {v[1]})'}"
          f"; absolute rule minmin <= 216 ms: {b['enc']['minmin'] <= 216}")
    for prog in ("prog_020", "prog_006"):
        print(f"{prog}: minmin ratio af/default {b['q'][prog]['minmin'] / d['q'][prog]['minmin']:.4f}, "
              f"medmed ratio {b['q'][prog]['medmed'] / d['q'][prog]['medmed']:.4f}")
    if "default (S4)" in res:
        s4 = res["default (S4)"]
        print(f"default S4 vs default S2: encoder minmin {s4['enc']['minmin'] / d['enc']['minmin']:.4f}, "
              f"medmed {s4['enc']['medmed'] / d['enc']['medmed']:.4f}")


if __name__ == "__main__":
    main()
