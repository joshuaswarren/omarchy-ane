#!/usr/bin/env python3
"""Fit the macOS P6' cold-start plateaus to the ADT ANE ladder and place Linux on it.

Usage: fit_ladder.py <MacWinRun3 dir>
Plateau indices are read off ramp-2 (passC/out/w2/ramp-2/runner.json exec_ms, printed by ramp_levels.py):
calls 0-1 bottom, 2-4, 5-6, 7-8, 9-10, 12-16 (odd/even split ignored: median), 18.. steady (median 18-217).
Assignment (INFERENCE): the 7 levels, slowest first, are the 7 voltage-states8 states, lowest first.
Model: t = a + c / f (a = fixed per-call time, c = compute-bound work in ms*MHz).
Linux P6' (MacWinRun analysis-output.txt): raw min-of-min 5.465 ms, corrected (raw - 1.05 ms settle) 4.415 ms.
"""
import json
import os
import statistics
import sys

LADDER = [600, 852, 1104, 1356, 1596, 1848, 2100]
GROUPS = [(0, 2), (2, 5), (5, 7), (7, 9), (9, 11), (12, 17), (18, 218)]

xs = json.load(open(os.path.join(sys.argv[1], "macos-passC/out/w2/ramp-2/runner.json")))["exec_ms"]
levels = [statistics.median(xs[a:b]) for a, b in GROUPS]
inv = [1.0 / f for f in LADDER]
n = len(inv)
mx, my = sum(inv) / n, sum(levels) / n
c = sum((x - mx) * (y - my) for x, y in zip(inv, levels)) / sum((x - mx) ** 2 for x in inv)
a = my - c * mx
print("levels (ms):", ["%.3f" % v for v in levels])
for f, v in zip(LADDER, levels):
    print("  %4d MHz  measured %.3f  model %.3f  resid %+.3f" % (f, v, a + c / f, v - (a + c / f)))
rms = (sum((v - (a + c / f)) ** 2 for f, v in zip(LADDER, levels)) / n) ** 0.5
print("fit: a=%.3f ms  c=%.1f ms*MHz  rms=%.3f ms" % (a, c, rms))
print("macOS steady/bottom ratio: %.2f (bottom %.3f / steady %.3f)" % (levels[0] / levels[-1], levels[0], levels[-1]))
for label, t in (("Linux corrected", 4.415), ("Linux raw", 5.465)):
    print("%s %.3f ms -> f_eff = c/(t-a) = %.0f MHz; t / macOS bottom = %.2f" % (label, t, c / (t - a), t / levels[0]))
