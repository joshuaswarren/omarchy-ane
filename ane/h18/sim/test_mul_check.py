#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Positive and negative controls for ane/h18/mul-check.py on canned ane_h18 dmesg dumps.

The canned dump is what the module logs with hs_prog=1 prog_calls=3 prog_out_words=4096: for each call a
"call I/M finished in X ns" line, then 16 words per "out buf 4 +0xOFF:" line (256 lines: one 64-byte
plane per line, the plane element in word 0, padding zero). Every mutated dump must FAIL; the clean one,
a 1 ulp lane, and the input files swapped must PASS. The swapped-inputs control PASSES because elementwise
multiply is commutative; the failure mode it is standing in for -- a device that read buffer 5 twice --
is covered by the a*a control, the defect of the srcB-tagged-like-srcA image that shipped as
ane_h18_prog-mul.bin before the srcB tag fix.
"""
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "mul-check.py")
spec = importlib.util.spec_from_file_location("mc", SCRIPT)
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

failed = 0


def report(name, ok):
    global failed
    failed += not ok
    print("  %-72s %s" % (name, "PASS" if ok else "FAIL"))


def dump(lanes_f16, pad=0, ns=(41000, 39000, 52000), nwords=4096, lines=None):
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


a, b = mc.make_input()
ref = mc.reference(a, b)
good = [mc.half_away(e) for e in ref]

print("mul-check inputs and reference")
report("a and b are nonzero with |v| >= 2^-7, and differ at every lane", (
    all(v != 0 and abs(v) >= 2 ** -7 for v in a + b) and all(a[j] != b[j] for j in range(mc.PLANES))))
report("gen-input files are 0x8000 bytes, deterministic, planes carry the values", (
    len(mc.input_file(a)) == mc.SURFACE and mc.input_file(a) == mc.input_file(mc.make_input()[0])
    and struct.unpack("<16384e", mc.input_file(a))[::32] == tuple(a)
    and max(abs(v) for h, v in enumerate(struct.unpack("<16384e", mc.input_file(a))) if h % 32) == 0))
report("half_away matches IEEE big-value ties: 0.5 ulp at 1 + 2^-11 rounds to 1 + 2^-10", (
    mc.half_away(1.0 + 2.0 ** -11) == 1.0 + 2.0 ** -10 and mc.half_away(-(1.0 + 2.0 ** -11)) == -(1.0 + 2.0 ** -10)))
report("products a*b need no rounding before the fp16 step (all exact in double)", (
    all(struct.unpack("<e", struct.pack("<e", e))[0] == good[j] or abs(struct.unpack("<e", struct.pack("<e", e))[0] - e) <= mc.ulp(good[j]) for j, e in enumerate(ref[:8]))))

print("mul-check on canned dmesg")
rc, out = run(dump(good))
report("clean dump of 3 calls: PASS, exit 0", rc == 0 and out.rstrip().endswith("PASS"))
report("device ns median 41000 min 39000 max 52000", "median 41000 min 39000 max 52000" in out)
report("cpu ns per call is printed", "cpu ns per call (" in out)
report("256 planes checked of 512 is stated", "256 planes checked per call of 512" in out)
report("max abs error is the fp16 rounding of the product (under 0.01)", float(out.split("max abs error ")[1].split()[0]) < 0.01)


def ulps_off(v, k):
    """v moved k fp16 steps away from zero."""
    return struct.unpack("<e", struct.pack("<H", struct.unpack("<H", struct.pack("<e", v))[0] + k))[0]


for k, verdict in ((1, "PASS"), (2, "PASS"), (3, "FAIL")):
    off = list(good)
    off[7] = ulps_off(good[7], k)
    rc, out = run(dump(off))
    report("one plane %d ulp off: %s (band is 2 ulp + 2^-15)" % (k, verdict), out.rstrip().endswith(verdict))

print("mul-check: the device read buffer 5 twice (a*a), the old srcB-tag defect")
a2 = [mc.half_away(x * x) for x in a]
rc, out = run(dump(a2))
report("a*a output instead of a*b: FAIL", rc == 1 and out.rstrip().endswith("FAIL"))

print("mul-check: one plane wrong, order and padding")
wrong = list(good)
wrong[100] = mc.half_away(a[101] * b[101])
rc, out = run(dump(wrong))
report("plane 100 holds a[101]*b[101] (one lane shifted): FAIL, first plane 100", (
    rc == 1 and out.rstrip().endswith("FAIL") and "first plane 100" in out))

rc, out = run(dump(list(reversed(good))))
report("planes in reverse order: FAIL", rc == 1)

rc, out = run(dump([0.0] * mc.PLANES))
report("all-zero output: FAIL", rc == 1)

rc, out = run(dump([float("nan")] * mc.PLANES))
report("NaN output: FAIL", rc == 1)

rc, out = run(dump(good, pad=0x3c00))
report("nonzero padding half in call 2: FAIL", rc == 1 and "call 2: padding half 1 (plane 0 lane 1)" in out)

for nw in (64, 2048, 4080):
    rc, out = run(dump(good, nwords=nw))
    report("dump of %d words, planes right, padding unchecked: FAIL" % nw, rc == 1 and "dump has %d words" % nw in out)

with tempfile.TemporaryDirectory() as d:
    fa, fb = os.path.join(d, "s-a.f16"), os.path.join(d, "s-b.f16")
    open(fa, "wb").write(mc.input_file(a))
    open(fb, "wb").write(mc.input_file(b))
    rc, out = run(dump(good), "--input", os.path.join(d, "s"))
    report("check with the staged input files: PASS", rc == 0 and out.rstrip().endswith("PASS"))
    os.rename(fa, fa + ".x")
    os.rename(fb, fa)
    os.rename(fa + ".x", fb)
    rc, out = run(dump(good), "--input", os.path.join(d, "s"))
    report("input files swapped: PASS (multiply is commutative; a*b == b*a)", rc == 0 and out.rstrip().endswith("PASS"))

rc, out = run(dump(good).replace("call 3/3 finished in 52000 ns", "call 3/3 gave no finish event"))
report("call without a finish event: FAIL", rc == 1 and "call 3: no finish event" in out)

rc, out = run(dump(good).replace("call 3/3 finished in 52000 ns", "call 3/3 finished, the finish event was not seen before the events were read: no latency"))
report("call finished without a latency: PASS, its dump stays with call 3, ns stats from 2 calls", rc == 0 and "median 40000 min 39000 max 41000" in out)

rc, out = run("\n".join(dump(good).splitlines()[:-256]) + "\n")
report("log cut short (3 calls announced, 2 and a part logged): FAIL", rc == 1)

rc, out = run("[ 1.0] ane_h18: nothing\n")
report("no call lines at all: FAIL", rc == 1 and "no 'ane_h18 prog: call" in out)

rc, out = run(dump(good).replace("out buf 4", "out buf 5"))
report("dump of another buffer id: FAIL", rc == 1)

print("MUL-CHECK: %s" % ("all controls PASS" if not failed else "%d FAILED" % failed))
sys.exit(1 if failed else 0)
