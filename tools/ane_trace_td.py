#!/usr/bin/env python3
"""Per-task timeline of CALLs traced with ane_t6021 trace_td, joined with
the H14 task descriptors of the program's ANEC.

usage: ane_trace_td.py TRACE [ANEC]

TRACE is the debugfs file ane_t6021/trace_td copied after the run (the
whole blob, or its header plus the used records). Layout: header <6I + Q
(magic "ATD1", rec_size, capacity, n, dropped, calls), then n records
<QIHH (ktime ns, word, kind, call). Kinds: 1 CALL, 2 ACK, 3 TD (TD word:
nid bits 23:16, index of the last task taken bits 15:0), 4 EVENT (IO_T2H
state), 5 GATE, 6 DONE (word = TD samples taken).

A task's take time is when the TD word first showed an index >= its own;
when one sample moves the index by more than one, the tasks in between get
times spread evenly over that sample interval. The TD word runs ahead of
execution: the task manager keeps a window of D tasks in flight, so task
k+1 is taken when task k+1-D has finished. The script finds D as the lag
at which the gaps of the take curve best follow per-task work, and reports
the run time of task j as T(j+D) - T(j+D-1).
"""
import collections
import statistics as st
import struct
import sys

import numpy as np

CALL, ACK, TD, EVENT, GATE, DONE = range(1, 7)


def read_trace(path):
    b = open(path, "rb").read()
    magic, rs, cap, n, dropped, calls = struct.unpack_from("<6I", b, 0)
    assert magic == 0x31445441 and rs == 16, (hex(magic), rs)
    recs = [struct.unpack_from("<QIHH", b, 32 + 16 * i) for i in range(n)]
    return dict(cap=cap, n=n, dropped=dropped, calls=calls), recs


def census(path):
    """Per-task rows from an ANEC (or a header + stream copy)."""
    with open(path, "rb") as f:
        head = f.read(0x1000)
        stream = f.read(struct.unpack_from("<Q", head, 0x10)[0])
    state, rows, off = {}, [], 0
    while off < len(stream):
        words = struct.unpack_from("<H", stream, off + 2)[0] & 0x7FF
        if not words:
            off += 16
            continue
        tw = struct.unpack_from(f"<{words}I", stream, off)
        regs, i = {}, 9 if tw[7] & 3 == 3 else 8
        while i < words:
            h = tw[i]
            r = (h & 0x7FFF) * 4
            if h & 0x80000000:
                m = (h >> 15) & 0xFFFF
                written = [r] + [r + 4 * (b + 1) for b in range(16) if m >> b & 1]
            else:
                written = [r + 4 * k for k in range(((h >> 15) & 0x3F) + 1)]
            regs.update(zip(written, tw[i + 1:i + 1 + len(written)]))
            i += 1 + len(written)
        state.update(regs)
        s = state
        kd = any(0x1900 <= r <= 0x19D4 for r in regs)
        inW, inH = s[0] & 0x7FFF, (s[0] >> 16) & 0x7FFF
        outW, outH = s[0x14] & 0x7FFF, (s[0x14] >> 16) & 0x7FFF
        inC, outC = s[0xC], s[0x10]
        ne = s.get(0x3C) == 0x100000
        src = regs.get(0x1100, 0) & 1
        dst = regs.get(0x1500, 0) & 1
        rows.append(dict(
            idx=tw[0] & 0xFFFF, ne=ne, inC=inC, outC=outC, outW=outW, outH=outH,
            macs=inC * outC * outW * outH if ne else 0,
            kbytes=sum(s.get(0x1998 + 4 * c, 0) for c in range(16)
                       if s.get(0x1918 + 4 * c, 0) & 1) if kd else 0,
            sbytes=inC * inW * inH * 2 if src else 0,
            dbytes=outC * outW * outH * 2 if dst else 0))
        off = (off + words * 4 + 15) & ~15
    return rows


def per_call(recs):
    calls = collections.defaultdict(list)
    for t, w, k, c in recs:
        calls[c].append((t, w, k))
    out = []
    for c in sorted(calls):
        r = calls[c]
        ack = next((t for t, w, k in r if k == ACK), None)
        t_call = next((t for t, w, k in r if k == CALL), None)
        tds = [(t, w) for t, w, k in r if k == TD]
        ev = [(t, w) for t, w, k in r if k == EVENT]
        done = [(t, w) for t, w, k in r if k == DONE]
        gates = [(t, w) for t, w, k in r if k == GATE]
        if ack is None or not tds:
            continue
        nid = (tds[-1][1] >> 16) & 0xFF
        own = [(t, w & 0xFFFF) for t, w in tds if (w >> 16) & 0xFF == nid]
        stale = [w for t, w in tds if (w >> 16) & 0xFF != nid]
        fin = next((t for t, w in ev if w == 1), None)
        ev0 = next((t for t, w in ev if w == 0), None)
        out.append(dict(call=c, t_call=t_call, ack=ack, own=own, stale=stale, fin=fin, ev0=ev0,
                        done=done[-1] if done else None, gates=gates))
    return out


def take_times(cl, ntask):
    """Per call: estimated take time (ms after the ack) of every task."""
    times = np.full(ntask, np.nan)
    prev_t, prev_i = cl["ack"], -1
    for t, i in cl["own"]:
        if i <= prev_i:
            continue
        span = i - prev_i
        for k in range(prev_i + 1, min(i, ntask - 1) + 1):
            frac = (k - prev_i) / span
            times[k] = (prev_t + frac * (t - prev_t) - cl["ack"]) / 1e6
        prev_t, prev_i = t, i
    return times


def report(path, anec=None):
    hdr, recs = read_trace(path)
    print(f"== {path}: n {hdr['n']} dropped {hdr['dropped']} calls {hdr['calls']} capacity {hdr['cap']}")
    cls = per_call(recs)
    if not cls:
        print("no call with ACK and TD records")
        return
    ntask = max(i for cl in cls for t, i in cl["own"]) + 1
    rows = census(anec) if anec else None
    if rows:
        assert len(rows) == ntask, (len(rows), ntask)
    print(f"tasks {ntask}, traced calls {len(cls)}")

    def stat(xs, f="{:.3f}"):
        xs = [x for x in xs if x is not None]
        return (f"min {f.format(min(xs))} median {f.format(st.median(xs))} max {f.format(max(xs))}"
                if xs else "n/a")

    fin = [(c["fin"] - c["ack"]) / 1e6 if c["fin"] else None for c in cls]
    ev0 = [(c["ev0"] - c["ack"]) / 1e6 if c["ev0"] else None for c in cls]
    exch = [(c["ack"] - c["t_call"]) / 1e6 if c["t_call"] else None for c in cls]
    last = [(c["own"][-1][0] - c["ack"]) / 1e6 for c in cls]
    lastidx = [c["own"][-1][1] for c in cls]
    samples = [c["done"][1] for c in cls if c["done"]]
    waitms = [(c["done"][0] - c["ack"]) / 1e6 for c in cls if c["done"]]
    seen = [len({i for t, i in c["own"]}) / ntask for c in cls]
    print(f"CALL->ACK ms: {stat(exch)}")
    print(f"ACK->finish event (state 1) ms: {stat(fin)}")
    print(f"ACK->state-0 event ms: {stat(ev0)}")
    print(f"ACK->last TD index first seen ms: {stat(last)}; last index {stat(lastidx, '{:.0f}')}")
    print(f"finish - last-index sighting ms: {stat([f - l for f, l in zip(fin, last) if f is not None])}")
    print(f"TD samples per call: {stat(samples, '{:.0f}')}; mean sample interval us: "
          f"{stat([w * 1e3 / s for w, s in zip(waitms, samples) if s], '{:.1f}')}")
    print(f"distinct indices seen per call (fraction of tasks): {stat(seen)}")
    print(f"stale TD values before the call's own (previous nid): "
          f"{collections.Counter(len(c['stale']) for c in cls)}; gate misses: {sum(len(c['gates']) for c in cls)}")
    if ntask < 3:
        return

    T = np.array([take_times(c, ntask) for c in cls])
    Tm = np.nanmean(T, axis=0)
    finm = st.median([f for f in fin if f is not None])
    print("\nCumulative curve (mean over calls; time in ms after the ACK):")
    print("  tasks taken | time ms | share of ACK->finish")
    for p in range(0, 101, 10):
        k = min(ntask - 1, int(round(p / 100 * (ntask - 1))))
        print(f"  {p:3d}% (task {k:5d}) | {Tm[k]:8.3f} | {Tm[k] / finm * 100:5.1f}%")
    print("  time decile | tasks taken in it | rate tasks/ms")
    for d in range(10):
        lo, hi = d * finm / 10, (d + 1) * finm / 10
        n = int(np.sum((Tm >= lo) & (Tm < hi)))
        print(f"  {d * 10:3d}-{d * 10 + 10:3d}% ({lo:8.3f}-{hi:8.3f} ms) | {n:5d} | {n / (hi - lo):8.2f}")

    gap = np.diff(Tm)  # gap[k] = T(k+1) - T(k)
    print(f"\nTail after the last task was taken (finish event - T(last)): {(finm - Tm[-1]) * 1e3:.1f} us")
    stands = collections.Counter()
    for c in cls:
        prev_t, prev_i = c["ack"], -1
        for t, i in c["own"]:
            if (t - prev_t) / 1e6 > 0.5:
                stands[prev_i] += 1
            prev_t, prev_i = t, i
    print(f"TD index held > 0.5 ms (index: calls with it), most common: "
          f"{stands.most_common(12) if stands else 'none'}")
    big_gaps = np.sort(gap)[::-1]
    print(f"take gaps: > 0.15 ms {int(np.sum(gap > 0.15))}, > 0.5 ms {int(np.sum(gap > 0.5))}; "
          f"largest {np.round(big_gaps[:5] * 1e3, 1).tolist()} us")
    if not rows:
        return

    ne = np.array([r["ne"] for r in rows])
    macs = np.array([r["macs"] for r in rows], float)
    kb = np.array([r["kbytes"] for r in rows], float)
    tb = np.array([r["sbytes"] + r["dbytes"] for r in rows], float)
    work = {"kDMA": kb, "tile": tb, "kDMA+tile": kb + tb, "MAC": macs}
    # Window model: the TM holds D tasks in flight, so task k+1 is taken when
    # task k+1-D has finished, and gap[k] is the run time of task k+1-D.
    scan = []
    for D in range(1, min(80, ntask)):
        src = np.arange(ntask - 1) + 1 - D
        ok = src >= 0
        for lab, w in work.items():
            if w[src[ok]].std() > 0 and gap[ok].std() > 0:
                scan.append((np.corrcoef(gap[ok], w[src[ok]])[0, 1], D, lab))
    scan.sort(reverse=True)
    D = scan[0][1]
    print(f"\nWindow depth scan r(gap[k], work of task k+1-D), best 6: "
          f"{[(round(r, 3), d, lab) for r, d, lab in scan[:6]]}")
    for lab in work:
        rs = {d: r for r, d, l in scan if l == lab}
        print(f"  {lab}: r at D-1, D, D+1 = {[round(rs.get(d, float('nan')), 3) for d in (D - 1, D, D + 1)]}")
    ex = np.full(ntask, np.nan)
    ex[:ntask - D] = gap[D - 1:]  # run time of task j = T(j+D) - T(j+D-1)
    tail = finm - Tm[-1]
    print(f"Model D = {D}: run time of task j = T(j+{D}) - T(j+{D - 1}) for j < {ntask - D}; "
          f"the last {D} tasks share the tail {tail * 1e3:.1f} us. "
          f"Sum {np.nansum(ex):.3f} ms + tail + first take {Tm[D - 1]:.3f} ms = "
          f"{np.nansum(ex) + tail + Tm[D - 1]:.3f} ms vs ACK->finish {finm:.3f} ms")

    fin_j = np.append(Tm[D:], np.full(D, finm))  # finish of task j ~ T(j+D)
    print("\nExecution curve (task j finished ~ T(j+D)):")
    print("  tasks finished | time ms | share of ACK->finish")
    for p in range(10, 101, 10):
        k = min(ntask - 1, int(round(p / 100 * ntask)) - 1)
        print(f"  {p:3d}% (task {k:5d}) | {fin_j[k]:8.3f} | {fin_j[k] / finm * 100:5.1f}%")

    def desc(k):
        r = rows[k]
        return (f"{'NE' if r['ne'] else 'PE'} in{r['inC']}->{r['outC']} out{r['outW']}x{r['outH']}"
                f" MAC {r['macs'] / 1e6:.1f}M kDMA {r['kbytes'] / 1e6:.2f}MB tile {tb[k] / 1e6:.2f}MB")

    sdx = np.full(ntask, np.nan)
    sdx[:ntask - D] = np.nanstd(np.diff(T, axis=1), axis=0)[D - 1:]
    print(f"\nRun time per task (model): median {np.nanmedian(ex) * 1e3:.1f} us, mean {np.nanmean(ex) * 1e3:.1f} us")
    print("Longest tasks (run time mean over calls, sd across calls):")
    for k in np.argsort(-np.nan_to_num(ex))[:12]:
        print(f"  task {k:5d}: {ex[k] * 1e3:7.1f} us (sd {sdx[k] * 1e3:5.1f}) {desc(k)}")

    print("\nBy class (model run times; the last D tasks are left out):")
    print("  class | tasks | ms | us/task | GMAC/s est | kDMA GB/s | tile GB/s est")
    groups = collections.defaultdict(list)
    for k, r in enumerate(rows[:ntask - D]):
        groups[(r["ne"], r["inC"], r["outC"], r["outW"], r["outH"]) if r["ne"] else ("PE",)].append(k)
    for g, ks in sorted(groups.items(), key=lambda kv: -np.nansum(ex[kv[1]])):
        ks = np.array(ks)
        tms = np.nansum(ex[ks])
        if len(ks) < 10 and tms < 2:
            continue
        name = "PE-only" if g == ("PE",) else f"NE in{g[1]}->{g[2]} out{g[3]}x{g[4]}"
        s = tms / 1e3
        print(f"  {name:28s} | {len(ks):5d} | {tms:7.2f} | {tms / len(ks) * 1e3:6.1f} | "
              f"{macs[ks].sum() / s / 1e9:7.0f} | {kb[ks].sum() / s / 1e9:6.2f} | {tb[ks].sum() / s / 1e9:6.2f}")

    ok = ~np.isnan(ex)
    A = np.column_stack([np.ones(ntask), macs / 1e6, kb / 1e6, tb / 1e6])
    y = ex * 1e3
    for name, m in (("all", ok), ("NE", ok & ne), ("PE", ok & ~ne)):
        if m.sum() < 10:
            continue
        cf, *_ = np.linalg.lstsq(A[m], y[m], rcond=None)
        pr = A[m] @ cf
        r2 = 1 - np.sum((y[m] - pr) ** 2) / np.sum((y[m] - y[m].mean()) ** 2)
        print(f"Fit {name} ({m.sum()} tasks): run us = {cf[0]:.1f} + {cf[1]:.4f}*MMAC + {cf[2]:.2f}*MB_kDMA "
              f"+ {cf[3]:.2f}*MB_tile, R^2 {r2:.3f}")
    cf, *_ = np.linalg.lstsq(A[ok], y[ok], rcond=None)
    resid = y - A @ cf
    out = np.where(ok & (resid > np.maximum(100, 2 * (A @ cf))))[0]
    print(f"Tasks > 2x the all-task fit and > 100 us over it: {len(out)}; "
          f"their excess {np.sum(resid[out]) / 1e3:.2f} ms")
    for k in out[np.argsort(-resid[out])][:8]:
        print(f"  task {k:5d}: run {y[k]:7.1f} us, fit {y[k] - resid[k]:6.1f} us, {desc(k)}")
    for lab, w in (("kDMA", kb), ("tile", tb)):
        top = np.array([k for k in np.argsort(-w) if ok[k]][:20])
        bw = w[top] / (ex[top] * 1e-3)
        print(f"20 most {lab}-heavy tasks: {w[top].min() / 1e6:.2f}-{w[top].max() / 1e6:.2f} MB, run "
              f"{np.nanmin(ex[top]) * 1e3:.0f}-{np.nanmax(ex[top]) * 1e3:.0f} us, implied GB/s median "
              f"{np.nanmedian(bw) / 1e9:.1f} (min {np.nanmin(bw) / 1e9:.1f}, max {np.nanmax(bw) / 1e9:.1f})")
    top = np.array([k for k in np.argsort(-macs) if ok[k]][:20])
    print(f"20 most MAC-heavy tasks: {macs[top].min() / 1e6:.1f}-{macs[top].max() / 1e6:.1f} MMAC est, run "
          f"{np.nanmin(ex[top]) * 1e3:.0f}-{np.nanmax(ex[top]) * 1e3:.0f} us, GMAC/s median "
          f"{np.nanmedian(macs[top] / (ex[top] * 1e-3)) / 1e9:.0f}")


if __name__ == "__main__":
    report(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
