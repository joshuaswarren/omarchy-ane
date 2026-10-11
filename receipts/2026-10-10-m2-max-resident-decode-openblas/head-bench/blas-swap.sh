#!/bin/bash
# blas-swap.sh : make the system BLAS OpenBLAS (Arch: blas-openblas replaces blas, cblas, lapack) and make sure numpy exists.
# Prints the package state before and after. Rollback: sudo pacman -S blas cblas lapack (they conflict with blas-openblas).
set -u
echo "== $(date -u +%FT%TZ) $(uname -n | sed 's/.*/host/') kernel=$(uname -r)"
echo "before: $(pacman -Q blas cblas lapack blas-openblas openblas python-numpy 2>&1 | tr '\n' ';')"
sudo -n pacman -S --noconfirm --ask 4 blas-openblas python-numpy 2>&1 | grep -E "^(removing|installing|upgrading|warning|error)|there is nothing" 
echo "after: $(pacman -Q blas cblas lapack blas-openblas openblas python-numpy 2>&1 | tr '\n' ';')"
echo "libcblas -> $(readlink -f /usr/lib/libcblas.so.3)"
python3 - <<'PY'
import time
import numpy as np
V, D = 248320, 2048
h = np.random.default_rng(1).standard_normal((V, D), dtype=np.float32)
x = np.random.default_rng(2).standard_normal(D, dtype=np.float32)
h @ x
best = 9.0
for _ in range(5):
    t = time.perf_counter()
    h @ x
    best = min(best, time.perf_counter() - t)
print(f"numpy {np.__version__}: head @ hidden f32 [{V}x{D}] best {best * 1000:.1f} ms")
PY
