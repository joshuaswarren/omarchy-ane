#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Window 4: the ANE op-point token word (0x285869200) read on macOS during cold P6' ramps.

  python3 analyze_w4.py --synthetic
  python3 analyze_w4.py --out <fetched>/out [--json r.json]

Reads out/w4/accept.txt, the first-read dumps, every ramp (series.bin + events.tsv + runner.json) and the
optional ramp-dtrace trace.out (window-3 W lines). Per ramp: the token value series against time from the
first islands-up dump, the decoded (prev, new) nibbles, the call index of each change (from the runner's
per-call ms laid end to end from power-on, or from aneWorkBegin anchors when a trace exists), the P6' level
of that call against the round-2 plateaus, and the dtrace write sequence when present.

Verdict (pre-registered): TRACKS = >= 3 distinct token values in a ramp with a consistent chain (each new
value's prev nibble equals the previous value's new nibble) and non-decreasing new states on the way up.
STATIC = reads ok but <= 1 distinct value. GATED/REFUSED = the kext rejected the request, or no read of
the word ever had status ok.
"""
import argparse
import bisect
import glob
import json
import os
import re
import sys
import tempfile
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "2026-10-03-ane-macos-window2", "macos-bundle"))
from w2sample import HDR, RANGE, REC, read_series  # noqa: E402

WORD = "opp-1200"
PLATEAUS_MS = [3.911, 2.707, 2.107, 1.770, 1.541, 1.345, 1.220]   # round 2: state 0..6 = 600..2100 MHz
LADDER_MHZ = [600, 852, 1104, 1356, 1596, 1848, 2100]
EXPECTED = [0x80000001, 0x80000012, 0x80000023, 0x80000034, 0x80000045, 0x80000056]  # window 3, ramp 2


def decode(v):
    """(trigger, prev, new) of a DVFS token 0x80000000 | prev<<4 | new."""
    return bool(v & 0x80000000), (v >> 4) & 0xf, v & 0xf


def level_of(ms):
    best = min(range(len(PLATEAUS_MS)), key=lambda i: abs(PLATEAUS_MS[i] - ms))
    return best if abs(PLATEAUS_MS[best] - ms) <= 0.25 else None


def events(d):
    ev = {}
    p = os.path.join(d, "events.tsv")
    if os.path.exists(p):
        for line in open(p):
            t, what, *_ = line.rstrip("\n").split("\t")
            ev.setdefault(what, int(t))
    return ev


def trace_writes(path):
    """(walltimestamp, value) of every map-113 offset-0x1200 write, and the aneWorkBegin timestamps."""
    ws, begins = [], []
    if not path or not os.path.exists(path):
        return ws, begins
    for line in open(path, errors="replace"):
        if line.startswith("B ") and "aneWorkBegin" in line:
            begins.append(int(re.search(r"w=(\d+)", line).group(1)))
        elif line.startswith("W ") and "ApplePMGR10writeReg32" in line:
            kv = dict(re.findall(r"(\w+)=(\S+)", line))
            if int(kv.get("a1", "0"), 0) == 113 and int(kv.get("a2", "0"), 0) == 0x1200:
                ws.append((int(kv["w"]), int(kv["a3"], 0)))
    return ws, begins


def ramp(d, trace=None):
    """One ramp directory: the token series, changes, chain check, call alignment, verdict inputs."""
    ss = [s for p in sorted(glob.glob(os.path.join(d, "**", "series.bin"), recursive=True)) for s in read_series(p)]
    ev = events(d)
    rj = os.path.join(d, "runner.json")
    ex = json.load(open(rj)).get("exec_ms", []) if os.path.exists(rj) and os.path.getsize(rj) else []
    ws, begins = trace_writes(trace)
    up = [s for s in ss if s["islands_up"]]
    t_up = up[0]["t0"] if up else None
    reads = [(s["t0"], s["ranges"][WORD]) for s in ss if WORD in s["ranges"]]
    ok = [(t, r[2][0]) for t, r in reads if r[1] == "ok"]
    status = Counter(r[1] for _, r in reads)
    # call boundaries: aneWorkBegin anchors if traced, else power-on + the runner's per-call ms end to end
    if begins and ev.get("runner-spawn"):
        bounds = [b for b in begins if ev["runner-spawn"] <= b <= ev.get("runner-exit", b)]
    elif t_up and ex:
        acc, bounds = t_up, []
        for ms in ex:
            bounds.append(acc)
            acc += int(ms * 1e6)
    else:
        bounds = []
    changes, last = [], None
    for t, v in ok:
        if v != last:
            trig, prev, new = decode(v)
            call = bisect.bisect_right(bounds, t) - 1 if bounds else -1
            lvl = level_of(ex[call]) if 0 <= call < len(ex) else None
            nxt = level_of(ex[call + 1]) if 0 <= call + 1 < len(ex) else None
            changes.append({"t_ms": round((t - (t_up or t)) / 1e6, 3), "value": hex(v), "trigger": trig, "prev": prev,
                            "new": new, "call": call, "level": lvl, "next_level": nxt,
                            "chain_ok": last is None or prev == decode(last)[2]})
            last = v
    distinct = [c["value"] for c in changes]
    news = [c["new"] for c in changes if c["trigger"]]
    rising = news[:news.index(max(news)) + 1] if news else []
    out = {"dir": os.path.basename(d), "dumps": len(ss), "up_dumps": len(up), "status": dict(status),
           "distinct_values": distinct, "changes": changes[:60],
           "chain_consistent": all(c["chain_ok"] for c in changes[1:]) if len(changes) > 1 else None,
           "rising_monotone": rising == sorted(rising) if rising else None,
           "state_matches_plateau": [(c["new"], c["next_level"]) for c in changes if c["next_level"] is not None][:12],
           "expected_subsequence": all(x in [int(v, 16) for v in distinct] for x in EXPECTED[:3]) if distinct else False,
           "spawn_to_up_ms": round((t_up - ev["runner-spawn"]) / 1e6, 1) if t_up and "runner-spawn" in ev else None,
           "dtrace_writes": [(hex(v), round((t - (t_up or t)) / 1e6, 3)) for t, v in ws
                             if "runner-spawn" in ev and ev["runner-spawn"] <= t <= ev.get("runner-exit", t)][:20],
           "first_calls_ms": [round(x, 3) for x in ex[:12]]}
    if ws and ok:  # read value seen within 20 ms after each traced write?
        seen = []
        for t, v in ws:
            after = [val for (tr, val) in ok if t <= tr <= t + 20_000_000]
            seen.append((hex(v), hex(after[0]) if after else None, v in after))
        out["write_then_read"] = seen[:20]
    return out


def analyze(outdir):
    w4 = os.path.join(outdir, "w4")
    acc = os.path.join(w4, "accept.txt")
    accept = open(acc).read().strip().splitlines() if os.path.exists(acc) else []
    refused = os.path.exists(os.path.join(w4, "REFUSED")) or any("rejected" in l or "too many" in l for l in accept)
    res = {"accept": accept, "refused": refused, "first": {}, "ramps": []}
    for d in sorted(glob.glob(os.path.join(w4, "first-*"))):
        ss = [s for p in sorted(glob.glob(os.path.join(d, "**", "series.bin"), recursive=True)) for s in read_series(p)]
        res["first"][os.path.basename(d)] = [
            {"islands_up": s["islands_up"], "ps": [hex(x) for x in s["ps"]],
             "words": {n: (st, [hex(x) for x in w]) for n, (_pa, st, w) in s["ranges"].items() if n != "fabric-ps" and n != "dcs-ps"}}
            for s in ss]
    for d in sorted(glob.glob(os.path.join(w4, "ramp-*"))) + [p for p in [os.path.join(w4, "idle-tail")] if os.path.isdir(p)]:
        res["ramps"].append(ramp(d, os.path.join(d, "trace.out")))
    ok_any = any(r["status"].get("ok") for r in res["ramps"]) or any(
        st == "ok" for f in res["first"].values() for s in f for st, _ in s["words"].values())
    tracks = [r["dir"] for r in res["ramps"] if len(r["distinct_values"]) >= 3 and r["chain_consistent"] and r["rising_monotone"]]
    if refused:
        res["verdict"] = "REFUSED: the kext rule rejected the window-4 request (no read happened)"
    elif not ok_any:
        res["verdict"] = "GATED: no read of the word ever had status ok (islands never up while sampled, or every dump gated)"
    elif tracks:
        res["verdict"] = f"TRACKS: {WORD} at 0x285869200 follows the DVFS token chain in {tracks}; the RegMap 113 -> reg[41] PA derivation is confirmed"
    elif all(len(r["distinct_values"]) <= 1 for r in res["ramps"] if r["status"].get("ok")):
        vals = sorted({v for r in res["ramps"] for v in r["distinct_values"]})
        res["verdict"] = f"STATIC: readable but constant ({vals}): a write-only trigger, a different PA, or the token is not latched"
    else:
        res["verdict"] = "UNRESOLVED: values change but the chain or the order does not hold; read the changes table"
    return res


def report(r):
    print("accept:", " | ".join(r["accept"]))
    for name, dumps in r["first"].items():
        print(f"== {name}: " + "; ".join(f"up={d['islands_up']} " + " ".join(f"{n}={st}:{w}" for n, (st, w) in d["words"].items()) for d in dumps))
    for rp in r["ramps"]:
        print(f"== {rp['dir']}: dumps={rp['dumps']} up={rp['up_dumps']} status={rp['status']} spawn_to_up={rp['spawn_to_up_ms']}ms "
              f"distinct={rp['distinct_values']} chain={rp['chain_consistent']} rising={rp['rising_monotone']} "
              f"expected_subseq={rp['expected_subsequence']} first_calls={rp['first_calls_ms'][:6]}")
        for c in rp["changes"][:20]:
            print(f"   +{c['t_ms']:9.3f}ms {c['value']} trig={int(c['trigger'])} prev={c['prev']} new={c['new']} "
                  f"call={c['call']} lvl={c['level']}->{c['next_level']} chain_ok={c['chain_ok']}")
        if rp["dtrace_writes"]:
            print(f"   dtrace writes: {rp['dtrace_writes']}")
        if rp.get("write_then_read"):
            print(f"   write->read within 20 ms: {rp['write_then_read']}")
    print(f"ladder: states 0..6 = {LADDER_MHZ} MHz, plateaus {PLATEAUS_MS} ms (round 2; assignment INFERENCE)")
    print("VERDICT:", r["verdict"])


def _rec(t0, up, opp_val, opp_status):
    names = ["fabric-ps", "dcs-ps", WORD, "ctx-1000", "ctx-2000"]
    rngs, data = [], b""
    for i, n in enumerate(names):
        st = 0 if (n in ("fabric-ps", "dcs-ps") or (up and opp_status == 0)) else 1
        ln = 4 if st == 0 else 0
        rngs.append(RANGE.pack(n.encode(), 0x285869200 if n == WORD else 0x28e20c000 + i, 0, ln, len(data), st, 0, 0, 0))
        if ln:
            data += (0x666 if n == "fabric-ps" else 0x999 if n == "dcs-ps" else opp_val if n == WORD else 0).to_bytes(4, "little")
    ps = [0x1f0003ff if up else 0x0f000300] * 2 + [0x3ff if up else 0x300] * 6
    hdr = HDR.pack(0x414e4531, 2, HDR.size + 40 * RANGE.size, len(names), len(data), int(up), *ps, 0, 0, *ps, 1, 0, 0x28e080000)
    body = hdr + b"".join(rngs) + b"\0" * ((40 - len(names)) * RANGE.size) + data
    return REC.pack(t0, t0 + 900_000, 0, len(body)) + body


def synthetic(kind, tmp):
    w4 = os.path.join(tmp, "out", "w4")
    os.makedirs(w4, exist_ok=True)
    open(os.path.join(w4, "accept.txt"), "w").write("rejected poll=10 ...\n" if kind == "refused" else
                                                     f"accepted {WORD} pa=0x285869200 len=4 gated\naccepted nranges=5\n")
    t = 1_800_000_000_000_000_000
    d = os.path.join(w4, "first-opp-idle", "s")
    os.makedirs(d)
    open(os.path.join(d, "series.bin"), "wb").write(_rec(t, False, 0, 1))
    d = os.path.join(w4, "first-opp-load", "a1")
    os.makedirs(d)
    open(os.path.join(d, "series.bin"), "wb").write(_rec(t + 1_000_000_000, True, 0x80000012, 1 if kind == "gated" else 0))
    spawn = t + 10_000_000_000
    exec_ms = [3.91] * 2 + [2.70] * 4 + [2.10] * 2 + [1.77] * 2 + [1.54] * 4 + [1.34] * 1 + [1.22] * 25
    seq = {0: 0x80000000, 2: 0x80000001, 3: 0x80000012, 7: 0x80000023, 9: 0x80000034, 11: 0x80000045, 15: 0x80000056}
    recs, t_up = [], spawn + 3_300_000_000
    t_call = []
    acc = t_up
    for ms in exec_ms:
        t_call.append(acc)
        acc += int(ms * 1e6)
    t_now, cur = spawn, 0
    while t_now < acc + 4_000_000_000:
        up = t_up <= t_now < acc + 100_000_000
        call = bisect.bisect_right(t_call, t_now) - 1 if up else -1
        if kind == "tracks":
            for k in sorted(seq):
                if call >= k:
                    cur = seq[k]
        elif kind == "static":
            cur = 0x80000000
        recs.append(_rec(t_now, up, cur, 1 if kind == "gated" else 0))
        t_now += 2_000_000
    rd = os.path.join(w4, "ramp-1")
    os.makedirs(os.path.join(rd, "s"))
    open(os.path.join(rd, "s", "series.bin"), "wb").write(b"".join(recs))
    open(os.path.join(rd, "events.tsv"), "w").write(f"{spawn}\trunner-spawn\tx\n{acc + 50_000_000}\trunner-exit\trc=0\n")
    json.dump({"exec_ms": exec_ms}, open(os.path.join(rd, "runner.json"), "w"))
    if kind == "tracks":  # a dtrace of the same ramp: writes 0.4 ms into each call that changes the token
        lines = [f"B w={tc} f=_ZN4clpc4CLPC12aneWorkBeginERNS" for tc in t_call]
        lines += [f"W w={t_call[k] + 400_000} f=_ZN9ApplePMGR10writeReg32ENS_6RegMapEjjj d=1 a0=0x1 a1=0x71 a2=0x1200 a3={v:#x} a4=0x3"
                  for k, v in sorted(seq.items())]
        open(os.path.join(rd, "trace.out"), "w").write("\n".join(sorted(lines, key=lambda l: int(l.split("w=")[1].split()[0]))) + "\n")
    return analyze(os.path.join(tmp, "out"))


def selfcheck():
    with tempfile.TemporaryDirectory() as tmp:
        r = synthetic("tracks", tmp)
        report(r)
        assert r["verdict"].startswith("TRACKS"), r["verdict"]
        rp = r["ramps"][0]
        assert rp["distinct_values"] == [hex(v) for v in (0x80000000, 0x80000001, 0x80000012, 0x80000023, 0x80000034, 0x80000045, 0x80000056)], rp["distinct_values"]
        assert rp["chain_consistent"] and rp["rising_monotone"] and rp["expected_subsequence"], rp
        assert [c["call"] for c in rp["changes"]] == [0, 2, 3, 7, 9, 11, 15], [c["call"] for c in rp["changes"]]
        assert rp["changes"][2]["prev"] == 1 and rp["changes"][2]["new"] == 2, rp["changes"][2]
        assert all(ok for _, _, ok in rp["write_then_read"]), rp["write_then_read"]
        assert r["first"]["first-opp-idle"][0]["words"][WORD][0] == "gated", r["first"]
        assert r["first"]["first-opp-load"][0]["words"][WORD] == ("ok", ["0x80000012"]), r["first"]
    with tempfile.TemporaryDirectory() as tmp:
        r = synthetic("static", tmp)
        assert r["verdict"].startswith("STATIC"), r["verdict"]
    with tempfile.TemporaryDirectory() as tmp:
        r = synthetic("gated", tmp)
        assert r["verdict"].startswith("GATED"), r["verdict"]
    with tempfile.TemporaryDirectory() as tmp:
        r = synthetic("refused", tmp)
        assert r["verdict"].startswith("REFUSED"), r["verdict"]
    print("selfcheck OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--synthetic", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--json")
    a = ap.parse_args()
    if a.synthetic:
        selfcheck()
        return 0
    if not a.out:
        ap.error("--out or --synthetic")
    r = analyze(a.out)
    report(r)
    if a.json:
        json.dump(r, open(a.json, "w"), indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
