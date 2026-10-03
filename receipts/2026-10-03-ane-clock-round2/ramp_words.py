#!/usr/bin/env python3
"""Values of every MacWinRun3 dump word inside the first N ms after the islands came up, per ramp.
Usage: ramp_words.py <w2sample.py dir> <ramp dir> [window_ms]
w2sample.py = omarchy-ane origin/agent/ane-macos-window2 receipts/.../macos-bundle/w2sample.py (read_series)."""
import collections, glob, os, sys
sys.path.insert(0, sys.argv[1])
from w2sample import read_series
d, win = sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 200.0
ss = [s for p in sorted(glob.glob(os.path.join(d, "**", "series.bin"), recursive=True)) for s in read_series(p)]
up = [s for s in ss if s["islands_up"]]
print("dumps", len(ss), "islands_up", len(up))
if not up:
    sys.exit()
t_up = up[0]["t0"]
vals = collections.defaultdict(collections.Counter)
n = 0
for s in ss:
    if t_up <= s["t0"] <= t_up + win * 1e6 and s["islands_up"]:
        n += 1
        for name, (pa, st, words) in s["ranges"].items():
            vals[name][(st, tuple(words))] += 1
print("dumps with islands up in the first %.0f ms after first up: %d" % (win, n))
for name, c in vals.items():
    print("  %-14s %s" % (name, ", ".join("%s:%s x%d" % (st, "/".join("%#x" % w for w in ws), k) for (st, ws), k in c.most_common(4))))
