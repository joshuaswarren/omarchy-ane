#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Check the ANE clip-high program run on a T8152 against a CPU reference.

  clip-check.py gen-input [-o FILE]        write the deterministic input buffer (feed it to ane-prog-pack --in 5=FILE)
  clip-check.py check DMESG [--input FILE] compare the output buffer dump in DMESG with the CPU result

The program is fixtures/h14-anec/clip-high: out = min(x, 0.5) elementwise, fp16. The bound pair comes
from the fixture's kernel section (128 bytes of constants: fp16 -inf and 0.5), so this is the smallest
constants-bearing program in the fixture set -- 2,432 bytes packed, one input, one output, one task-
descriptor block -- while add, the only program proven on the M6, is 18,800 bytes with two inputs and
no constants. A pass widens the proven envelope to one-input programs and to programs whose kernel
section carries constants; a CAneProgramManager-class failure here says the assert is not about the
size of a weight section.

The surface layout is the add/mul one, read off the ANEC header: channels 4 and 5 both hold
n=1 c=512 h=1 w=1 with planeStride 0x40 -- 512 planes of 32 fp16 halves (0x8000 bytes per buffer).
Element j is half 32*j (byte 64*j) of each buffer; the other 31 halves of every plane are padding and
must be 0 in the input and the output. Channel plan: input 5, output 4 (inputCount=1 in the ANEC
header, and the buffers ane-prog-pack lists: 5 in, 4 out).

min(x, 0.5) is exact in fp16: the output is bit-for-bit the input value or the constant 0.5, so the
check wants the exact halfword, not a band. The input sequence spans -1..1 with |x| >= 2^-7, so every
plane exercises one of the two clamp branches and roughly half the lanes take the min.

Load the module with hs_prog=1 prog_calls=M prog_out_words=4096: the dump covers 4096 words = 8192
halves = planes 0..255 of 512. Every dumped half outside the 256 plane elements must be 0, and every
element must equal min(x[j], 0.5) bit-for-bit. Planes 256..511 are not dumped and not checked (8192
words would cover them). It logs, per call, "ane_h18 prog: call I/M finished in X ns" and
"ane_h18 prog: out buf 4 +0xOFF: <words>". X is the time the module's poll loop saw the finish event
after it rang the doorbell: poll granularity and interrupt latency are in it. It is not a hardware time.
parse_dmesg and lanes are copied from ane/h18/matvec-check.py; the module logs the same lines.

check prints PASS or FAIL, the count of mismatched planes, the module's per-call ns (median, min, max)
and the CPU time of the same 512-clamp op. Exit status 0 for PASS, 1 for FAIL, 2 for bad arguments.
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
BOUND = 0.5

CALL_RE = re.compile(r"ane_h18 prog: call (\d+)/(\d+) (?:finished in (\d+) ns|(gave no finish event)|finished, the finish event was not seen)")
OUT_RE = re.compile(r"ane_h18 prog: out buf (\d+) \+(0x[0-9a-f]+|\d+): ((?:[0-9a-f]{8} ?)+)")


def make_input():
    """x[j] = ((37j + 11) mod 257 - 128) / 128, the residue 0 replaced by +1/128: exact in fp16, spread
    over -1..1, and never 0 -- the same construction as the mul and matvec inputs, so a lane can diff
    the staged buffers across programs. |x| >= 2^-7 keeps every value normal, and the spread puts
    roughly half the planes above the 0.5 bound, so both clamp branches are exercised."""
    out = []
    for j in range(PLANES):
        r = (37 * j + 11) % 257 - 128
        out.append((1 if r == 0 else r) / 128)
    return out


def input_file(x):
    """The input buffer: element j at half 32*j of its plane, everything else zero."""
    halves = [0.0] * (SURFACE // 2)
    for j, v in enumerate(x):
        halves[32 * j] = v
    return struct.pack("<%de" % len(halves), *halves)


def reference(x):
    """Exact clamp: min(x[j], BOUND) is one of the two fp16 operands, no rounding."""
    return [min(v, BOUND) for v in x]


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
    """-> (problem or None, mismatch count)."""
    checked = DUMP_WORDS * 2 // 32
    if words is None or len(words) < DUMP_WORDS:
        return "dump has %d words, need %d (prog_out_words=%d): the padding would go unchecked" % (len(words or []), DUMP_WORDS, DUMP_WORDS), 0
    halves = struct.unpack("<%de" % (DUMP_WORDS * 2), struct.pack("<%dI" % len(words), *words))
    for h, v in enumerate(halves):
        if h % 32 and v != 0.0:
            return "padding half %d (plane %d lane %d) is %g, not zero" % (h, h // 32, h % 32, v), 0
    bad = [j for j, want in enumerate(ref[:checked]) if not (halves[32 * j] == want)]  # not (...): NaN never equal
    if bad:
        return "%d planes wrong, first plane %d (got %g want %g)" % (len(bad), bad[0], halves[32 * bad[0]], ref[bad[0]]), len(bad)
    return None, 0


def cpu_time(x):
    """ns per 512-clamp op on this CPU, stdlib only. -> (backend, [ns per op])"""
    reps, backend = 200, "python float"
    out = []
    for _ in range(reps):
        t = time.perf_counter_ns()
        run = [min(v, BOUND) for v in x]
        out.append(time.perf_counter_ns() - t)
    return backend, out


def cmd_check(a):
    if a.input:
        xv = struct.unpack("<%de" % (32 * PLANES), open(a.input, "rb").read())[::32]
        src = a.input
    else:
        xv = make_input()
        src = "the gen-input vector"
    ref = reference(xv)
    calls, outs, declared = parse_dmesg(open(a.dmesg, errors="replace").read(), a.buf)
    problems, worst = [], 0
    if not calls:
        problems.append("no 'ane_h18 prog: call I/M' line in the log")
    if len(calls) != declared:
        problems.append("log has %d of %d calls" % (len(calls), declared))
    for i, chunks in enumerate(outs):
        p, n = check_call(lanes(chunks), ref)
        worst = max(worst, n)
        if p:
            problems.append("call %d: %s" % (i + 1, p))
        if not calls[i][1]:
            problems.append("call %d: no finish event" % (i + 1))
    ns = [c for c, _ in calls if c is not None]
    print("clip-high %d planes fp16 min(x, %g), %d planes checked per call of %d (prog_out_words=%d), input %s:" % (
        PLANES, BOUND, DUMP_WORDS * 2 // 32, PLANES, DUMP_WORDS, src))
    print("%d calls, %d plane mismatches across dumped planes (exact match wanted)" % (len(calls), worst))
    if ns:
        print("device ns per call (poll loop, not hardware time): median %d min %d max %d" % (statistics.median(ns), min(ns), max(ns)))
    backend, cpu = cpu_time(xv)
    print("cpu ns per call (%s, %d runs): median %d min %d max %d" % (backend, len(cpu), statistics.median(cpu), min(cpu), max(cpu)))
    for p in problems:
        print("  " + p)
    print("FAIL" if problems else "PASS")
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-input")
    g.add_argument("-o", dest="out", default="clip-in.f16")
    c = sub.add_parser("check")
    c.add_argument("dmesg")
    c.add_argument("--input", help="the input file given to gen-input (default: the gen-input vector)")
    c.add_argument("--buf", type=int, default=OUT_BUF, help="output buffer id (default 4)")
    a = ap.parse_args()
    if a.cmd == "gen-input":
        x = make_input()
        open(a.out, "wb").write(input_file(x))
        print("%s: %d bytes, x[0..3] = %s" % (a.out, SURFACE, x[:4]))
        return 0
    return cmd_check(a)


if __name__ == "__main__":
    sys.exit(main())
