#!/usr/bin/env python3
"""Host tests for tools/ane-cycle (H13 ABI-1 lifecycle loop), built against
a fake libane - no device is opened.

Regression: the parser required `--in IDX=FILE` inside one argv element
(strncmp "--in ", 5) while the usage line and the only caller pass --in and
IDX=FILE as two shell words, so every real invocation exited 2 on the usage
line before any device open. These tests pin the two-word contract and the
bit-exact expect path against the fixture's fp16-add surface semantics
(lanes at stride 64, fp16 add with ties away from zero - the same model the
soak harness and the fake device implement).
"""

import random
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANEC = ROOT / "fixtures/h13-anec/add/program-0.anec"
ANEC_SHA = "9a6a6a9a701ce207d4a8ea80ee1ca6d5ade076be2115b73a9aed6f4236aa56ab"
FAKE_LIBANE = ROOT / "tests/fixtures/fake_libane_cycle.c"
CH = 16384
STRIDE = 64


def build_ane_cycle(out_dir):
    exe = out_dir / "ane-cycle"
    subprocess.run(
        ["gcc", "-I", str(ROOT / "libane"), "-I", str(ROOT / "ane/src/uapi/drm"),
         "-Wall", "-Werror", "-Wextra", "-O2", "-std=gnu99", "-o", str(exe),
         str(ROOT / "tools/ane-cycle.c"), str(FAKE_LIBANE)],
        check=True)
    return exe


def fp16_units(bits):
    sign, exp, frac = bits >> 15, (bits >> 10) & 31, bits & 1023
    value = frac if exp == 0 else (1024 + frac) << (exp - 1)
    return -value if sign else value


def round_fp16_units(value):
    sign = 0x8000 if value < 0 else 0
    n = abs(value)
    if n < 1024:
        return sign | n
    shift = max(n.bit_length() - 11, 0)
    quantum = 1 << shift
    n = ((n + quantum // 2) // quantum) * quantum
    exponent = n.bit_length() - 10
    significand = n >> (exponent - 1)
    if significand == 2048:
        exponent += 1
        significand = 1024
    return sign | (exponent << 10) | (significand - 1024)


def seeded_lane(rng):
    """fp16 in [1,16): exponent 11..14, random mantissa, sums stay < 32."""
    return (rng.randrange(11, 15) << 10) | rng.randrange(1024)


def surface(lanes):
    buf = bytearray(CH)
    for j, v in enumerate(lanes):
        buf[j * STRIDE:j * STRIDE + 2] = v.to_bytes(2, "little")
    return bytes(buf)


def add_surface(a, b):
    out = bytearray(CH)
    for off in range(0, CH, STRIDE):
        s = (fp16_units(int.from_bytes(a[off:off + 2], "little")) +
             fp16_units(int.from_bytes(b[off:off + 2], "little")))
        out[off:off + 2] = round_fp16_units(s).to_bytes(2, "little")
    return bytes(out)


def test_two_word_in_flag_runs_cycles_bit_exact():
    """The caller's exact argv shape (--in 0=F --in 1=F --expect F) runs and
    the fake device's add datapath matches the model bit-exact."""
    import hashlib
    assert hashlib.sha256(ANEC.read_bytes()).hexdigest() == ANEC_SHA
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        exe = build_ane_cycle(tmp)
        lanes_a = [seeded_lane(random.Random(1)) for _ in range(256)]
        lanes_b = [seeded_lane(random.Random(2)) for _ in range(256)]
        a, b, e = tmp / "a.f16", tmp / "b.f16", tmp / "expect.f16"
        a.write_bytes(surface(lanes_a))
        b.write_bytes(surface(lanes_b))
        e.write_bytes(add_surface(a.read_bytes(), b.read_bytes()))
        run = subprocess.run(
            [str(exe), "--anec", str(ANEC), "--cycles", "3",
             "--in", f"0={a}", "--in", f"1={b}", "--expect", str(e)],
            capture_output=True, text=True, timeout=60)
        assert run.returncode == 0, run.stdout + run.stderr
        lines = run.stdout.splitlines()
        assert len(lines) == 3, run.stdout
        assert all(line.startswith("CYCLE ") and line.endswith("match 1")
                   for line in lines), run.stdout


def test_bad_in_spec_and_unknown_flag_refuse():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build_ane_cycle(Path(tmp))
        for argv in ([str(exe), "--anec", str(ANEC), "--cycles", "1",
                      "--in", "0novalue"],
                     [str(exe), "--anec", str(ANEC), "--cycles", "1",
                      "--bogus"]):
            run = subprocess.run(argv, capture_output=True, text=True,
                                 timeout=60)
            assert run.returncode == 2, run
            assert "usage: ane-cycle" in run.stderr, run.stderr
