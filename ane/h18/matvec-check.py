#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check the ANE matvec program run on a T8152 against a CPU reference.

  matvec-check.py gen-input [-o FILE]       write the deterministic input buffer (feed it to ane-prog-pack --in 5=FILE)
  matvec-check.py check DMESG [--input FILE]  compare the output buffer dump in DMESG with the CPU result

The program is fixtures/h14-anec/matvec: y[1,256] = x[1,256] . W[256,256], fp16, M=1 K=256 N=256. The device reads the input
buffer (id 5) and writes the output buffer (id 4, 0x4000 bytes): the first 256 fp16 are y, the rest is padding and must be 0.

The CPU weights are not invented. They are the constant section of the fixture (kernel.bin, 128 KiB = 256*256*2), un-permuted
with Apple's weight packing rule (research/h14-td-fields.md in mil-hwx-compiler, proven by 125 minted probes): with
g = min(16, N/16), planes = N/g, p = n//g, q = (p%16)*(planes/16) + p//16, W[n][k] is halfword q*K*g + k*g + n%g.

Load the module with hs_prog=1 prog_calls=M prog_out_words=N (N >= 16 + the 256 fp16 = 128 words; 4096 dumps the whole buffer).
It logs, per call, "ane_h18 prog: call I/M finished in X ns" and "ane_h18 prog: out buf 4 +0xOFF: <words>". X is the time
the module's poll loop saw the finish event after it rang the doorbell: poll granularity and interrupt latency are in it. It is
not a hardware time.

check compares each call with the CPU result rounded to fp16. A lane passes at 2 ulp of the rounded exact value, or within 4
condition units (one unit is 2^-11 * sum|terms|), the band tools/ane_check_band.h uses for ane-run --check matvec. It prints
PASS or FAIL, the max abs error, the module's per-call ns (median, min, max) and the CPU time of the same matvec. Exit status
0 for PASS, 1 for FAIL, 2 for bad arguments.
"""
import argparse
import math
import os
import re
import statistics
import struct
import sys
import time

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fixtures", "h14-anec", "matvec", "kernel.bin")
K = N = 256
SURFACE = 0x4000
OUT_BUF = 4

CALL_RE = re.compile(r"ane_h18 prog: call (\d+)/(\d+) (?:finished in (\d+) ns|gave no finish event)")
OUT_RE = re.compile(r"ane_h18 prog: out buf (\d+) \+(0x[0-9a-f]+|\d+): ((?:[0-9a-f]{8} ?)+)")


def make_input():
    """x[k] = ((37k + 11) mod 257 - 128) / 128: exact in fp16, spread over -1..1 so a wrong lane shows."""
    return [((37 * k + 11) % 257 - 128) / 128 for k in range(K)]


def input_file(x):
    return struct.pack("<%de" % K, *x) + bytes(SURFACE - 2 * K)


def unpermute(kernel):
    """W[n][k] as floats from the constant section, by Apple's weight packing (see the module docstring)."""
    g = min(16, N // 16)
    planes = N // g
    if N % 256 or K % 16 or len(kernel) < K * N * 2:
        sys.exit("matvec-check: the weight packing rule is proven for N a multiple of 256 and a full %d byte section" % (K * N * 2))
    half = struct.unpack("<%de" % (K * N), kernel[:K * N * 2])
    w = []
    for n in range(N):
        p = n // g
        q = (p % 16) * (planes // 16) + p // 16
        base = q * K * g + n % g
        w.append([half[base + k * g] for k in range(K)])
    return w


def ulp(v):
    m, e = math.frexp(abs(v))
    return 2.0 ** -24 if v == 0 or abs(v) < 2.0 ** -14 else 2.0 ** (e - 11)


def reference(x, w):
    """Exact (double) y[n] and sum|x[k] W[n][k]| for each n."""
    return [(sum(a * b for a, b in zip(x, row)), sum(abs(a * b) for a, b in zip(x, row))) for row in w]


def lane_ok(got, exact, sumabs):
    """The tools/ane_check_band.h rule; returns (ok, abs error)."""
    err = abs(got - exact)
    if not math.isfinite(got):
        return False, math.inf
    if sumabs == 0.0:
        return err == 0.0, err
    want = struct.unpack("<e", struct.pack("<e", exact))[0]
    return abs(got - want) / ulp(want) <= 2.0 or err / (sumabs * 2.0 ** -11) <= 4.0, err


def parse_dmesg(text, buf):
    """-> (calls, outs): calls[i] = ns or None (no finish event); outs[i] = {byte offset: [u32 words]} of buffer buf for call i."""
    calls, outs = [], []
    declared = 0
    for line in text.splitlines():
        m = CALL_RE.search(line)
        if m:
            calls.append(int(m.group(3)) if m.group(3) else None)
            outs.append({})
            declared = int(m.group(2))
            continue
        m = OUT_RE.search(line)
        if m and calls and int(m.group(1)) == buf:
            outs[-1][int(m.group(2), 0)] = [int(t, 16) for t in m.group(3).split()]
    return calls, outs, declared


def lanes(chunks):
    """The u32 words of a buffer dump from offset 0, or None if the dump has a gap."""
    words = []
    for off in sorted(chunks):
        if off != 4 * len(words):
            return None
        words += chunks[off]
    return words


def check_call(words, ref):
    """-> (problem or None, max abs error over the lanes)."""
    if words is None or len(words) < N // 2:
        return "dump has %d words, need %d" % (len(words or []), N // 2), math.nan
    raw = struct.pack("<%dI" % len(words), *words)
    got = struct.unpack("<%de" % N, raw[:2 * N])
    bad, worst = [], 0.0
    for n, (g, (exact, sumabs)) in enumerate(zip(got, ref)):
        ok, err = lane_ok(g, exact, sumabs)
        worst = max(worst, err)
        if not ok:
            bad.append(n)
    if bad:
        return "%d lanes out of band, first lane %d" % (len(bad), bad[0]), worst
    if any(raw[2 * N:]):
        return "padding after the %d lanes is not zero" % N, worst
    return None, worst


def cpu_time(x, w):
    """ns per matvec call on this CPU, by numpy when installed, else the stdlib. -> (backend, [ns per call])"""
    try:
        import numpy
        a, v, reps, backend = numpy.array(w, dtype=numpy.float32), numpy.array(x, dtype=numpy.float32), 200, "numpy fp32"
        run = lambda: a @ v
    except ImportError:
        reps, backend = 20, "python float"
        run = lambda: [sum(map(float.__mul__, x, row)) for row in w]
    out = []
    for _ in range(reps):
        t = time.perf_counter_ns()
        run()
        out.append(time.perf_counter_ns() - t)
    return backend, out


def cmd_check(a):
    kernel = open(a.kernel, "rb").read()
    x = struct.unpack("<%de" % K, open(a.input, "rb").read()[:2 * K]) if a.input else make_input()
    w = unpermute(kernel)
    ref = reference(x, w)
    calls, outs, declared = parse_dmesg(open(a.dmesg, errors="replace").read(), a.buf)
    problems, worst = [], 0.0
    if not calls:
        problems.append("no 'ane_h18 prog: call I/M' line in the log")
    if len(calls) != declared:
        problems.append("log has %d of %d calls" % (len(calls), declared))
    for i, chunks in enumerate(outs):
        p, err = check_call(lanes(chunks), ref)
        worst = max(worst, err) if not math.isnan(err) else worst
        if p:
            problems.append("call %d: %s" % (i + 1, p))
        if calls[i] is None:
            problems.append("call %d: no finish event" % (i + 1))
    ns = [c for c in calls if c is not None]
    print("matvec M=1 K=%d N=%d fp16: %d calls, max abs error %.6g" % (K, N, len(calls), worst))
    if ns:
        print("device ns per call (poll loop, not hardware time): median %d min %d max %d" % (statistics.median(ns), min(ns), max(ns)))
    backend, cpu = cpu_time(x, w)
    print("cpu ns per call (%s, %d runs): median %d min %d max %d" % (backend, len(cpu), statistics.median(cpu), min(cpu), max(cpu)))
    for p in problems:
        print("  " + p)
    print("FAIL" if problems else "PASS")
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-input")
    g.add_argument("-o", dest="out", default="matvec-in.f16")
    c = sub.add_parser("check")
    c.add_argument("dmesg")
    c.add_argument("--input", help="the input buffer file given to ane-prog-pack (default: the gen-input vector)")
    c.add_argument("--kernel", default=FIXTURE, help="the constant section (default: fixtures/h14-anec/matvec/kernel.bin)")
    c.add_argument("--buf", type=int, default=OUT_BUF, help="output buffer id (default 4)")
    a = ap.parse_args()
    if a.cmd == "gen-input":
        open(a.out, "wb").write(input_file(make_input()))
        print("%s: %d bytes, x[0..3] = %s" % (a.out, SURFACE, make_input()[:4]))
        return 0
    return cmd_check(a)


if __name__ == "__main__":
    sys.exit(main())
