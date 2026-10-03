#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Post-process the window-3 dtrace of the ANE op-point write path (AneClockHunt round 2, E1).

  python3 analyze_w3.py --synthetic                              # PASS / STOP / UNRESOLVED fixtures
  python3 analyze_w3.py --trace trace.out --events events.tsv [--runner-json r1.json r2.json r3.json]
                        [--regmap pmgr-regmap-26A428.json] [--json out.json]

Per ramp (one runner spawn to exit): every register write in time order, with t relative to the ramp's
first aneWorkBegin, the distinct (function, RegMap, offset, value) triples, each triple's call index
(the aneWorkBegin count at that time) and the plateau level of that call (runner exec_ms). Then the
decision from the round-2 report: PASS when exactly one (function, RegMap, offset) wrote >= 5 distinct
values whose first-occurrence order follows the plateau order; STOP when no AP write happened inside
any ramp; otherwise UNRESOLVED. RegMap -> PA uses the pmgr IODeviceMemory list (RegMap = reg index:
INFERENCE until the fabric-ps write at RegMap 40 confirms it).
"""
import argparse
import bisect
import json
import os
import re
import sys
import tempfile
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
PLATEAUS_MS = [3.911, 2.707, 2.107, 1.770, 1.541, 1.345, 1.220]   # round-2 fit_ladder, ramp-2
LADDER_MHZ = [600, 852, 1104, 1356, 1596, 1848, 2100]              # ADT voltage-states8
FIT_A, FIT_C = 0.117, 2249.7                                        # t = a + c/f (ms, MHz)
LINE = re.compile(r"^(E|R|W|B|SEG|A) (.*)$")
KV = re.compile(r"(\w+)=(\S+)")


def parse_trace(path):
    """Events in file order; stack frames (indented lines) attach to the preceding E/W event."""
    ev, aggs = [], []
    for raw in open(path, errors="replace"):
        line = raw.rstrip("\n")
        if not line.strip():
            continue
        if line[0] in " \t":
            if ev:
                ev[-1].setdefault("stack", []).append(line.strip())
            continue
        m = LINE.match(line)
        if not m:
            continue
        kind, rest = m.groups()
        d = {"k": kind}
        for key, val in KV.findall(rest):
            d[key] = int(val, 0) if re.fullmatch(r"-?(0x[0-9a-fA-F]+|\d+)", val) else val
        (aggs if kind == "A" else ev).append(d)
    return ev, aggs


def read_events(path):
    """[(spawn_ns, exit_ns)] from w2sample events.tsv (runner-spawn / runner-exit pairs)."""
    ramps, spawn = [], None
    for line in open(path):
        t, what, *_ = line.rstrip("\n").split("\t")
        if what == "runner-spawn":
            spawn = int(t)
        elif what == "runner-exit" and spawn is not None:
            ramps.append((spawn, int(t)))
            spawn = None
    return ramps


def level_of(ms):
    """Index of the nearest plateau (0 = bottom state 600 MHz), or None if far from every plateau."""
    best = min(range(len(PLATEAUS_MS)), key=lambda i: abs(PLATEAUS_MS[i] - ms))
    return best if abs(PLATEAUS_MS[best] - ms) <= 0.25 else None


def monotonic(vals):
    return len(vals) >= 2 and (all(a < b for a, b in zip(vals, vals[1:])) or all(a > b for a, b in zip(vals, vals[1:])))


def analyze(trace, events, runner_json=(), regmap=None):
    ev, aggs = parse_trace(trace)
    ramps = read_events(events)
    regs = {r["index"]: int(r["pa"], 16) for r in json.load(open(regmap))["regs"]} if regmap else {}
    writes = [e for e in ev if e["k"] == "W"]
    begins = [e["w"] for e in ev if e["k"] == "B" and "aneWorkBegin" in str(e.get("f", ""))]
    out = {"ramps": [], "aggregates": aggs, "writes_total": len(writes), "events_total": len(ev),
           "absent_note": "a W line carries a1=RegMap a2=offset a3=value a4=die for ApplePMGR::writeReg32 "
                          "(T6001 capture 2026-09-25: a1=0 a2=0x18014 a3=0xf); other functions: see probes.json"}
    rjs = [json.load(open(p)).get("exec_ms", []) for p in runner_json]
    candidates = {}
    for ri, (t_spawn, t_exit) in enumerate(ramps):
        b = [t for t in begins if t_spawn <= t <= t_exit]
        t0 = b[0] if b else t_spawn
        ex = rjs[ri] if ri < len(rjs) else []
        lv = [level_of(x) for x in ex]
        ws = [w for w in writes if t_spawn <= w["w"] <= t_exit]
        rows, seq = [], defaultdict(list)
        for w in ws:
            call = bisect.bisect_right(b, w["w"]) - 1 if b else -1
            trip = (w["f"], w.get("a1"), w.get("a2"))
            val = w.get("a3")
            if val not in seq[trip]:
                seq[trip].append(val)
            rows.append({"t_ms": round((w["w"] - t0) / 1e6, 3), "f": w["f"], "regmap": w.get("a1"),
                         "offset": w.get("a2"), "value": val, "d": w.get("d"), "call": call,
                         "call_ms": ex[call] if 0 <= call < len(ex) else None,
                         "level": lv[call] if 0 <= call < len(lv) else None,
                         "next_level": lv[call + 1] if 0 <= call + 1 < len(lv) else None,
                         "pa": hex(regs[w["a1"]] + w["a2"]) if w.get("a1") in regs and isinstance(w.get("a2"), int) else None,
                         "stack": w.get("stack", [])[:3]})
        fam = {}
        for trip, vals in seq.items():
            first = [r for r in rows if (r["f"], r["regmap"], r["offset"]) == trip]
            levels = []
            for v in vals:
                r = next(x for x in first if x["value"] == v)
                levels.append(r["next_level"] if r["next_level"] is not None else r["level"])
            known = [l for l in levels if l is not None]
            fam[trip] = {"values": vals, "levels_at_first_write": levels,
                         "monotonic_values": monotonic(vals), "plateau_order": known == sorted(known) and len(set(known)) >= 2,
                         "distinct": len(vals), "writes": len(first),
                         "pa": first[0]["pa"]}
            if len(vals) >= 5 and fam[trip]["monotonic_values"]:
                candidates.setdefault(trip, []).append(ri)
        out["ramps"].append({"ramp": ri + 1, "spawn": t_spawn, "exit": t_exit, "calls_seen": len(b),
                             "first_begin_rel_spawn_ms": round((t0 - t_spawn) / 1e6, 1) if b else None,
                             "exec_levels": [l for l in lv[:40]], "writes": len(ws), "rows": rows[:400],
                             "families": {f"{k[0]} map={k[1]} off={k[2]:#x}" if isinstance(k[2], int) else str(k): v
                                          for k, v in fam.items()}})
    any_writes = any(r["writes"] for r in out["ramps"])
    strong = {k: v for k, v in candidates.items() if len(v) >= 1}
    if not ramps:
        out["decision"] = "INVALID: no runner spawn/exit pairs in events.tsv"
    elif not any(r["calls_seen"] for r in out["ramps"]):
        out["decision"] = "INVALID: no aneWorkBegin anchor inside any ramp (wrong trace format, or the anchor probes are absent)"
    elif not any_writes:
        out["decision"] = "STOP: no AP-side register write inside any ramp (op point is coprocessor- or firmware-owned)"
    elif len(strong) == 1:
        (k, rs), = strong.items()
        out["decision"] = (f"PASS: {k[0]} RegMap={k[1]} offset={k[2]:#x} wrote >= 5 distinct values in order "
                           f"in ramp(s) {[r + 1 for r in rs]}")
    else:
        out["decision"] = f"UNRESOLVED: writes seen; {len(strong)} families with >= 5 ordered values ({[k[0] for k in strong]})"
    out["candidates"] = {f"{k[0]} map={k[1]} off={k[2]:#x}": [r + 1 for r in v] for k, v in strong.items()}
    return out


def report(r):
    print(f"events={r['events_total']} writes={r['writes_total']} aggregates={len(r['aggregates'])}")
    for rp in r["ramps"]:
        print(f"== ramp {rp['ramp']}: calls_seen={rp['calls_seen']} first_begin+{rp['first_begin_rel_spawn_ms']}ms "
              f"writes={rp['writes']} levels[:40]={rp['exec_levels']}")
        for row in rp["rows"][:60]:
            print(f"  +{row['t_ms']:9.3f}ms {row['f'][:48]:48s} map={row['regmap']} off={row['offset']:#x} "
                  f"val={row['value']:#x} call={row['call']} lvl={row['level']}->{row['next_level']} pa={row['pa']} d={row['d']}"
                  if isinstance(row['offset'], int) and isinstance(row['value'], int) else f"  {row}")
        for k, v in rp["families"].items():
            print(f"  family {k}: distinct={v['distinct']} writes={v['writes']} values={[hex(x) if isinstance(x, int) else x for x in v['values']]} "
                  f"levels={v['levels_at_first_write']} monotonic={v['monotonic_values']} plateau_order={v['plateau_order']} pa={v['pa']}")
    for a in r["aggregates"][:40]:
        print(f"  agg {a}")
    print(f"ladder: plateaus_ms={PLATEAUS_MS} MHz={LADDER_MHZ} (t = {FIT_A} + {FIT_C}/f)")
    print(f"DECISION: {r['decision']}")


def synthetic(kind, tmp):
    """One ramp of 40 calls; writes depend on `kind`: pass / stop / unresolved."""
    t = 1_800_000_000_000_000_000
    spawn, calls = t + 10_000_000_000, 40
    begins = [spawn + 30_000_000 + i * 1_500_000 for i in range(calls)]
    exec_ms = [3.91] * 6 + [2.70] * 5 + [2.10] * 5 + [1.77] * 5 + [1.54] * 5 + [1.34] * 5 + [1.22] * 9
    lines = [f"TRACE-START w={t}", f"SEG w={spawn + 1000} seg=1 pid=4242"]
    writes = []
    if kind == "pass":
        for i, v in zip((5, 10, 15, 20, 25, 30), (1, 2, 3, 4, 5, 6)):
            writes.append((begins[i] + 700_000, "_ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj", 41, 0xa04, 0x80000000 | v))
    elif kind == "unresolved":
        for i, v in zip((5, 10, 15, 20, 25, 30), (1, 2, 3, 4, 5, 6)):
            writes.append((begins[i] + 700_000, "_ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj", 41, 0xa04, v))
            writes.append((begins[i] + 800_000, "_ZN14AppleT6020PMGR10writeReg32EN9ApplePMGR6RegMapEjjj", 0, 0x1e8, 0xf0 + v))
    for i, b in enumerate(begins):
        lines.append(f"B w={b} f=_ZN4clpc4CLPC12aneWorkBeginERNS_4tgrp20ThreadGroupContainerEyRN")
        for w in [x for x in writes if b <= x[0] < b + 1_500_000]:
            lines.append(f"W w={w[0]} f={w[1]} d=1 a0=0xfffffe001 a1={w[2]:#x} a2={w[3]:#x} a3={w[4]:#x} a4=0x0")
            lines.append("              com.apple.driver.ApplePMGR`_ZN9ApplePMGR13_setPerfStateEjhj+0x1c4")
            lines.append("              com.apple.driver.AppleT6020CLPC`_ZN4clpc4pmgr17PMCVoterInterface20setANEPerfStateFloorEhb+0x40")
        lines.append(f"B w={b + 1_200_000} f=_ZN4clpc4CLPC10aneWorkEndERNS_4tgrp20ThreadGroupContainerEyRN")
    # a write outside every ramp must not count
    lines.append(f"W w={t + 2_000_000_000} f=_ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj d=0 a0=0x1 a1=0x0 a2=0x1e8 a3=0xf a4=0x0")
    lines.append(f"TRACE-END w={begins[-1] + 11_000_000_000} hot=7")
    lines.append("A f=_ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj seg=1 a1=0x29 a2=0xa04 a3=0x80000003 n=1 first=1 last=1")
    open(f"{tmp}/trace.out", "w").write("\n".join(lines) + "\n")
    open(f"{tmp}/events.tsv", "w").write(f"{spawn}\trunner-spawn\tx\n{begins[-1] + 2_000_000}\trunner-exit\trc=0\n")
    json.dump({"exec_ms": exec_ms}, open(f"{tmp}/runner.json", "w"))
    return analyze(f"{tmp}/trace.out", f"{tmp}/events.tsv", [f"{tmp}/runner.json"],
                   os.path.join(HERE, "pmgr-regmap-26A428.json"))


def selfcheck():
    with tempfile.TemporaryDirectory() as tmp:
        r = synthetic("pass", tmp)
        report(r)
        assert r["decision"].startswith("PASS: _ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj RegMap=41 offset=0xa04"), r["decision"]
        fam = r["ramps"][0]["families"]["_ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj map=41 off=0xa04"]
        assert fam["distinct"] == 6 and fam["plateau_order"] and fam["pa"] == "0x285868a04", fam
        assert fam["levels_at_first_write"] == [1, 2, 3, 4, 5, 6], fam  # a write during call k shows at call k+1
        assert r["writes_total"] == 7 and r["ramps"][0]["writes"] == 6, (r["writes_total"], r["ramps"][0]["writes"])
        assert r["ramps"][0]["rows"][0]["stack"][0].endswith("_setPerfStateEjhj+0x1c4"), r["ramps"][0]["rows"][0]
        assert r["ramps"][0]["calls_seen"] == 40
        assert len(r["aggregates"]) == 1 and r["aggregates"][0]["a3"] == 0x80000003
        r = synthetic("stop", tmp)
        assert r["decision"].startswith("STOP"), r["decision"]
        r = synthetic("unresolved", tmp)
        assert r["decision"].startswith("UNRESOLVED") and "2 families" in r["decision"], r["decision"]
    print("selfcheck OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--trace")
    ap.add_argument("--events")
    ap.add_argument("--runner-json", nargs="*", default=[])
    ap.add_argument("--regmap", default=os.path.join(HERE, "pmgr-regmap-26A428.json"))
    ap.add_argument("--json")
    a = ap.parse_args()
    if a.synthetic:
        selfcheck()
        return 0
    if not (a.trace and a.events):
        ap.error("--trace and --events, or --synthetic")
    r = analyze(a.trace, a.events, a.runner_json, a.regmap)
    report(r)
    if a.json:
        json.dump(r, open(a.json, "w"), indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
