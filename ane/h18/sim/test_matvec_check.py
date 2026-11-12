#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Positive and negative controls for ane/h18/matvec-check.py on canned ane_h18 dmesg dumps.

The canned dump is what the module logs with hs_prog=1 prog_calls=3 prog_out_words=4096: for each call a "call I/M finished in X ns"
line, then 16 words per "out buf 4 +0xOFF:" line. Every mutated dump must FAIL; the clean one and a 1 ulp lane must PASS.
"""
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "matvec-check.py")
spec = importlib.util.spec_from_file_location("mc", SCRIPT)
mc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mc)

failed = 0


def report(name, ok):
    global failed
    failed += not ok
    print("  %-72s %s" % (name, "PASS" if ok else "FAIL"))


def dump(lanes_f16, pad=0, ns=(41000, 39000, 52000), nwords=4096, lines=None):
    """A dmesg text with one call per entry of ns; lanes_f16: the 256 device lane values (floats) per call."""
    out = ["[   12.000000] ane_h18 0.ane: ane_h18 prog: PROCEDURE_CALL queued"]
    for i, x in enumerate(ns):
        out.append("[   12.%06d] ane_h18 0.ane: ane_h18 prog: call %d/%d finished in %d ns" % (i, i + 1, len(ns), x))
        raw = (struct.pack("<%de" % mc.N, *lanes_f16) + bytes(4 * 4096 - 2 * mc.N))[:4 * nwords]
        if pad and i == 1:
            raw = raw[:2 * mc.N] + struct.pack("<I", pad) + raw[2 * mc.N + 4:]
        words = struct.unpack("<%dI" % nwords, raw)
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
w = mc.unpermute(open(mc.FIXTURE, "rb").read())
ref = mc.reference(x, w)
good = [struct.unpack("<e", struct.pack("<e", e))[0] for e, _ in ref]

print("matvec-check weights and input")
closed = all(w[n][k] == ((n % 8) * 8 + (k % 4) * 2 - 31) / 32 for n in range(mc.N) for k in range(mc.K))
report("un-permuted fixture weights are W[n][k] = ((n%8)*8 + (k%4)*2 - 31)/32", closed)
report("gen-input is 0x4000 bytes, deterministic, lanes are exact fp16", (
    len(mc.input_file(x)) == 0x4000 and mc.input_file(x) == mc.input_file(mc.make_input())
    and struct.unpack("<256e", mc.input_file(x)[:512]) == tuple(x)))

print("matvec-check on canned dmesg")
rc, out = run(dump(good))
report("clean dump of 3 calls: PASS, exit 0", rc == 0 and out.rstrip().endswith("PASS"))
report("device ns median 41000 min 39000 max 52000", "median 41000 min 39000 max 52000" in out)
report("cpu ns per call is printed", "cpu ns per call (" in out)
report("max abs error is the fp16 rounding of the result (under 0.01)", float(out.split("max abs error ")[1].split()[0]) < 0.01)

one_ulp = list(good)
one_ulp[7] = struct.unpack("<e", struct.pack("<H", struct.unpack("<H", struct.pack("<e", good[7]))[0] + 1))[0]
rc, out = run(dump(one_ulp))
report("one lane 1 ulp off: still PASS (band is 2 ulp)", rc == 0 and out.rstrip().endswith("PASS"))

bad = list(good)
bad[100] = good[100] + 0.25
rc, out = run(dump(bad))
report("one lane off by 0.25: FAIL, exit 1", rc == 1 and out.rstrip().endswith("FAIL") and "first lane 100" in out)

rc, out = run(dump(list(reversed(good))))
report("lanes in reverse order: FAIL", rc == 1)

rc, out = run(dump([0.0] * mc.N))
report("all-zero output: FAIL", rc == 1)

rc, out = run(dump([float("nan")] * mc.N))
report("NaN output: FAIL", rc == 1)

rc, out = run(dump(good, pad=0x3c003c00))
report("nonzero padding word in call 2: FAIL", rc == 1 and "call 2: padding" in out)

rc, out = run(dump(good, nwords=64))
report("dump shorter than 128 words: FAIL", rc == 1 and "dump has 64 words" in out)

rc, out = run(dump(good).replace("call 3/3 finished in 52000 ns", "call 3/3 gave no finish event"))
report("call without a finish event: FAIL", rc == 1 and "call 3: no finish event" in out)

rc, out = run("\n".join(dump(good).splitlines()[:-512]) + "\n")
report("log cut short (3 calls announced, 2 and a part logged): FAIL", rc == 1)

rc, out = run("[ 1.0] ane_h18: nothing\n")
report("no call lines at all: FAIL", rc == 1 and "no 'ane_h18 prog: call" in out)

rc, out = run(dump(good).replace("out buf 4", "out buf 5"))
report("dump of another buffer id: FAIL", rc == 1)

print("MATVEC-CHECK: %s" % ("all controls PASS" if not failed else "%d FAILED" % failed))
sys.exit(1 if failed else 0)
