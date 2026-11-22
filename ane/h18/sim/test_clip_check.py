#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Positive and negative controls for ane/h18/clip-check.py on canned ane_h18 dmesg dumps.

The canned dump is what the module logs with hs_prog=1 prog_calls=3 prog_out_words=4096: for each call a
"call I/M finished in X ns" line, then 16 words per "out buf 4 +0xOFF:" line (256 lines: one 64-byte
plane per line, the plane element in word 0, padding zero). The op is exact in fp16 (min(x, 0.5) returns
one of its two operands), so every mutated dump must FAIL, including a one-ulp lane; the clean one must
PASS. The max(x, 0.5) dump -- the clip-low fixture's answer, the other member of the bound pair -- must
FAIL: it is the stand-in for the wrong program image being staged.
"""
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "clip-check.py")
spec = importlib.util.spec_from_file_location("mc", SCRIPT)
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

failed = 0


def report(name, ok):
    global failed
    failed += not ok
    print("  %-72s %s" % (name, "PASS" if ok else "FAIL"))


def dump(lanes_f16, pad=0, ns=(41000, 39000, 52000), nwords=4096):
    """A dmesg text with one call per entry of ns; lanes_f16: the 512 device plane elements (floats) per call."""
    out = ["[   12.000000] ane_h18 0.ane: ane_h18 prog: PROCEDURE_CALL queued"]
    for i, x in enumerate(ns):
        out.append("[   12.%06d] ane_h18 0.ane: ane_h18 prog: call %d/%d finished in %d ns" % (i, i + 1, len(ns), x))
        halves = [0.0] * (mc.DUMP_WORDS * 2)
        for j, v in enumerate(lanes_f16[:mc.DUMP_WORDS * 2 // 32]):
            halves[32 * j] = v
        raw = struct.pack("<%de" % (mc.DUMP_WORDS * 2), *halves)
        if pad and i == 1:
            raw = raw[:2] + struct.pack("<H", pad) + raw[4:]
        words = struct.unpack("<%dI" % nwords, raw[:4 * nwords])
        for off in range(0, nwords, 16):
            out.append("[   12.%06d] ane_h18 0.ane: ane_h18 prog: out buf 4 +0x%x: %s" % (
                i, off * 4, " ".join("%08x" % w for w in words[off:off + 16])))
    return "\n".join(out) + "\n"


def run(text, *args):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "dmesg.txt")
        open(p, "w").write(text)
        r = subprocess.run([sys.executable, SCRIPT, "check", p] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout


x = mc.make_input()
ref = mc.reference(x)

print("clip-check inputs and reference")
report("x is nonzero with |v| >= 2^-7", all(v != 0 and abs(v) >= 2 ** -7 for v in x))
report("both clamp branches are exercised (planes above and below 0.5)", (
    any(v > 0.5 for v in x) and any(v < 0.5 for v in x)))
report("gen-input file is 0x8000 bytes, deterministic, planes carry the values", (
    len(mc.input_file(x)) == mc.SURFACE and mc.input_file(x) == mc.input_file(mc.make_input())
    and struct.unpack("<16384e", mc.input_file(x))[::32] == tuple(x)
    and max(abs(v) for h, v in enumerate(struct.unpack("<16384e", mc.input_file(x))) if h % 32) == 0))
report("the fixture bound is fp16 0.5 and the reference is exact (min of two fp16)", (
    mc.BOUND == 0.5 and all(ref[j] == x[j] or ref[j] == 0.5 for j in range(mc.PLANES))))

print("clip-check on canned dmesg")
rc, out = run(dump(ref))
report("clean dump of 3 calls: PASS, exit 0", rc == 0 and out.rstrip().endswith("PASS"))
report("device ns median 41000 min 39000 max 52000", "median 41000 min 39000 max 52000" in out)
report("cpu ns per call is printed", "cpu ns per call (" in out)
report("256 planes checked of 512 is stated", "256 planes checked per call of 512" in out)
report("0 plane mismatches reported", "0 plane mismatches" in out)


def ulps_off(v, k):
    """v moved k fp16 steps away from zero."""
    return struct.unpack("<e", struct.pack("<H", struct.unpack("<H", struct.pack("<e", v))[0] + k))[0]


off = list(ref)
off[7] = ulps_off(ref[7], 1)
rc, out = run(dump(off))
report("one plane 1 ulp off: FAIL (the op is exact, no band)", rc == 1 and "first plane 7" in out)

print("clip-check: the clip-low answer max(x, 0.5) -- the wrong program staged")
low = [max(v, 0.5) for v in x]
rc, out = run(dump(low))
report("max(x, 0.5) instead of min(x, 0.5): FAIL", rc == 1 and out.rstrip().endswith("FAIL"))

print("clip-check: identity output -- the device copied the input instead of clamping")
rc, out = run(dump(x))
report("y = x (no clamp applied): FAIL, first mismatch is the first plane above 0.5", rc == 1)

print("clip-check: one plane wrong, order and padding")
wrong = list(ref)
wrong[100] = ref[101]
rc, out = run(dump(wrong))
report("plane 100 holds plane 101's value (one lane shifted): FAIL, first plane 100", (
    rc == 1 and "first plane 100" in out))

rc, out = run(dump(list(reversed(ref))))
report("planes in reverse order: FAIL", rc == 1)

rc, out = run(dump([0.0] * mc.PLANES))
report("all-zero output: FAIL", rc == 1)

rc, out = run(dump([float("nan")] * mc.PLANES))
report("NaN output: FAIL (NaN never equals)", rc == 1)

rc, out = run(dump(ref, pad=0x3c00))
report("nonzero padding half in call 2: FAIL", rc == 1 and "call 2: padding half 1 (plane 0 lane 1)" in out)

for nw in (64, 2048, 4080):
    rc, out = run(dump(ref, nwords=nw))
    report("dump of %d words, planes right, padding unchecked: FAIL" % nw, rc == 1 and "dump has %d words" % nw in out)

with tempfile.TemporaryDirectory() as d:
    f = os.path.join(d, "in.f16")
    open(f, "wb").write(mc.input_file(x))
    rc, out = run(dump(ref), "--input", f)
    report("check with the staged input file: PASS", rc == 0 and out.rstrip().endswith("PASS"))

rc, out = run(dump(ref).replace("call 3/3 finished in 52000 ns", "call 3/3 gave no finish event"))
report("call without a finish event: FAIL", rc == 1 and "call 3: no finish event" in out)

rc, out = run("\n".join(dump(ref).splitlines()[:-256]) + "\n")
report("log cut short (3 calls announced, 2 and a part logged): FAIL", rc == 1)

rc, out = run("[ 1.0] ane_h18: nothing\n")
report("no call lines at all: FAIL", rc == 1 and "no 'ane_h18 prog: call" in out)

rc, out = run(dump(ref).replace("out buf 4", "out buf 5"))
report("dump of another buffer id: FAIL", rc == 1)

print("CLIP-CHECK: %s" % ("all controls PASS" if not failed else "%d FAILED" % failed))
sys.exit(1 if failed else 0)
