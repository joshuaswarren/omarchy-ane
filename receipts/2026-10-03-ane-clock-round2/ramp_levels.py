#!/usr/bin/env python3
"""Per-call P6' exec times from the MacWinRun3 macOS runners: find the plateau levels of the cold-start
ramp and fit them to the ADT ANE ladder (voltage-states8) as t = a + c/f.

Usage: ramp_levels.py <MacWinRun3 dir>
Input: macos-pass*/out/w2/{ramp-*,dtrace/run}/runner.json (key exec_ms: one value per evaluate call, in order).
Output: per run the first 40 calls, the plateaus (runs of >= 2 calls within 3%), the steady median of calls
200.., and a least-squares fit of plateau times to the top-k ladder states (the assignment is INFERENCE).
"""
import glob
import json
import os
import statistics
import sys

LADDER = [600, 852, 1104, 1356, 1596, 1848, 2100]  # MHz, voltage-states8 (live macOS 27 ADT, ioreg L5069)


def plateaus(xs, tol=0.03):
    out, i = [], 0
    while i < len(xs):
        j = i
        while j + 1 < len(xs) and abs(xs[j + 1] - xs[i]) <= tol * xs[i]:
            j += 1
        out.append((i, j - i + 1, statistics.median(xs[i : j + 1])))
        i = j + 1
    return out


def fit(levels, freqs):
    """Least squares t = a + c / f; returns a, c, rms."""
    xs = [1.0 / f for f in freqs]
    n = len(xs)
    mx, my = sum(xs) / n, sum(levels) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    c = sum((x - mx) * (y - my) for x, y in zip(xs, levels)) / sxx
    a = my - c * mx
    rms = (sum((a + c * x - y) ** 2 for x, y in zip(xs, levels)) / n) ** 0.5
    return a, c, rms


def main():
    root = sys.argv[1]
    for path in sorted(glob.glob(os.path.join(root, "macos-pass*/out/w2/*/runner.json")) + sorted(glob.glob(os.path.join(root, "macos-pass*/out/w2/dtrace/run/runner.json")))):
        d = json.load(open(path))
        xs = d.get("exec_ms") or []
        if len(xs) < 200:
            continue
        rel = os.path.relpath(path, root)
        steady = statistics.median(xs[200:])
        print("== %s calls=%d steady(median 200..)=%.4f first=%.3f first/steady=%.3f" % (rel, len(xs), steady, xs[0], xs[0] / steady))
        print("   first40:", " ".join("%.3f" % x for x in xs[:40]))
        pl = [p for p in plateaus(xs[:80]) if p[1] >= 2]
        print("   plateaus(start,len,median):", " ".join("(%d,%d,%.3f)" % p for p in pl))
        levels = sorted({round(p[2], 2) for p in pl} | {round(steady, 2)}, reverse=True)
        for k in range(len(levels), 0, -1):
            sel = levels[-k:]
            # assign the k fastest distinct levels to the top-k ladder states, slowest to the lowest of them
            freqs = LADDER[-k:] if k <= len(LADDER) else None
            if not freqs or k < 2:
                continue
            a, c, rms = fit(sel, freqs)
            print("   fit top-%d %s -> %s: a=%.3f ms c=%.1f ms*MHz rms=%.3f; predicted t(600)=%.3f t(2100)=%.3f" % (k, sel, freqs, a, c, rms, a + c / 600, a + c / 2100))
            break


if __name__ == "__main__":
    main()
