#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Tabulate macOS window 2 (and optional Linux twin) ANE clock-state captures.

  python3 analyze_w2.py --synthetic                       # self-check on generated data
  python3 analyze_w2.py --macos <fetched>/out [--linux <dmesg with ane_clk_twin_probe lines>] \
      [--pmgr-regs <DEC decode with 'reg[ N] 0xPA +0xLEN' lines>] [--json report.json]
  python3 analyze_w2.py --runner-json <ane_inmem_run block JSON>...   # ramp of any runner output

Per word: macOS values with the ANE powered down / up, every change inside a P6' ramp (ms from the
first islands-up dump), Linux values, and flags LOAD-CHANGE / RAMP-CHANGE / OS-DIFF. Per ramp: the
runner's per-call ms (cold call 1 to steady state), dump cadence. IOService idle vs load diffs
(keys matching freq/perf/state/volt/dvfm/clock/power first), powermetrics ANE lines, the log stream,
dtrace writeReg32 groups (RegMap = ADT pmgr reg index is the T6001 decode: INFERENCE here), and the
`mask=` value for the Linux twin (words every macOS dump read with status ok).
"""
import argparse
import glob
import json
import os
import plistlib
import re
import statistics
import struct
import sys
import tempfile
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "macos-bundle"))
from w2sample import HDR, RANGE, REC, read_series  # noqa: E402

MASTER = os.path.join(HERE, "macos-bundle", "ranges-w2.txt")
PROP_RE = re.compile(r"freq|perf|state|volt|dvfm|clock|power", re.I)
LINUX_RE = re.compile(r"ane_clk_twin_probe: s=(\d+) t=(\d+) (.*)")


def word_order(master=MASTER):
    """Range names in master order = the Linux twin's table order (its mask bit index)."""
    return [ln.split()[1] for ln in open(master) if ln.startswith("range ")]


def series_under(d):
    return [s for p in sorted(glob.glob(os.path.join(d, "**", "series.bin"), recursive=True))
            for s in read_series(p)]


def events(d):
    ev = {}
    p = os.path.join(d, "events.tsv")
    if os.path.exists(p):
        for line in open(p):
            t, what, *_ = line.rstrip("\n").split("\t")
            ev.setdefault(what, int(t))
    return ev


def exec_ramp(exec_ms, tol=0.10, run=5):
    """Cold start to steady state of one runner: steady = median of the second half; the ramp ends
    at the first call that starts `run` consecutive calls within `tol` of steady."""
    if not exec_ms:
        return None
    steady = statistics.median(exec_ms[len(exec_ms) // 2:])
    n = len(exec_ms)
    i = next((i for i in range(n) if all(x <= steady * (1 + tol) for x in exec_ms[i:i + run])), n)
    return {"calls": n, "first": [round(x, 3) for x in exec_ms[:5]], "steady_ms": round(steady, 4),
            "first_over_steady": round(exec_ms[0] / steady, 3), "calls_to_steady": i,
            "ms_to_steady": round(sum(exec_ms[:i]), 2)}


def words_table(w2, order):
    """name -> {idle, up, status, ramps}; ramps hold value changes while the islands are up."""
    tab = {n: {"idle": set(), "up": set(), "status": Counter(), "ramps": []} for n in order}

    def add(samples):
        for s in samples:
            for n, (_pa, st, words) in s["ranges"].items():
                if n not in tab:
                    continue
                tab[n]["status"][st] += 1
                if st == "ok":
                    tab[n]["up" if s["islands_up"] else "idle"].add(words[0])

    for d in sorted(glob.glob(os.path.join(w2, "*"))):
        if os.path.isdir(d) and not os.path.basename(d).startswith(("ramp-", "ioservice", "log", "dtrace", "ranges")):
            add(series_under(d))
    ramps = []
    for d in sorted(glob.glob(os.path.join(w2, "ramp-*"))):
        ss = series_under(d)
        add(ss)
        up = [s for s in ss if s["islands_up"]]
        t_up = up[0]["t0"] if up else None
        down = next((s["t0"] for s in ss if t_up and s["t0"] > t_up and not s["islands_up"]), None)
        ev = events(d)
        rj = os.path.join(d, "runner.json")
        ex = json.load(open(rj)).get("exec_ms", []) if os.path.exists(rj) and os.path.getsize(rj) else []
        gaps = [(b["t0"] - a["t0"]) / 1e6 for a, b in zip(ss, ss[1:])]
        r = {"ramp": os.path.basename(d), "dumps": len(ss),
             "dump_period_ms_median": round(statistics.median(gaps), 3) if gaps else None,
             "dump_call_us_median": round(statistics.median([(s["t1"] - s["t0"]) / 1e3 for s in ss]), 1) if ss else None,
             "spawn_to_up_ms": round((t_up - ev["runner-spawn"]) / 1e6, 1) if t_up and "runner-spawn" in ev else None,
             "exit_to_down_ms": round((down - ev["runner-exit"]) / 1e6, 1) if down and "runner-exit" in ev else None,
             "exec": exec_ramp(ex)}
        ramps.append(r)
        for n in tab:
            last, changes = None, []
            for s in up:
                v = s["ranges"].get(n)
                if not v or v[1] != "ok":
                    continue
                if last is not None and v[2][0] != last:
                    changes.append((round((s["t0"] - t_up) / 1e6, 2), f"{last:#x}->{v[2][0]:#x}"))
                last = v[2][0]
            if changes:
                tab[n]["ramps"].append({"ramp": r["ramp"], "changes": changes[:20]})
    return tab, ramps


def linux_words(path):
    vals, first = defaultdict(set), []
    for line in open(path, errors="replace"):
        if "first read" in line and "ane_clk_twin_probe" in line:
            first.append(line.strip())
        m = LINUX_RE.search(line)
        if m:
            for kv in m.group(3).split():
                k, _, v = kv.partition("=")
                if v.startswith("0x"):
                    vals[k].add(int(v, 16))
    return vals, first


def flatten_plist(path):
    """{node path: {key: repr(value)}} for every node of an `ioreg -r -a -l -c CLASS` dump."""
    out = {}

    def walk(node, path):
        props = {k: v for k, v in node.items() if k != "IORegistryEntryChildren"}
        out[path] = {k: repr(v)[:160] for k, v in props.items()}
        for i, ch in enumerate(node.get("IORegistryEntryChildren", [])):
            walk(ch, f"{path}/{ch.get('IORegistryEntryName', '?')}#{i}")

    try:
        roots = plistlib.load(open(path, "rb"))
    except Exception as e:  # ioreg error text instead of a plist
        return {"<unreadable>": {"error": str(e)[:120]}}
    for i, n in enumerate(roots if isinstance(roots, list) else [roots]):
        walk(n, f"{n.get('IORegistryEntryName', '?')}#{i}")
    return out


def ioservice_diff(w2):
    idle, load = os.path.join(w2, "ioservice-idle"), os.path.join(w2, "ioservice-load")
    res = {"changed": [], "only_load_nodes": [], "candidate_keys": []}
    if not (os.path.isdir(idle) and os.path.isdir(load)):
        return res
    for p in sorted(glob.glob(os.path.join(idle, "*.plist"))):
        cls = os.path.basename(p)[:-6]
        a, q = flatten_plist(p), os.path.join(load, cls + ".plist")
        b = flatten_plist(q) if os.path.exists(q) else {}
        res["only_load_nodes"] += [f"{cls}:{n}" for n in b if n not in a][:20]
        for node, props in a.items():
            for k, v in props.items():
                if PROP_RE.search(k):
                    res["candidate_keys"].append((cls, node, k, v))
                w = b.get(node, {}).get(k)
                if w is not None and w != v:
                    res["changed"].append((bool(PROP_RE.search(k)), cls, node, k, v, w))
    res["changed"].sort(key=lambda c: not c[0])
    return res


def powermetrics_lines(w2):
    p = os.path.join(w2, "load", "powermetrics.txt")
    if not os.path.exists(p):
        return {}
    ane = [ln.strip() for ln in open(p, errors="replace") if "ANE" in ln]
    mw = [int(m.group(1)) for ln in ane for m in [re.search(r"ANE Power:\s*(\d+)\s*mW", ln)] if m]
    return {"ane_lines": sorted(Counter(re.sub(r"[\d.]+", "#", ln) for ln in ane).items())[:20],
            "ane_power_mw": [min(mw), max(mw)] if mw else None, "samples": len(mw)}


def logstream(w2):
    p = os.path.join(w2, "log", "stream.ndjson")
    if not os.path.exists(p):
        return {}
    procs, hits = Counter(), Counter()
    n = 0
    for line in open(p, errors="replace"):
        try:
            e = json.loads(line)
        except ValueError:
            continue
        n += 1
        procs[f"{e.get('processImagePath', '?').rsplit('/', 1)[-1]}|{e.get('subsystem', '')}"] += 1
        msg = e.get("eventMessage", "")
        if re.search(r"(?i)freq|perf|clock|dvfs|power", msg):
            hits[re.sub(r"0x[0-9a-f]+|\d+", "#", msg)[:140]] += 1
    return {"events": n, "top_sources": procs.most_common(10), "perf_messages": hits.most_common(20)}


def dtrace_groups(w2, regs):
    p = os.path.join(w2, "dtrace", "trace.out")
    if not os.path.exists(p):
        return {}
    ev = events(os.path.join(w2, "dtrace", "run"))
    funcs, writes = Counter(), defaultdict(list)
    for line in open(p, errors="replace"):
        m = re.match(r"(\w) w=(\d+) (\S+)((?: a\d=0x[0-9a-f]+)*)", line)
        if not m:
            continue
        funcs[(m.group(1), m.group(3))] += 1
        a = {k: int(v, 16) for k, v in re.findall(r"(a\d)=0x([0-9a-f]+)", m.group(4))}
        if "writeReg32" in m.group(3):
            writes[(a.get("a1"), a.get("a2"))].append((int(m.group(2)), a.get("a3")))
    t_on, t_off = ev.get("runner-spawn"), ev.get("runner-exit")
    groups = []
    for (rm, reg), w in sorted(writes.items(), key=lambda kv: -len(kv[1])):
        inside = sum(1 for t, _ in w if t_on and t_off and t_on <= t <= t_off)
        base = regs.get(rm)
        groups.append({"regmap": rm, "reg": hex(reg) if reg is not None else None, "writes": len(w),
                       "during_runner": inside, "values": sorted({hex(v) for _, v in w})[:8],
                       "pa_inference": hex(base + reg) if base is not None and reg is not None else None})
    return {"functions": [(t, f, n) for (t, f), n in funcs.most_common(30)], "writeReg32": groups[:40]}


def pmgr_regs(path):
    regs = {}
    for line in open(path):
        m = re.match(r"\s*reg\[\s*(\d+)\]\s+(0x[0-9a-f]+)\s+\+", line)
        if m:
            regs[int(m.group(1))] = int(m.group(2), 16)
    return regs


def analyze(macos, linux=None, regs_path=None, master=MASTER):
    w2 = os.path.join(macos, "w2")
    order = word_order(master)
    tab, ramps = words_table(w2, order)
    lin, first = linux_words(linux) if linux else ({}, [])
    words, mask = [], 0
    for i, n in enumerate(order):
        t = tab[n]
        flags = []
        if (t["idle"] and t["up"] and t["idle"] != t["up"]) or len(t["up"]) > 1:
            flags.append("LOAD-CHANGE")
        if t["ramps"]:
            flags.append("RAMP-CHANGE")
        if lin.get(n) and t["up"] and lin[n] != t["up"]:
            flags.append("OS-DIFF")
        reads = sum(t["status"].values())
        if reads and t["status"]["ok"] and set(t["status"]) <= {"ok", "gated"}:
            mask |= 1 << i
        words.append({"bit": i, "name": n, "macos_idle": sorted(hex(v) for v in t["idle"]),
                      "macos_up": sorted(hex(v) for v in t["up"]), "status": dict(t["status"]),
                      "ramps": t["ramps"], "linux": sorted(hex(v) for v in lin.get(n, ())), "flags": flags})
    return {"words": words, "linux_mask": hex(mask), "linux_first_reads": first, "ramps": ramps,
            "ioservice": ioservice_diff(w2), "powermetrics": powermetrics_lines(w2),
            "logstream": logstream(w2),
            "dtrace": dtrace_groups(w2, pmgr_regs(regs_path) if regs_path else {})}


def report(r):
    print("== words (macOS: islands down / up; Linux twin; flags)")
    for w in r["words"]:
        print(f"  {w['bit']:2d} {w['name']:14s} idle={','.join(w['macos_idle']) or '-':24s} "
              f"up={','.join(w['macos_up']) or '-':24s} linux={','.join(w['linux']) or '-':12s} "
              f"{' '.join(w['flags'])}  {w['status']}")
        for rr in w["ramps"]:
            print(f"       {rr['ramp']}: " + ", ".join(f"+{t}ms {c}" for t, c in rr["changes"]))
    print(f"== Linux twin: insmod ane_clk_twin_probe mask={r['linux_mask']} "
          "(words every macOS dump read ok; tier 2 only after its macOS first exposure)")
    for ln in r["linux_first_reads"][:30]:
        print("  " + ln)
    print("== ramps (P6' runner, cold call 1, no warm-up)")
    for rp in r["ramps"]:
        print(f"  {rp}")
    io = r["ioservice"]
    print(f"== IOService idle vs load: {len(io['changed'])} changed properties "
          f"({sum(c[0] for c in io['changed'])} perf-like), {len(io['only_load_nodes'])} load-only nodes")
    for c in io["changed"][:25]:
        print(f"  {'*' if c[0] else ' '} {c[1]} {c[2]} {c[3]}: {c[4][:60]} -> {c[5][:60]}")
    for c in io["candidate_keys"][:25]:
        print(f"  key {c[0]} {c[1]} {c[2]} = {c[3][:80]}")
    print(f"== powermetrics: {r['powermetrics']}")
    print(f"== log stream: {r['logstream']}")
    dt = r["dtrace"]
    print(f"== dtrace: {len(dt.get('functions', []))} functions hit")
    for f in dt.get("functions", [])[:15]:
        print(f"  {f}")
    for g in dt.get("writeReg32", [])[:20]:
        print(f"  writeReg32 {g}")


def synthetic(tmp):
    """Fake capture with known answers; returns the analysis result."""
    order = word_order()
    w2 = os.path.join(tmp, "out", "w2")

    def rec(t0, up, vals):
        rngs, data = [], b""
        for i, n in enumerate(order):
            st = 0 if (up or not n.startswith(("set-", "clk6", "pll-", "dev-", "ev-", "ane0-"))) else 1
            ln = 4 if st == 0 else 0
            rngs.append(RANGE.pack(n.encode(), 0x28e000000 + 4 * i, 0, ln, len(data), st, 0, 0, 0))
            data += struct.pack("<I", vals.get(n, 0x10 + i)) if ln else b""
        ps = [0x1f0003ff if up else 0x0f000300] * 2 + [0x3ff if up else 0x300] * 6
        hdr = HDR.pack(0x414e4531, 2, HDR.size + 40 * RANGE.size, len(order), len(data), int(up), *ps,
                       0, 0, *ps, 1, 0, 0x28e080000)
        body = hdr + b"".join(rngs) + b"\0" * ((40 - len(order)) * RANGE.size) + data
        return REC.pack(t0, t0 + 3000000, 0, len(body)) + body

    def write(sub, recs):
        os.makedirs(os.path.join(w2, sub), exist_ok=True)
        open(os.path.join(w2, sub, "series.bin"), "wb").write(b"".join(recs))

    t = 1_000_000_000_000
    write("tier1-idle/s", [rec(t + i * 200_000_000, False, {}) for i in range(5)])
    for b in ("set", "clk", "perf1", "dvfm"):
        write(f"first-{b}/a1", [rec(t + 2_000_000_000, True, {"pll-ane0": 0x5, "ane0-adclk-0": 0x77})])
    ramp = [rec(t + 3_000_000_000 + i * 2_000_000, 300 <= i < 2300,
                {"pll-ane0": 0x1 if i < 310 else 0x5, "ane0-adclk-0": 0x77}) for i in range(4000)]
    write("ramp-1/s", ramp)
    open(os.path.join(w2, "ramp-1", "events.tsv"), "w").write(
        f"{t + 3_000_000_000 + 580_000_000}\trunner-spawn\tx\n{t + 3_000_000_000 + 4_590_000_000}\trunner-exit\trc=0\n")
    json.dump({"exec_ms": [2.8, 2.7, 2.1, 1.7, 1.5, 1.3] + [1.2, 1.31] * 100},
              open(os.path.join(w2, "ramp-1", "runner.json"), "w"))
    write("load/regs/s", [rec(t + 9_000_000_000, True, {"pll-ane0": 0x5, "ane0-adclk-0": 0x77})])
    for tag, f in (("idle", 600), ("load", 2100)):
        d = os.path.join(w2, f"ioservice-{tag}")
        os.makedirs(d)
        plistlib.dump([{"IORegistryEntryName": "ane0", "ANEFrequencies": f, "Busy": 0,
                        "IORegistryEntryChildren": [{"IORegistryEntryName": "uc", "x": 1}]}],
                      open(os.path.join(d, "H11ANEIn.plist"), "wb"))
    os.makedirs(os.path.join(w2, "load"), exist_ok=True)
    open(os.path.join(w2, "load", "powermetrics.txt"), "w").write("ANE Power: 9046 mW\nANE Power: 2020 mW\n")
    os.makedirs(os.path.join(w2, "log"))
    open(os.path.join(w2, "log", "stream.ndjson"), "w").write(
        json.dumps({"processImagePath": "/usr/libexec/aned", "subsystem": "com.apple.ane",
                    "eventMessage": "setting FW perf mode 3"}) + "\nnot json\n")
    os.makedirs(os.path.join(w2, "dtrace", "run"))
    open(os.path.join(w2, "dtrace", "run", "events.tsv"), "w").write("100\trunner-spawn\tx\n900\trunner-exit\trc=0\n")
    open(os.path.join(w2, "dtrace", "trace.out"), "w").write(
        "TRACE-START w=1\n" + "".join(f"P w={t} _ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj a1=0x0 a2=0x6c100 "
                                      f"a3=0x{v:x} a4=0x0\n" for t, v in ((50, 1), (200, 5), (950, 1))))
    regs = os.path.join(tmp, "dec.txt")
    open(regs, "w").write("  reg[ 0] 0x28e080000 +0x80000\n")
    lin = os.path.join(tmp, "dmesg.txt")
    open(lin, "w").write("[ 1.0] ane_clk_twin_probe: first read pll-ane0 0x28e0e03b0\n"
                         "[ 1.1] ane_clk_twin_probe: s=0 t=5 fabric-ps=0x555 pll-ane0=0x1 ane0-adclk-0=0x77\n")
    return analyze(os.path.join(tmp, "out"), lin, regs)


def selfcheck():
    with tempfile.TemporaryDirectory() as tmp:
        r = synthetic(tmp)
    report(r)
    w = {x["name"]: x for x in r["words"]}
    assert w["pll-ane0"]["flags"] == ["LOAD-CHANGE", "RAMP-CHANGE", "OS-DIFF"], w["pll-ane0"]
    assert w["pll-ane0"]["ramps"][0]["changes"] == [(20.0, "0x1->0x5")], w["pll-ane0"]["ramps"]
    assert w["ane0-adclk-0"]["flags"] == [] and w["ane0-adclk-0"]["linux"] == ["0x77"], w["ane0-adclk-0"]
    assert w["fabric-ps"]["flags"] == ["OS-DIFF"], w["fabric-ps"]  # same value idle/up; Linux 0x555
    assert int(r["linux_mask"], 16) == (1 << len(w)) - 1, r["linux_mask"]
    rp = r["ramps"][0]
    assert rp["spawn_to_up_ms"] == 20.0 and rp["exit_to_down_ms"] == 10.0, rp
    assert rp["exec"]["calls_to_steady"] == 5 and rp["exec"]["first_over_steady"] > 2, rp["exec"]
    assert rp["dump_period_ms_median"] == 2.0, rp
    io = r["ioservice"]
    assert io["changed"][0][3] == "ANEFrequencies" and io["changed"][0][0], io["changed"]
    assert r["powermetrics"]["ane_power_mw"] == [2020, 9046], r["powermetrics"]
    assert r["logstream"]["events"] == 1 and r["logstream"]["perf_messages"], r["logstream"]
    g = r["dtrace"]["writeReg32"][0]
    assert g["writes"] == 3 and g["during_runner"] == 1 and g["pa_inference"] == "0x28e0ec100", g
    print("selfcheck OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--macos")
    ap.add_argument("--linux")
    ap.add_argument("--pmgr-regs")
    ap.add_argument("--json")
    ap.add_argument("--runner-json", nargs="+")
    a = ap.parse_args()
    if a.synthetic:
        selfcheck()
        return 0
    if a.runner_json:
        for p in a.runner_json:
            print(p, exec_ramp(json.load(open(p)).get("exec_ms", [])))
        return 0
    if not a.macos:
        ap.error("--macos, --runner-json or --synthetic")
    r = analyze(a.macos, a.linux, a.pmgr_regs)
    report(r)
    if a.json:
        json.dump(r, open(a.json, "w"), indent=1, default=list)
    return 0


if __name__ == "__main__":
    sys.exit(main())
