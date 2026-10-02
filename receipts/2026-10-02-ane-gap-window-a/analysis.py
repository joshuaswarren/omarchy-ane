#!/usr/bin/env python3
"""GapWinA analysis: baseline, E1 rates, sweep table + correlation, E2 ratios.
Usage: analysis.py <gapwin-runs-root>   (a copy of M2:/var/tmp/gapwin/runs)
Writes summary to stdout; every number traces to the copied run files."""
import glob
import os
import re
import statistics
import sys

root = sys.argv[1]


def blocks(path):
    """(mins, medians, flagged) from a blocks.tsv: k min median [gate] golden"""
    mins, meds, bad = [], [], []
    for ln in open(path, errors="replace"):
        f = ln.rstrip("\n").split("\t")
        if len(f) < 3:
            continue
        try:
            mn, md = float(f[1]), float(f[2])
        except ValueError:
            continue
        mins.append(mn)
        meds.append(md)
        if any("NOT-IDLE" in x for x in f[3:4]):
            bad.append(f[0])
    return mins, meds, bad


def mm(p):
    mins, meds, bad = blocks(p)
    return (
        min(mins) if mins else None,
        statistics.median(meds) if meds else None,
        len(mins),
        bad,
    )


print("== baseline")
b = sorted(glob.glob(f"{root}/s0-*/blocks.tsv"))
b = [x for x in b if open(x).read(1)]
if b:
    mn, md, n, bad = mm(b[0])
    print(f"min-of-min {mn} median-of-medians {md} blocks {n} flagged {bad}")
    print(f"settle-corrected min {mn - 1.05:.3f} med {md - 1.05:.3f} (call_settle_us ~1000-1100)")

print("== E1")
for prog, mac in (("p6_conv64", 8.59e9), ("p7_add32m", None)):
    p = sorted(glob.glob(f"{root}/s1-*/{prog}.blocks.tsv"))
    p = [x for x in p if open(x).read(1)]
    if not p:
        continue
    mn, md, n, bad = mm(p[0])
    corr = mn - 1.05
    if mac:
        print(f"{prog}: min {mn} med {md} blocks {n} flagged {bad} corrected-min {corr:.3f} ms "
              f"-> {mac / (corr / 1e3) / 1e12:.3f} TMAC/s (8.59e9 MAC)")
    else:
        gbs = 96 * 2**20 / (corr / 1e3) / 1e9
        print(f"{prog}: min {mn} med {md} blocks {n} flagged {bad} corrected-min {corr:.3f} ms "
              f"-> {gbs:.1f} GB/s (96 MiB moved)")

print("== cpufreq sweep")
pts = sorted(glob.glob(f"{root}/s2-*/points.tsv"))
pts = [x for x in pts if os.path.exists(os.path.join(os.path.dirname(x), "DONE"))]
if pts:
    pts = [pts[-1]]
if pts:
    targets = [ln.split() for ln in open(pts[0])]
    rows = []
    for i, (pf, ef) in enumerate(targets):
        bb = sorted(glob.glob(os.path.dirname(pts[0]) + f"/blocks_{i}.tsv"))
        ff = sorted(glob.glob(os.path.dirname(pts[0]) + f"/freq{i}.tsv"))
        if not bb:
            continue
        mn, md, n, bad = mm(bb[0])
        freqs = {"p0": [], "p4": [], "p8": []}
        if ff:
            for ln in open(ff[0]):
                f = ln.split()
                for j, k in enumerate(("p0", "p4", "p8")):
                    freqs[k].append(int(f[j + 1]))
        med = {k: statistics.median(v) / 1000 for k, v in freqs.items()}
        rows.append((int(pf), mn, md, med, bad, n))
        print(f"P={pf:>7} E={ef:>7} enc-min {mn:>8} enc-med {md:>8} "
              f"meas-MHz p0 {med['p0']:.0f} p4 {med['p4']:.0f} p8 {med['p8']:.0f} blocks {n} flagged {bad}")
    if len(rows) >= 3:
        xs = [r[3]["p4"] for r in rows]
        ys = [r[2] for r in rows]
        mx, my = statistics.mean(xs), statistics.mean(ys)
        cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        vx = sum((x - mx) ** 2 for x in xs) ** 0.5
        vy = sum((y - my) ** 2 for y in ys) ** 0.5
        print(f"Pearson r(enc-median, p4-freq) = {cov / (vx * vy):+.3f} n={len(rows)}")
        lo, hi = rows[0], rows[-1]
        print(f"lowest/highest ratio (enc-median) = {lo[2] / hi[2]:.4f}  (min {lo[1]}/{hi[1]:.4f})")
        print(f"decision: {'SUPPORTED (<=0.95)' if lo[2] / hi[2] <= 0.95 else 'REJECTED (>=0.98)' if lo[2] / hi[2] >= 0.98 else 'UNRESOLVED'}")

print("== E2")
import re as _re
for f in sorted(glob.glob(f"{root}/s3-*/console.log")):
    if not os.path.exists(os.path.join(os.path.dirname(f), "DONE")):
        continue
    arms = {}
    order = []
    for ln in open(f, errors="replace"):
        m = _re.match(r"== arm ([AB]) ", ln)
        if m:
            cur = m.group(1)
            arms.setdefault(cur, [])
            order.append(cur)
        m = _re.match(r"(enc-[AB])\t(\d+)\t([\d.]+)\t([\d.]+)\t([\d.]+)\t([\d.]+)", ln)
        if m and cur:
            arms[cur].append((m.group(1), float(m.group(4))))
        m = _re.match(r"(mv-[AB])\t(\d+)\t([\d.]+)\t([\d.]+)\t", ln)
        if m and cur:
            arms[cur].append((m.group(1), float(m.group(4))))
    for kind in ("enc", "mv"):
        vals = {}
        for arm in order:
            vs = [v for (t, v) in arms.get(arm, []) if t == f"{kind}-{arm}"]
            if vs:
                vals.setdefault(arm, []).append(statistics.median(vs))
        if vals.get("A") and vals.get("B"):
            print(f"{kind}: A arm-medians {[round(v,3) for v in vals['A']]} B arm-medians {[round(v,3) for v in vals['B']]}")
            pairs = [vals["B"][i] / vals["A"][i] for i in range(min(len(vals["A"]), len(vals["B"])))]
            r = statistics.median(vals["B"]) / statistics.median(vals["A"])
            print(f"{kind}: B/A per pair {[round(x,4) for x in pairs]} overall {r:.4f} "
                  f"-> {'SUPPORTED (>=1.15)' if r >= 1.15 else 'REJECTED (<=1.03)' if r <= 1.03 else 'UNRESOLVED'}")
    for h in sorted(glob.glob(os.path.dirname(f) + "/hog-*.log")):
        lines = [ln.split() for ln in open(h) if ln.strip()]
        if lines:
            gbs = [float(x[1]) for x in lines]
            print(f"hog {os.path.basename(h)}: n={len(gbs)} median {statistics.median(gbs):.1f} GB/s max {max(gbs):.1f} (cap 60)")
