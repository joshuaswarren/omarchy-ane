#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check the ANE mul program run on a T8152 against a CPU reference.

  mul-check.py gen-input [-o PREFIX]          write PREFIX-a.f16 and PREFIX-b.f16 (feed them to ane-prog-pack --in 5=A.f16 --in 6=B.f16)
  mul-check.py check DMESG [--input PREFIX]   compare the output buffer dump in DMESG with the CPU result

The program is fixtures/h14-anec/mul: out = a * b elementwise, fp16, rounded half away from zero. That is
the rule ane-run --check mul applies on the M2 (products of two fp16 are exact before the rounding, so the
only rounding is the final one). The surface layout is in the ANEC header: channels 4, 5 and 6 all hold
n=1 c=512 h=1 w=1 with planeStride 0x40 -- 512 planes of 32 fp16 halves (0x8000 bytes per buffer).
Element j is half 32*j (byte 64*j) of each buffer; the other 31 halves of every plane are padding and
must be 0 in the inputs and the output. Channel plan: inputs 5 and 6, output 4 (the H14 contract,
inputCount=2 in the ANEC header, and the buffers ane-prog-pack lists: 5 and 6 in, 4 out).

Load the module with hs_prog=1 prog_calls=M prog_out_words=4096: the dump covers 4096 words = 8192
halves = planes 0..255 of 512. Every dumped half outside the 256 plane elements must be 0, and every
element must be within 2 ulp of the CPU product plus a 2^-15 floor: half the smallest nonzero product,
since |a[j]| and |b[j]| are at least 2^-7. Planes 256..511 are not dumped and not checked (8192 words
would cover them). It logs, per call, "ane_h18 prog: call I/M finished in X ns" and
"ane_h18 prog: out buf 4 +0xOFF: <words>". X is the time the module's poll loop saw the finish event
after it rang the doorbell: poll granularity and interrupt latency are in it. It is not a hardware time.
parse_dmesg and lanes are copied from ane/h18/matvec-check.py (branch w91/ane-h18-matvec-check); the
module logs the same lines.

check prints PASS or FAIL, the max abs error, the module's per-call ns (median, min, max) and the CPU
time of the same 512-multiply op. Exit status 0 for PASS, 1 for FAIL, 2 for bad arguments.
"""
import argparse
import math
import os
import re
import statistics
import struct
import sys
import time

SURFACE = 0x8000
PLANES = 512
DUMP_WORDS = 4096
OUT_BUF = 4

CALL_RE = re.compile(r"ane_h18 prog: call (\d+)/(\d+) (?:finished in (\d+) ns|(gave no finish event)|finished, the finish event was not seen)")
FLOOR = 2.0 ** -15
OUT_RE = re.compile(r"ane_h18 prog: out buf (\d+) \+(0x[0-9a-f]+|\d+): ((?:[0-9a-f]{8} ?)+)")


def make_input():
    """Two 512-lane vectors: a[j] = ((37j + 11) mod 257 - 128) / 128 and b[j] = a[(j + 1) % 512], the
    residue 0 replaced by +1/128. Same construction as the matvec input: exact in fp16, spread over
    -1..1, never 0, |v| >= 2^-7. a and b differ at every lane (b is shifted one lane), which is what
    makes a device that reads buffer 5 twice (a*a, the srcB-tagged-like-srcA failure seen in the
    ane_h18_prog-mul.bin image that shipped before the srcB tag fix) detectable in all 512 lanes."""
    def v(j):
        r = (37 * j + 11) % 257 - 128
        return (1 if r == 0 else r) / 128
    return [v(j) for j in range(PLANES)], [v((j + 1) % PLANES) for j in range(PLANES)]


def input_file(x):
    """0x8000 bytes: the 512 values at the plane starts, padding zero."""
    halves = [0.0] * (32 * PLANES)
    for j, v in enumerate(x):
        halves[32 * j] = v
    return struct.pack("<%de" % (32 * PLANES), *halves)


def half_away(s):
    """Round a double to fp16, ties away from zero: ane_f16_round_half_away (tools/ane_f16_add.h),
    the rounding rule proven on the M2 for the add-class ops."""
    if not math.isfinite(s):
        return s
    sign = -1.0 if math.copysign(1.0, s) < 0 else 1.0
    a = abs(s)
    if a == 0.0:
        return s
    m, e = math.frexp(a)
    ulp = 2.0 ** -24 if e <= -14 else 2.0 ** (e - 11)
    n = math.floor(a / ulp)
    if a / ulp - n >= 0.5:
        n += 1.0
    return sign * n * ulp


def ulp(v):
    _, e = math.frexp(abs(v))
    return 2.0 ** -24 if v == 0 or abs(v) < 2.0 ** -14 else 2.0 ** (e - 11)


def reference(a, b):
    """Exact (double) products; the fp16 inputs make each product exact."""
    return [x * y for x, y in zip(a, b)]


def lane_ok(got, exact):
    """Within 2 ulp of the product rounded half-away to fp16, plus FLOOR; returns (ok, abs error, error in ulp of the rounded value)."""
    if not math.isfinite(got):
        return False, math.inf, math.inf
    want = half_away(exact)
    d = abs(got - want)
    return d <= 2.0 * ulp(want) + FLOOR, abs(got - exact), d / ulp(want)


def parse_dmesg(text, buf):
    """-> (calls, outs, declared): calls[i] = (ns or None, finished); outs[i] = {byte offset: [u32 words]} of buffer buf for call i.
    Copied from ane/h18/matvec-check.py."""
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
    """The u32 words of a buffer dump from offset 0, or None if the dump has a gap.
    Copied from ane/h18/matvec-check.py."""
    words = []
    for off in sorted(chunks):
        if off != 4 * len(words):
            return None
        words += chunks[off]
    return words


def check_call(words, ref):
    """-> (problem or None, max abs error, max error in ulp over the planes)."""
    checked = DUMP_WORDS * 2 // 32
    if words is None or len(words) < DUMP_WORDS:
        return "dump has %d words, need %d (prog_out_words=%d): the padding would go unchecked" % (len(words or []), DUMP_WORDS, DUMP_WORDS), math.nan, math.nan
    halves = struct.unpack("<%de" % (DUMP_WORDS * 2), struct.pack("<%dI" % len(words), *words))
    bad, worst, worst_ulp = [], 0.0, 0.0
    for h, v in enumerate(halves):
        if h % 32 and v != 0.0:
            return "padding half %d (plane %d lane %d) is %g, not zero" % (h, h // 32, h % 32, v), worst, worst_ulp
    for j, exact in enumerate(ref[:checked]):
        ok, err, u = lane_ok(halves[32 * j], exact)
        worst, worst_ulp = max(worst, err), max(worst_ulp, u)
        if not ok:
            bad.append(j)
    if bad:
        return "%d planes out of band, first plane %d" % (len(bad), bad[0]), worst, worst_ulp
    return None, worst, worst_ulp


def cpu_time(a, b):
    """ns per 512-multiply op on this CPU, stdlib only. -> (backend, [ns per op])"""
    reps, backend = 200, "python float"
    out = []
    for _ in range(reps):
        t = time.perf_counter_ns()
        run = [x * y for x, y in zip(a, b)]
        out.append(time.perf_counter_ns() - t)
    return backend, out


def cmd_check(a):
    if a.input:
        af, bf = a.input + "-a.f16", a.input + "-b.f16"
        av = struct.unpack("<%de" % (32 * PLANES), open(af, "rb").read())[::32]
        bv = struct.unpack("<%de" % (32 * PLANES), open(bf, "rb").read())[::32]
        src = "%s-a.f16, %s-b.f16" % (a.input, a.input)
    else:
        av, bv = make_input()
        src = "the gen-input vectors"
    ref = reference(av, bv)
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
    print("mul %d planes fp16, %d planes checked per call of %d (prog_out_words=%d), inputs %s:" % (PLANES, DUMP_WORDS * 2 // 32, PLANES, DUMP_WORDS, src))
    print("%d calls, max abs error %.6g (%.3g ulp)" % (len(calls), worst, worst_ulp))
    if ns:
        print("device ns per call (poll loop, not hardware time): median %d min %d max %d" % (statistics.median(ns), min(ns), max(ns)))
    backend, cpu = cpu_time(av, bv)
    print("cpu ns per call (%s, %d runs): median %d min %d max %d" % (backend, len(cpu), statistics.median(cpu), min(cpu), max(cpu)))
    for p in problems:
        print("  " + p)
    print("FAIL" if problems else "PASS")
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-input")
    g.add_argument("-o", dest="out", default="mul-in")
    c = sub.add_parser("check")
    c.add_argument("dmesg")
    c.add_argument("--input", help="the input prefix given to gen-input (default: the gen-input vectors)")
    c.add_argument("--buf", type=int, default=OUT_BUF, help="output buffer id (default 4)")
    a = ap.parse_args()
    if a.cmd == "gen-input":
        a2, b2 = make_input()
        open(a.out + "-a.f16", "wb").write(input_file(a2))
        open(a.out + "-b.f16", "wb").write(input_file(b2))
        print("%s-a.f16, %s-b.f16: %d bytes each, a[0..3] = %s, b[0..3] = %s" % (
            a.out, a.out, SURFACE, a2[:4], b2[:4]))
        return 0
    return cmd_check(a)


if __name__ == "__main__":
    sys.exit(main())
