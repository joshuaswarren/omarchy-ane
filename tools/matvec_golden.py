#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Golden reference for the M2 H14 matvec kit programs (h14-matvec.anec).

The kit programs compute y = x @ W with W stored K x N in weight-kn.f16.
w73 supply rows 3475/3579/3585 failed 0/4 with max_abs equal to the
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


def gen_x(rows, k=K, seed=SEED):
    rng = np.random.default_rng(seed)
    return ((rng.random((rows, k)) - 0.5)).astype(np.float16)


def load_weight(path):
    return np.fromfile(path, dtype=np.float16).astype(np.float32).reshape(K, N)


def golden(x, w):
    """The kit program convention: y = x @ W, W stored K x N."""
    return np.matmul(x.astype(np.float32), w.astype(np.float32))


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
    for r in (8, 16, 32, 64):
        print(f"r={r} orientation_delta={orientation_delta(gen_x(r), w):.6f}")
