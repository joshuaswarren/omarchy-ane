#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Golden CPU fp16 digests for the five M6 ANE ladder rungs, on any x86 host.

  ladder-digests.py gen-inputs [-o DIR]      write the six deterministic input buffers
  ladder-digests.py gen [-o FILE]            write the digest file (inputs + golden fp16 output shas)
  ladder-digests.py check FILE               recompute every digest and compare; exit 1 on any mismatch

The ladder (CARD-QUEUE M6c, receipts/2026-11-26-sup38-m6-ladder-latency-trend.md) is five rungs of
fixtures/h14-anec programs: mul control, clip (high and low), relu, rms-c2048-gamma, matvec+iova_kern.
SUP-15 runs them on the M6 and its receipts self-check each rung's output against the digests here.

Every value is CPU fp16 and reproducible on any x86 host: the inputs are the deterministic vectors the
check scripts document (x[j] = ((37j + 11) mod 257 - 128) / 128, residue 0 -> +1/128), the goldens are
the check scripts' own reference chains. Nothing here touches a device.

Output surfaces, per the ANEC headers and the check scripts:

  elementwise (mul, clip-high, clip-low, relu): n=1 c=512 h=1 w=1, planeStride 0x40 -- lane j at half
  32j of a 0x8000 buffer; the other 31 halves of every plane are padding and stay zero. mul reads
  inputs on channels 5 and 6, clip and relu read channel 5; all write channel 4.

  rms-c2048-gamma: C=2048 rows, row r at half 32r of a 0x20000 buffer in and out. The device-proven
  formula (receipts/2026-09-30-t6021-island-select-rms): rs = sqrt(E[x^2] + 2^-17 * max|x|^2),
  scale = fp16(0.5/rs) once, y[r] = fp16(x[r]*scale) for r in 64..2047; rows 0..63 are not written by
  the device, so the golden keeps them zero.

  matvec: y[1,256] = x[1,256] . W[256,256]; x is the first 256 halves of a 0x4000 buffer on channel 5,
  y the first 256 halves of a 0x4000 buffer on channel 4. W comes from the fixture's kernel.bin,
  un-permuted by Apple's packing rule (matvec-check.py).

Rounding: mul and rms use fp16 round-half-away (the M2-proven device rule, tools/ane_f16_add.h);
matvec uses IEEE round-half-even like matvec-check.py's want; clip and relu are exact (every output
is an input value or the fp16 bound/0). Lanes where the two tie rules differ do not occur in mul/rms
(half-away is the proven rule), and the matvec golden states its rule beside the digest.
"""
import argparse
import hashlib
import json
import math
import os
import struct
import sys

FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "fixtures", "h14-anec")
PLANES = 512
EW_SURFACE = 0x8000
RMS_C = 2048
RMS_SURFACE = 0x20000
RMS_FIRST_ROW = 64
RMS_EPS = 2.0 ** -17
MATVEC_K = MATVEC_N = 256
MATVEC_SURFACE = 0x4000


def lanes_x(n):
    """x[j] = ((37j + 11) mod 257 - 128) / 128, residue 0 -> +1/128: the shared deterministic vector
    the check scripts document; exact in fp16, spread over -1..1, never 0, |x| >= 2^-7."""
    out = []
    for j in range(n):
        r = (37 * j + 11) % 257 - 128
        out.append((1 if r == 0 else r) / 128)
    return out


def surface(vals, stride, size):
    """Lane j at half stride*j of a zeroed size-byte fp16 buffer."""
    halves = [0.0] * (size // 2)
    for j, v in enumerate(vals):
        halves[stride * j] = v
    return struct.pack("<%de" % len(halves), *halves)


def half_away(s):
    """Round a double to fp16, ties away from zero: ane_f16_round_half_away, the M2-proven rule
    (copied from mul-check.py)."""
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


def half_even(s):
    """IEEE round-half-even to fp16, like matvec-check.py's want."""
    return struct.unpack("<e", struct.pack("<e", s))[0]


def f16(buf):
    return struct.unpack("<%de" % (len(buf) // 2), buf)


def sha(buf):
    return hashlib.sha256(buf).hexdigest()


def input_buffers():
    """name -> (surface bytes, channel, note): the six canonical input buffers."""
    x512 = lanes_x(PLANES)
    a = x512
    b = [x512[(j + 1) % PLANES] for j in range(PLANES)]
    return {
        "mul-a.f16": (surface(a, 32, EW_SURFACE), 5, "mul input a = x"),
        "mul-b.f16": (surface(b, 32, EW_SURFACE), 6, "mul input b = x shifted one lane"),
        "clip-in.f16": (surface(x512, 32, EW_SURFACE), 5, "shared clip-high/clip-low input x"),
        "relu-in.f16": (surface(x512, 32, EW_SURFACE), 5, "relu input x"),
        "rms-in.f16": (surface(lanes_x(RMS_C), 32, RMS_SURFACE), 5, "rms input x, 2048 rows"),
        "matvec-in.f16": (struct.pack("<%de" % MATVEC_K, *lanes_x(MATVEC_K)) + bytes(MATVEC_SURFACE - 2 * MATVEC_K), 5, "matvec input x, dense"),
    }


def matvec_weights():
    """W[n][k] from the fixture's kernel.bin, Apple's weight packing (matvec-check.py unpermute)."""
    kernel = open(os.path.join(FIXTURES, "matvec", "kernel.bin"), "rb").read()
    g = min(16, MATVEC_N // 16)
    planes = MATVEC_N // g
    half = f16(kernel[:MATVEC_K * MATVEC_N * 2])
    w = []
    for n in range(MATVEC_N):
        p = n // g
        q = (p % 16) * (planes // 16) + p // 16
        base = q * MATVEC_K * g + n % g
        w.append([half[base + k * g] for k in range(MATVEC_K)])
    return w


def goldens():
    """rung -> (program, golden fp16 output surface, lane values, rule note)."""
    x512 = lanes_x(PLANES)
    a = x512
    b = [x512[(j + 1) % PLANES] for j in range(PLANES)]
    out = {}
    out["mul"] = ("mul", surface([half_away(a[j] * b[j]) for j in range(PLANES)], 32, EW_SURFACE),
                  "y[j] = fp16(a[j]*b[j]), round half-away; a=x, b=x shifted one lane")
    out["clip-high"] = ("clip-high", surface([min(v, 0.5) for v in x512], 32, EW_SURFACE),
                        "y[j] = min(x[j], 0.5), exact fp16")
    out["clip-low"] = ("clip-low", surface([max(v, 0.5) for v in x512], 32, EW_SURFACE),
                       "y[j] = max(x[j], 0.5), exact fp16")
    out["relu"] = ("relu", surface([max(v, 0.0) for v in x512], 32, EW_SURFACE),
                   "y[j] = max(x[j], 0), exact fp16")
    x2048 = lanes_x(RMS_C)
    mean_sq = sum(v * v for v in x2048) / RMS_C
    rs = math.sqrt(mean_sq + RMS_EPS * max(abs(v) for v in x2048) ** 2)
    scale = half_away(0.5 / rs)
    yr = [0.0] * RMS_C
    for r in range(RMS_FIRST_ROW, RMS_C):
        yr[r] = half_away(x2048[r] * scale)
    out["rms-c2048-gamma"] = ("rms-c2048-gamma", surface(yr, 32, RMS_SURFACE),
                              "rs = sqrt(E[x^2] + 2^-17*max|x|^2); y[r] = fp16(x[r]*fp16(0.5/rs)), "
                              "half-away, r in 64..2047; rows 0..63 device-untouched, golden zeros")
    w = matvec_weights()
    ym = [half_even(sum(xv * wv for xv, wv in zip(x512[:MATVEC_K], w[n]))) for n in range(MATVEC_N)]
    out["matvec"] = ("matvec", struct.pack("<%de" % MATVEC_N, *ym) + bytes(MATVEC_SURFACE - 2 * MATVEC_N),
                     "y[n] = fp16(sum_k x[k]*W[n][k]), fp64 accumulate, IEEE half-even; "
                     "W un-permuted from kernel.bin")
    return out


def digest_file(inputs, outs, base):
    rungs = {}
    for rung, (program, surface_buf, rule) in outs.items():
        entry = {
            "program_fixture": "fixtures/h14-anec/%s/program-0.anec" % program,
            "program_sha256": sha(open(os.path.join(base, program, "program-0.anec"), "rb").read()),
            "kernel_sha256": sha(open(os.path.join(base, program, "kernel.bin"), "rb").read()),
            "golden_output_sha256": sha(surface_buf),
            "output_surface_bytes": len(surface_buf),
            "cpu_rule": rule,
        }
        if rung == "mul":
            entry["input_fixtures"] = {"channel5": "ladder-inputs/mul-a.f16", "channel6": "ladder-inputs/mul-b.f16"}
            entry["input_sha256"] = {"mul-a.f16": sha(inputs["mul-a.f16"][0]), "mul-b.f16": sha(inputs["mul-b.f16"][0])}
        elif rung in ("clip-high", "clip-low"):
            entry["input_fixtures"] = {"channel5": "ladder-inputs/clip-in.f16"}
            entry["input_sha256"] = {"clip-in.f16": sha(inputs["clip-in.f16"][0])}
        elif rung == "relu":
            entry["input_fixtures"] = {"channel5": "ladder-inputs/relu-in.f16"}
            entry["input_sha256"] = {"relu-in.f16": sha(inputs["relu-in.f16"][0])}
        elif rung == "rms-c2048-gamma":
            entry["input_fixtures"] = {"channel5": "ladder-inputs/rms-in.f16"}
            entry["input_sha256"] = {"rms-in.f16": sha(inputs["rms-in.f16"][0])}
        elif rung == "matvec":
            entry["input_fixtures"] = {"channel5": "ladder-inputs/matvec-in.f16"}
            entry["input_sha256"] = {"matvec-in.f16": sha(inputs["matvec-in.f16"][0])}
        rungs[rung] = entry
    return {
        "purpose": "Golden CPU fp16 output digests for the five M6 ANE ladder rungs "
                   "(mul control, clip, relu, rms, matvec+iova_kern), so SUP-15 receipts "
                   "self-check each rung's output. Computed on x86 CPU; no device data.",
        "generator": "ane/h18/ladder-digests.py",
        "x_vector": "x[j] = ((37j + 11) mod 257 - 128) / 128, residue 0 -> +1/128",
        "rungs": rungs,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen-inputs", help="write the input buffers")
    g.add_argument("-o", dest="dir", default="ladder-inputs")
    d = sub.add_parser("gen", help="write the digest file")
    d.add_argument("-o", dest="out", default="fixtures/h14-anec/ladder-fp16-digests.json")
    c = sub.add_parser("check", help="recompute and compare every digest")
    c.add_argument("file")
    a = ap.parse_args()
    inputs = input_buffers()
    if a.cmd == "gen-inputs":
        os.makedirs(a.dir, exist_ok=True)
        for name, (buf, chan, note) in inputs.items():
            path = os.path.join(a.dir, name)
            open(path, "wb").write(buf)
            print("%s: %d bytes on channel %d (%s) sha256 %s" % (path, len(buf), chan, note, sha(buf)))
        return 0
    outs = goldens()
    doc = digest_file(inputs, outs, FIXTURES)
    if a.cmd == "gen":
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        open(a.out, "w").write(json.dumps(doc, indent=2) + "\n")
        print("%s: %d rungs" % (a.out, len(doc["rungs"])))
        for rung, e in doc["rungs"].items():
            print("  %s -> %s" % (rung, e["golden_output_sha256"]))
        return 0
    want = json.load(open(a.file))
    problems = []
    for rung, entry in want["rungs"].items():
        got = doc["rungs"].get(rung)
        if got is None:
            problems.append("%s: no recomputed entry" % rung)
            continue
        for key in ("program_sha256", "kernel_sha256", "golden_output_sha256"):
            if got[key] != entry[key]:
                problems.append("%s %s: file %s != recomputed %s" % (rung, key, entry[key], got[key]))
        if got["input_sha256"] != entry["input_sha256"]:
            problems.append("%s input_sha256 differs" % rung)
    extra = set(doc["rungs"]) - set(want["rungs"])
    if extra:
        problems.append("recomputed rungs missing from the file: %s" % ", ".join(sorted(extra)))
    for p in problems:
        print("  " + p)
    print("FAIL" if problems else "PASS")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
