#!/usr/bin/env python3
"""DartKernel analysis: per-arm encoder / prog_020 / prog_006 minmin and medmed
(AfBridgeRun method), correctness, and the pre-registered rule: the control
within 254 ms +-1%, the other arm against it (>= 15% supported, 3-15%
partial, < 3% rejected).

usage: analyze.py CONTROL_RUN OTHER_RUN   (ab-turn.sh run-*/ dirs, each with DONE)"""
import re
import statistics as st
import sys
from pathlib import Path

GOLD = "golden max_abs=0 relL2=0 exact=1"
SHA16 = "fca96f1355485ec3"
BASE, TOL = 254.0, 0.01


def stats(blocks):
    mins, meds = [b[0] for b in blocks], [b[1] for b in blocks]
    return dict(n=len(blocks), minmin=min(mins), medmed=st.median(meds))


def arm(run):
    enc, bad = [], []
    for line in (run / "enc.summary").read_text().splitlines():
        tag, mn, md, g, sha = re.match(r"enc (\S+) min ([\d.]+) median ([\d.]+) (golden .*) sha16 (\w+)", line).groups()
        if g != GOLD or not sha.startswith(SHA16):
            bad.append(tag)
        if tag.startswith("b"):
            enc.append((float(mn), float(md)))
    q = {p: stats([tuple(map(float, ln.split()[1:3])) for ln in (run / "q" / f"{p}.blocks").read_text().splitlines()])
         for p in ("prog_020", "prog_006")}
    checks = [re.sub(r"WARNING unresolved port identity: \{.*?\} ", "", ln)
              for ln in (run / "console.log").read_text().splitlines()
              if ln.startswith(("gate ", "prog_020 ", "prog_006 ", "burst ", "dmesg new"))]
    return dict(enc=stats(enc), q=q, bad=bad, done=(run / "DONE").exists(), checks=checks)


def band(drop):
    return "SUPPORTED" if drop >= 0.15 else "PARTIAL" if drop >= 0.03 else "REJECTED"


def main():
    runs = [Path(p) for p in sys.argv[1:3]]
    c, x = (arm(r) for r in runs)
    for r, a in zip(runs, (c, x)):
        e = a["enc"]
        print(f"== {r.name}: DONE {a['done']}, encoder blocks {e['n']}, minmin {e['minmin']:.3f}, medmed {e['medmed']:.3f} ms; "
              + ", ".join(f"{p} {s['minmin']:.3f} / {s['medmed']:.3f}" for p, s in a["q"].items())
              + f"; not bit-exact: {a['bad'] or 'none'}")
        for line in a["checks"]:
            print("  " + line[:200])
    ok = abs(c["enc"]["minmin"] / BASE - 1) <= TOL
    print(f"control: minmin {c['enc']['minmin']:.3f} vs {BASE} ms +-1%: {'PASS' if ok else 'FAIL'}")
    v = []
    for k in ("minmin", "medmed"):
        drop = 1 - x["enc"][k] / c["enc"][k]
        v.append(band(drop))
        print(f"encoder {k}: {c['enc'][k]:.3f} -> {x['enc'][k]:.3f} ms, drop {drop * 100:+.2f}% -> {v[-1]}")
    print(f"VERDICT {v[0] if v[0] == v[1] else f'unresolved (minmin {v[0]}, medmed {v[1]})'}"
          f"{'' if ok else ' (control FAILED: not comparable)'}")
    for p in ("prog_020", "prog_006"):
        print(f"{p}: other/control minmin {x['q'][p]['minmin'] / c['q'][p]['minmin']:.4f}, "
              f"medmed {x['q'][p]['medmed'] / c['q'][p]['medmed']:.4f}")


if __name__ == "__main__":
    main()
