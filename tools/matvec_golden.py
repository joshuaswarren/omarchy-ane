#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Golden reference for the ANE matvec kit programs (y = x @ W).

The kit programs compute y = x @ W with W stored K x N in weight-kn.f16.
Supply rows 3475/3579/3585 failed 0/4 with max_abs equal to the
orientation delta max_abs(x@W - x@W.T) on the real weight (1.847/1.822/
1.745/2.214 for r8/16/32/64), in both the row-major and transposed views -
proving the programs were right all along and the scratch runner's
golden (x @ W.T) was the wrong convention.

This module is the single source of the correct convention, and the test
pins it against the sibling harness (prof/w73-h14-matvec-ab.py), whose
cpu_golden is matmul(x, w) - the two harnesses must agree.
"""

import numpy as np

K = N = 1024
SEED = 20261121
TOL = 0.08
DEFAULT_ROWS = (8, 16, 32, 64)


def gen_x(rows, k=K, seed=SEED, rng=None):
    if rng is None:
        rng = np.random.default_rng(seed)
    return ((rng.random((rows, k)) - 0.5)).astype(np.float16)


def gen_x_stream(rows_seq=DEFAULT_ROWS, k=K, seed=SEED):
    """Yield (rows, x) from one sequential RNG across arms, matching the runner."""
    rng = np.random.default_rng(seed)
    for rows in rows_seq:
        yield rows, gen_x(rows, k=k, rng=rng)


def load_weight(path, k=K, n=N):
    return np.fromfile(path, dtype=np.float16).astype(np.float32).reshape(k, n)


def golden(x, w):
    """The kit program convention: y = x @ W, W stored K x N."""
    return np.matmul(x.astype(np.float32), w.astype(np.float32))


def check_output(y, x, w, tol=TOL):
    """Compare output y against golden(x, w); return (max_abs, passed)."""
    ref = golden(x, w).astype(np.float32)
    max_abs = float(np.max(np.abs(y.astype(np.float32) - ref)))
    return max_abs, max_abs <= tol


def orientation_delta(x, w):
    """max_abs(x@W - x@W.T) - the exact signature the transposed golden prints."""
    d = golden(x, w).astype(np.float32) - np.matmul(
        x.astype(np.float32), w.astype(np.float32).T)
    return float(np.max(np.abs(d)))


def transposed_view(y, rows, n=N):
    """The wrong-convention view a failing runner computes from the same file."""
    flat = np.frombuffer(y.astype(np.float32).tobytes(), dtype=np.float32)
    return flat[: rows * n].reshape(n, rows).T


if __name__ == "__main__":
    import sys

    w = load_weight(sys.argv[1])
    for r, x in gen_x_stream():
        print(f"r={r} orientation_delta={orientation_delta(x, w):.6f}")
