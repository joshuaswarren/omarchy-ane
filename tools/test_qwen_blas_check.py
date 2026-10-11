#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Offline checks for qwen_m2_decode.reference_blas (the single-thread reference BLAS warning).

On the M2 Max the reference BLAS made the output projection (head @ hidden, float32, 248320 x 2048) take
604 ms at any thread count; OpenBLAS takes about 20 ms (receipts/2026-10-10-m2-max-resident-decode-openblas).
The decoder warns once at the start of a run when numpy maps the reference libcblas and no OpenBLAS.

Checks, driven with the text of /proc/self/maps so no BLAS has to be swapped on the host:
  1. reference libcblas and libblas mapped, no OpenBLAS: warns (True).
  2. blas-openblas: libcblas.so.3 resolves to libopenblas.so.0.3, which is what the map shows: no warning.
  3. a numpy wheel with its own OpenBLAS (a scipy_openblas library): no warning.
  4. no BLAS library mapped at all (a numpy built without one): no warning.
  5. live call on this host returns a bool and does not raise.
  6. OpenBLAS mapped next to a libcblas wrapper: no warning.
Negative control: the test rebuilds the function without the "openblas" clause and asserts that this mutant
misreports check 6, so a weakened function cannot pass.
"""

from pathlib import Path

import qwen_m2_decode as decode

REFERENCE = """ffff352f0000-ffff352f9000 r-xp 00000000 00:00 1 /usr/lib/libcblas.so.3.12.0
ffff34c30000-ffff34c90000 r-xp 00000000 00:00 2 /usr/lib/libblas.so.3.12.0
"""
OPENBLAS = "ffff35100000-ffff36400000 r-xp 00000000 00:00 3 /usr/lib/libopenblas.so.0.3\n"
WHEEL = "ffff30000000-ffff31000000 r-xp 00000000 00:00 4 /venv/lib/python3.14/site-packages/numpy.libs/libscipy_openblas64_-aaaa.so\n"
NONE = "ffff30000000-ffff31000000 r-xp 00000000 00:00 5 /usr/lib/libm.so.6\n"
MIXED = OPENBLAS + "ffff35000000-ffff35100000 r-xp 00000000 00:00 6 /usr/lib/libcblas.so.3.12.0\n"


def main():
    assert decode.reference_blas(REFERENCE) is True, "reference libcblas must warn"
    assert decode.reference_blas(OPENBLAS) is False, "blas-openblas must not warn"
    assert decode.reference_blas(WHEEL) is False, "a wheel with its own OpenBLAS must not warn"
    assert decode.reference_blas(NONE) is False, "no BLAS library mapped must not warn"
    assert decode.reference_blas(MIXED) is False, "OpenBLAS next to libcblas must not warn"
    assert isinstance(decode.reference_blas(), bool)
    source = Path(decode.__file__).read_text().replace('"openblas" not in maps_text and (', "(")
    assert source != Path(decode.__file__).read_text(), "mutation did not apply"
    mutant = {}
    exec(compile(source, "mutant", "exec"), mutant)
    assert mutant["reference_blas"](MIXED) is True, "the mutant must misreport the mixed map"
    assert "blas-openblas" in decode.REFERENCE_BLAS_WARNING
    print("PASS qwen_m2_decode.reference_blas: 5 map cases, the live call and the mutation control")


if __name__ == "__main__":
    main()
