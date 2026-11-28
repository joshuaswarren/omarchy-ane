#!/usr/bin/env python3
"""Self-test for tools/q6k_head_bench.py: the Q6_K head timer.

Imports the module (import does not pin: PINNED is guarded by __main__)
and checks the two testable pieces without a GGUF: measure() times
logits() per thread count and reports the JSON fields the bench prints,
and verdict() gates on the largest error of the first 256 logits being
finite and within the tolerance — a wrong kernel exits 3.
"""
import importlib.util
import math
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]
BLOCK = 210
K = 256


def dequant_row(blocks: np.ndarray, k: int) -> np.ndarray:
    """numpy reference for q6k_dequant_row: (d * scale) * q, fp32 left to right."""
    y = np.empty(k, dtype=np.float32)
    for b in range(k // 256):
        blk = blocks[b * BLOCK:(b + 1) * BLOCK]
        ql, qh = blk[:128], blk[128:192]
        sc = blk[192:208].view(np.int8)
        d = np.float32(blk[208:210].view(np.float16)[0])
        out = y[b * 256:]
        for half in range(2):
            for l in range(32):
                s = half * 8 + l // 16
                q1 = int(ql[half * 64 + l] & 0xF | (qh[half * 32 + l] & 3) << 4) - 32
                q2 = int(ql[half * 64 + l + 32] & 0xF | (qh[half * 32 + l] >> 2 & 3) << 4) - 32
                q3 = int(ql[half * 64 + l] >> 4 | (qh[half * 32 + l] >> 4 & 3) << 4) - 32
                q4 = int(ql[half * 64 + l + 32] >> 4 | (qh[half * 32 + l] >> 6 & 3) << 4) - 32
                o = half * 128
                out[o + l] = np.float32(d * np.float32(sc[s])) * np.float32(q1)
                out[o + l + 32] = np.float32(d * np.float32(sc[s + 2])) * np.float32(q2)
                out[o + l + 64] = np.float32(d * np.float32(sc[s + 4])) * np.float32(q3)
                out[o + l + 96] = np.float32(d * np.float32(sc[s + 6])) * np.float32(q4)
    return y


with tempfile.TemporaryDirectory() as tmp:
    os.environ["XDG_CACHE_HOME"] = tmp          # the q6k_head build caches under this
    sys.path.insert(0, str(REPO / "tools"))
    spec = importlib.util.spec_from_file_location("q6k_head_bench", REPO / "tools/q6k_head_bench.py")
    bench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bench)

    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    check("import does not pin", bench.PINNED, None)

    rows = 256                                # measure() compares the first CHECK_ROWS = 256 logits
    rng = np.random.default_rng(20261128)
    blocks = rng.integers(0, 256, size=rows * K // 256 * BLOCK, dtype=np.uint8)
    for b in range(rows * K // 256):
        # small magnitudes: the bench's 3e-6 absolute gate presumes real-model values, and
        # random bytes give NaN d ~4% of the time plus scales to 127 (|logit| ~1e3, reorder error ~1e-3)
        blocks[b * BLOCK + 192:b * BLOCK + 208] = rng.integers(0, 3, 16)
        blocks[b * BLOCK + 208:b * BLOCK + 210] = np.frombuffer(np.float16(0.00390625).tobytes(), np.uint8)
    x = (rng.standard_normal(K) * 0.01).astype(np.float32)
    ref = np.stack([dequant_row(blocks[r * K // 256 * BLOCK:(r + 1) * K // 256 * BLOCK], K)
                    for r in range(rows)]).astype(np.float64) @ x.astype(np.float64)

    per = bench.measure(blocks, rows, K, x, ref, [1, 2], reps=2)
    check("thread keys", sorted(per), ["1", "2"])
    for field in ("median_ms", "best_ms", "worst_ms", "gb_per_s", "max_abs_err_first256"):
        check(f"field {field}", all(field in r for r in per.values()), True)
    check("worst >= best", all(r["worst_ms"] >= r["best_ms"] for r in per.values()), True)
    check("gb/s positive", all(r["gb_per_s"] > 0 for r in per.values()), True)

    code, bad = bench.verdict(per, 3e-6)
    check("verdict pass code", code, 0)
    check("verdict pass reasons empty", bad, [])

    corrupt = {n: dict(r, max_abs_err_first256=r["max_abs_err_first256"] + 1.0)
               for n, r in per.items()}
    code, bad = bench.verdict(corrupt, 3e-6)
    check("verdict fail code", code, 3)
    check("verdict fail names both", len(bad), 2)

    naned = {n: dict(r, max_abs_err_first256=math.nan) for n, r in per.items()}
    code, bad = bench.verdict(naned, 3e-6)
    check("verdict NaN fails", code, 3)
    check("verdict NaN names both", len(bad), 2)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_q6k_head_bench: {len(fails)} failed")
        sys.exit(1)
    print("test_q6k_head_bench: ok")
    sys.exit(0)
