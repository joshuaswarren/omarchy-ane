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

Load the module with hs_prog=1 prog_calls=M prog_out_words=4096: the dump must cover the whole buffer (4096 words) so the padding is checked.
It logs, per call, "ane_h18 prog: call I/M finished in X ns" and "ane_h18 prog: out buf 4 +0xOFF: <words>". X is the time
the module's poll loop saw the finish event after it rang the doorbell: poll granularity and interrupt latency are in it. It is
not a hardware time.

check compares each call with the CPU result rounded to fp16. A lane passes within 2 ulp of the rounded exact value plus an
absolute floor of 2^-13. The band is relative to the lane, not to the sum of its terms (the 4 condition units of ane-run
--check matvec): with this data a condition-unit band is as wide as a whole dropped product. 2^-13 is half the smallest nonzero
product (|x| >= 2^-7 times |W| >= 2^-5), so a lane that misses any one product is outside it. A device that accumulates in
fp16 may need a wider band; the output prints the max error in ulp so that can be judged from a real run. It prints
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

CALL_RE = re.compile(r"ane_h18 prog: call (\d+)/(\d+) (?:finished in (\d+) ns|(gave no finish event)|finished, the finish event was not seen)")
FLOOR = 2.0 ** -13
OUT_RE = re.compile(r"ane_h18 prog: out buf (\d+) \+(0x[0-9a-f]+|\d+): ((?:[0-9a-f]{8} ?)+)")


def make_input():
    """x[k] = ((37k + 11) mod 257 - 128) / 128, the residue 0 replaced by +1/128: exact in fp16, spread over -1..1, and never 0. A zero input would make the
    product of that k undetectable in every lane (a dropped x[k]*W[n][k] changes no result), so the generator refuses to emit one. The residues step by 37 over
    the odd modulus 257, so they cover -128..128 once and the zero falls at k = 24; the replacement keeps |x| >= 2^-7, the floor the tolerance reasoning uses."""
    out = []
    for k in range(K):
        r = (37 * k + 11) % 257 - 128
        out.append((1 if r == 0 else r) / 128)
    return out


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
    """Exact (double) y[n] for each n."""
    return [sum(a * b for a, b in zip(x, row)) for row in w]


def lane_ok(got, exact):
    """Within 2 ulp of the exact value rounded to fp16, plus FLOOR; returns (ok, abs error, error in ulp of the rounded value)."""
    if not math.isfinite(got):
        return False, math.inf, math.inf
    want = struct.unpack("<e", struct.pack("<e", exact))[0]
    d = abs(got - want)
    return d <= 2.0 * ulp(want) + FLOOR, abs(got - exact), d / ulp(want)


def parse_dmesg(text, buf):
    """-> (calls, outs, declared): calls[i] = (ns or None, finished); outs[i] = {byte offset: [u32 words]} of buffer buf for call i."""
    calls, outs = [], []
    declared = 0
    for line in text.splitlines():
        m = CALL_RE.search(line)
        if m:
            calls.append((int(m.group(3)) if m.group(3) else None, not m.group(4)))
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
    """-> (problem or None, max abs error, max error in ulp over the lanes)."""
    if words is None or len(words) < SURFACE // 4:
        return "dump has %d words, need %d (prog_out_words=%d): the padding would go unchecked" % (len(words or []), SURFACE // 4, SURFACE // 4), math.nan, math.nan
    raw = struct.pack("<%dI" % len(words), *words)
    got = struct.unpack("<%de" % N, raw[:2 * N])
    bad, worst, worst_ulp = [], 0.0, 0.0
    for n, (g, exact) in enumerate(zip(got, ref)):
        ok, err, u = lane_ok(g, exact)
        worst, worst_ulp = max(worst, err), max(worst_ulp, u)
        if not ok:
            bad.append(n)
    if bad:
        return "%d lanes out of band, first lane %d" % (len(bad), bad[0]), worst, worst_ulp
    if any(raw[2 * N:]):
        return "padding after the %d lanes is not zero" % N, worst, worst_ulp
    return None, worst, worst_ulp


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
    problems, worst, worst_ulp = [], 0.0, 0.0
    if not calls:
        problems.append("no 'ane_h18 prog: call I/M' line in the log")
    if len(calls) != declared:
        problems.append("log has %d of %d calls" % (len(calls), declared))
    for i, chunks in enumerate(outs):
        p, err, u = check_call(lanes(chunks), ref)
        if not math.isnan(err):
            worst, worst_ulp = max(worst, err), max(worst_ulp, u)
        if p:
            problems.append("call %d: %s" % (i + 1, p))
        if not calls[i][1]:
            problems.append("call %d: no finish event" % (i + 1))
    ns = [c for c, _ in calls if c is not None]
    print("matvec M=1 K=%d N=%d fp16: %d calls, max abs error %.6g (%.3g ulp)" % (K, N, len(calls), worst, worst_ulp))
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
