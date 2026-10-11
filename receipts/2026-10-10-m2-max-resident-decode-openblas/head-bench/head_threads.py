"""CPU-only: the float32 head matvec split into row chunks over a Python thread pool (BLAS calls release the GIL)."""
import os
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

V, D = 248320, 2048
rng = np.random.default_rng(1)
head = rng.standard_normal((V, D), dtype=np.float32)
hidden = rng.standard_normal(D, dtype=np.float32)
want = head @ hidden
for workers in (2, 4, 8, 12):
    chunk = -(-V // workers)
    out = np.empty(V, dtype=np.float32)

    def part(i, out=out, chunk=chunk):
        lo = i * chunk
        hi = min(V, lo + chunk)
        np.matmul(head[lo:hi], hidden, out=out[lo:hi])

    with ThreadPoolExecutor(workers) as pool:
        times = []
        for _ in range(5):
            t0 = time.perf_counter()
            list(pool.map(part, range(workers)))
            times.append(time.perf_counter() - t0)
    times.sort()
    print(f"workers={workers:2d} chunked f32 matvec: best {times[0] * 1000:7.1f} ms  median {times[2] * 1000:7.1f} ms  equal={bool(np.array_equal(out, want))}  max_abs_diff={float(np.abs(out - want).max()):.2e}")
print("cpu count", os.cpu_count())
