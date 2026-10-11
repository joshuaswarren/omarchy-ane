"""CPU-only: time the Qwen output projection (logits = head @ hidden) the way the resident decoder runs it.

The head is a float32 matrix of the tied embedding, about 248k x 2048 (2 GB). Random values of the same shape and
dtype give the same memory traffic and BLAS path, so no model file is needed. One BLAS thread count per process
(OPENBLAS_NUM_THREADS), because OpenBLAS reads it at import. Prints one line per variant.
"""
import os
import sys
import time

import numpy as np

V = int(os.environ.get("HB_VOCAB", "248320"))
D = int(os.environ.get("HB_DIM", "2048"))
REPS = int(os.environ.get("HB_REPS", "8"))
rng = np.random.default_rng(1)
head = rng.standard_normal((V, D), dtype=np.float32)
hidden = rng.standard_normal(D, dtype=np.float32)
head @ hidden  # warm the pages


def best_of(fn):
    times = []
    for _ in range(REPS):
        t0 = time.perf_counter()
        fn()
        times.append(time.perf_counter() - t0)
    times.sort()
    return times[0], times[len(times) // 2]


threads = os.environ.get("OPENBLAS_NUM_THREADS", "default")
lo, med = best_of(lambda: head @ hidden)
print(f"threads={threads:>7} head@hidden f32 [{V}x{D}]: best {lo * 1000:7.1f} ms  median {med * 1000:7.1f} ms")
logits = head @ hidden


def top2():
    first = int(np.argmax(logits))
    rest = logits.copy()
    rest[first] = -np.inf
    return first, int(np.argmax(rest))


lo, med = best_of(top2)
print(f"threads={threads:>7} top2 (copy + 2 argmax)   : best {lo * 1000:7.1f} ms  median {med * 1000:7.1f} ms")
sys.stdout.flush()

if os.environ.get("HB_FP16", "0") == "1":
    h16 = head.astype(np.float16)
    x16 = hidden.astype(np.float16)
    reps_saved = REPS
    REPS = 2
    lo, med = best_of(lambda: h16 @ x16)
    print(f"threads={threads:>7} head@hidden f16 numpy [{V}x{D}]: best {lo * 1000:7.1f} ms  median {med * 1000:7.1f} ms")

    def chunked():
        out = np.empty(V, dtype=np.float32)
        step = 16384
        for i in range(0, V, step):
            out[i:i + step] = h16[i:i + step].astype(np.float32) @ hidden
        return out

    lo, med = best_of(chunked)
    print(f"threads={threads:>7} head@hidden f16->f32 chunked : best {lo * 1000:7.1f} ms  median {med * 1000:7.1f} ms")
    REPS = reps_saved
    sys.stdout.flush()
