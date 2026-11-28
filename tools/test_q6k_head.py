#!/usr/bin/env python3
"""Self-test for tools/q6k_head.py: the Q6_K output head.

Builds the C library in a temp dir (XDG_CACHE_HOME pointed at it) and
checks it against the ggml block_q6_K layout spelled out in q6k_head.c:
ql[128] low nibbles, qh[64] high 2 bits, scales[16] int8, d fp16, and a
weight is d * scales[g] * (q - 32). embed_row is promised bit-for-bit;
logits uses a different rounding order so it is compared with a
tolerance, plus the constructor and argument refusals.
"""
import importlib.util
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
    os.environ["XDG_CACHE_HOME"] = tmp          # build() caches under this
    spec = importlib.util.spec_from_file_location("q6k_head", REPO / "tools/q6k_head.py")
    qh_mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(qh_mod)

    fails = []

    def check(name, got, want):
        ok = np.array_equal(got, want) if isinstance(want, np.ndarray) else got == want
        if not ok:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    rows = 4
    rng = np.random.default_rng(20261128)
    blocks = rng.integers(0, 256, size=rows * K // 256 * BLOCK, dtype=np.uint8)
    for b in range(rows * K // 256):            # random bytes are NaN as fp16 ~4% of the time
        blocks[b * BLOCK + 208:b * BLOCK + 210] = np.frombuffer(np.float16(0.5 + b).tobytes(), np.uint8)
    head = qh_mod.Q6KHead(blocks, rows=rows, k=K, threads=2)

    # embed_row is bit-for-bit the reference dequant
    check("embed_row bit-exact", head.embed_row(2), dequant_row(blocks[2 * K // 256 * BLOCK:3 * K // 256 * BLOCK], K))
    check("embed_row 0", head.embed_row(0), dequant_row(blocks[:K // 256 * BLOCK], K))

    # logits = dequant @ x at fp32 rounding (different order, so tolerance not bit-equal)
    x = rng.standard_normal(K).astype(np.float32)
    ref_y = np.stack([dequant_row(blocks[r * K // 256 * BLOCK:(r + 1) * K // 256 * BLOCK], K) for r in range(rows)]) @ x
    got_y = head.logits(x)
    check("logits shape", got_y.shape, (rows,))
    if not np.allclose(got_y, ref_y, rtol=1e-5, atol=1e-3):
        fails.append(f"logits off reference: max diff {np.abs(got_y - ref_y).max()}")
    else:
        print("ok   logits within rounding of dequant @ x")

    # an fp16-representable x takes the exact path (result still matches reference)
    x16 = rng.standard_normal(K).astype(np.float16).astype(np.float32)
    got16 = head.logits(x16)
    if not np.allclose(got16, ref_y @ (x16 / x) if False else np.stack(
            [dequant_row(blocks[r * K // 256 * BLOCK:(r + 1) * K // 256 * BLOCK], K) for r in range(rows)]) @ x16,
            rtol=1e-5, atol=1e-3):
        fails.append("fp16-exact logits off reference")
    else:
        print("ok   fp16-exact logits within reference")

    # refusals
    try:
        qh_mod.Q6KHead(np.zeros(BLOCK, dtype=np.uint8), rows=2, k=K)
        fails.append("short blocks: no error")
    except ValueError:
        print("ok   short blocks refused")

    try:
        qh_mod.Q6KHead(np.zeros(BLOCK, dtype=np.uint8), rows=1, k=100)
        fails.append("k not multiple of 256: no error")
    except ValueError:
        print("ok   k not multiple of 256 refused")

    try:
        head.logits(np.zeros(K + 1, dtype=np.float32))
        fails.append("wrong hidden size: no error")
    except ValueError:
        print("ok   wrong hidden size refused")

    try:
        head.embed_row(rows)
        fails.append("token out of range: no error")
    except IndexError:
        print("ok   token out of range refused")

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_q6k_head: {len(fails)} failed")
        sys.exit(1)
    print("test_q6k_head: ok")
    sys.exit(0)
