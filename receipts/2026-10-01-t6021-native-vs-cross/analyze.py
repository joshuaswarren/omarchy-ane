#!/usr/bin/env python3
"""NativeVsCross analysis: per-arm minmin / medmed, ratios, the pre-registered
decision rule, and the Qwen output comparison (N vs X bytes, each vs the M1
step-11 golden).

usage: analyze.py RUN_DIR GOLDEN_DIR   (RUN_DIR = copied run-*/, GOLDEN_DIR =
copied step11/ with prog_020/ and prog_006/)"""
import re
import statistics as st
import sys
from pathlib import Path

import numpy as np

MACOS_MS = 89.3


def arm_stats(blocks):
    mins = [b[0] for b in blocks]
    meds = [b[1] for b in blocks]
    return dict(n=len(blocks), minmin=min(mins), medmed=st.median(meds), medmin=st.median(mins),
                maxmin=max(mins), maxmed=max(meds))


def enc_blocks(run):
    arms = {"X": [], "N": []}
    golden, shas = [], set()
    for line in (run / "enc.summary").read_text().splitlines():
        m = re.match(r"enc (\S+) min ([\d.]+) median ([\d.]+) (golden .*) sha16 (\w+)", line)
        tag, mn, md, g, sha = m.groups()
        golden.append((tag, g))
        shas.add((tag[0], sha))
        if tag.startswith("w-"):
            continue
        arms[tag[0]].append((float(mn), float(md)))
    return arms, golden, shas


def band(minmin_n, ratio):
    if minmin_n <= 130:
        return "H_compile"
    if ratio >= 0.95:
        return "H_driver"
    return "mixed"


def main():
    run, gold = Path(sys.argv[1]), Path(sys.argv[2])
    arms, golden, shas = enc_blocks(run)
    x, n = arm_stats(arms["X"]), arm_stats(arms["N"])
    r, rmed = n["minmin"] / x["minmin"], n["medmed"] / x["medmed"]
    print("== encoder (ms per CALL)")
    for k, s in (("X", x), ("N", n)):
        print(f"{k}: blocks {s['n']}, minmin {s['minmin']:.3f}, median of block mins {s['medmin']:.3f}, "
              f"medmed {s['medmed']:.3f}, worst block min {s['maxmin']:.3f}, worst block median {s['maxmed']:.3f}")
    print(f"R = minmin(N)/minmin(X) = {r:.4f}; Rmed = {rmed:.4f}; minmin(X) - minmin(N) = {x['minmin'] - n['minmin']:.3f} ms")
    paired = [b[1] / a[1] for a, b in zip(arms["X"], arms["N"])]
    print(f"paired block-median ratios N/X (k-th X with k-th N): median {st.median(paired):.4f}, "
          f"range {min(paired):.4f}-{max(paired):.4f}")
    v_min, v_med = band(n["minmin"], r), band(n["medmed"], rmed)
    verdict = v_min if v_min == v_med else f"unresolved (minmin {v_min}, medmed {v_med})"
    print(f"decision rule: minmin -> {v_min}; medmed -> {v_med}; VERDICT {verdict}")
    gap = x["minmin"] - MACOS_MS
    print(f"split of the gap to macOS {MACOS_MS} ms ({gap:.1f} ms): compile share "
          f"{(x['minmin'] - n['minmin']) / gap:.4f}, driver/operating-point share {(n['minmin'] - MACOS_MS) / gap:.4f}")
    anchor = abs(x["minmin"] - 254.4) <= 1.5
    print(f"X anchor 254.4 +- 1.5: {'pass' if anchor else 'FAIL'}")
    bad = [t for t, g in golden if g != "golden max_abs=0 relL2=0 exact=1"]
    print(f"golden: {len(golden)} processes, not bit-exact: {bad or 'none'}; fp16 sha256 prefixes by arm: {sorted(shas)}")

    print("== Qwen (ms per CALL, 16 calls per block)")
    for prog in ("prog_020", "prog_006"):
        blk = {"X": [], "N": []}
        for line in (run / "q" / f"{prog}.blocks").read_text().splitlines():
            a, _, mn, md = line.split()
            blk[a].append((float(mn), float(md)))
        qx, qn = arm_stats(blk["X"]), arm_stats(blk["N"])
        print(f"{prog}: X blocks {qx['n']} minmin {qx['minmin']:.3f} medmed {qx['medmed']:.3f}; "
              f"N blocks {qn['n']} minmin {qn['minmin']:.3f} medmed {qn['medmed']:.3f}; "
              f"R {qn['minmin'] / qx['minmin']:.4f} Rmed {qn['medmed'] / qx['medmed']:.4f}")
        for f in sorted((run / "q" / f"{prog}-X").glob("*.f16")):
            fx = f.read_bytes()
            fn = (run / "q" / f"{prog}-N" / f.name).read_bytes()
            ax, an = np.frombuffer(fx, np.float16).astype(np.float32), np.frombuffer(fn, np.float16).astype(np.float32)
            g = gold / prog / f"out_{f.stem}.f16"
            line = f"  {f.stem}: {len(fx)} B, N==X bytes {fx == fn}"
            if g.exists():
                ag = np.fromfile(g, np.float16).astype(np.float32)
                if ag.size == ax.size:
                    nrm = np.linalg.norm(ag)
                    for k, a in (("X", ax), ("N", an)):
                        line += (f"; {k} vs golden max_abs {np.abs(a - ag).max():.6g} "
                                 f"relL2 {np.linalg.norm(a - ag) / nrm if nrm else float('nan'):.6g}")
                else:
                    line += f"; golden size {ag.size} != {ax.size}"
            print(line)


if __name__ == "__main__":
    main()
