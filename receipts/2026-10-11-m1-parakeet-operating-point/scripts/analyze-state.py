#!/usr/bin/env python3
"""analyze-state.py RUNDIR... : one line per sampled Parakeet run.
Active window = rows where the driver job counter is between its first value in the window and +20 (my 20 calls;
the filler is stopped by the guard while my ticket runs). Columns come from state-sampler.sh."""
import csv
import re
import statistics as st
import sys

print("run median_ms | active window: dur_s busy_frac | sys_W heatpipe_W ac_W fan temp_NAND temp_chg tz0 cpu0_MHz cpuhi_MHz load1 | pre-run (first 3 rows) sys_W heatpipe_W")
for d in sys.argv[1:]:
    try:
        rows = list(csv.DictReader(open(f"{d}/state.csv")))
        res = open(f"{d}/results.txt").read()
    except OSError:
        continue
    med = re.search(r"median ([0-9.]+)", res)
    rows = [r for r in rows if r["jobs"] and r["jobs"].isdigit()]
    if len(rows) < 5:
        continue
    jobs = [int(r["jobs"]) for r in rows]
    # my window: the stretch in which the counter advances by exactly 20 with small steps
    start = next((i for i in range(1, len(rows)) if 0 < jobs[i] - jobs[i - 1] <= 2 and jobs[min(i + 40, len(rows) - 1)] - jobs[i - 1] <= 25), 1)
    end = next((i for i in range(len(rows) - 1, start, -1) if jobs[i] - jobs[i - 1] > 0), len(rows) - 1)
    w = rows[start:end + 1]
    f = lambda k, s=1.0: st.mean(float(r[k]) for r in w) / s
    dur = float(w[-1]["t_s"]) - float(w[0]["t_s"])
    busy = (int(w[-1]["busy_ns"]) - int(w[0]["busy_ns"])) / 1e9
    pre = rows[:3]
    p = lambda k: st.mean(float(r[k]) for r in pre) / 1e6
    print(f"{d.rsplit('/', 1)[-1]} {med.group(1) if med else '?'} | {dur:.2f} {busy / dur if dur else 0:.2f} | "
          f"{f('sys_power_uW', 1e6):.1f} {f('heatpipe_uW', 1e6):.1f} {f('ac_in_uW', 1e6):.1f} {f('fan_rpm'):.0f} "
          f"{f('nand_mC', 1e3):.1f} {f('charger_mC', 1e3):.1f} {f('tz0_mC', 1e3):.1f} {f('cpu0_kHz', 1e3):.0f} {f('cpu_hi_kHz', 1e3):.0f} {f('loadavg1'):.2f} | "
          f"{p('sys_power_uW'):.1f} {p('heatpipe_uW'):.1f}")
