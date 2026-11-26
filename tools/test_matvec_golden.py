#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Pin the H14 matvec golden orientation against the sibling harness.

The kit programs (h14-matvec.anec, h14-matvec-m8.anec) compute y = x @ W.
The sibling M2 harness (prof/w73-h14-matvec-ab.py, w73 lane) uses
cpu_golden = matmul(x, w). This test asserts matvec_golden.golden agrees
with that convention, and that the transposed convention produces
exactly the 1.7-2.2 max_abs band the November 25-26 supply rows printed -
so a future transposed golden cannot masquerade as a driver regression.
"""

import importlib.util
import os
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
AB = os.environ.get("W73_MATVEC_AB", "")

spec = importlib.util.spec_from_file_location("mg", os.path.join(HERE, "matvec_golden.py"))
assert spec and spec.loader
mg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mg)

ab = None


class GoldenOrientationTests(unittest.TestCase):
    def test_golden_matches_sibling_harness_convention(self):
        """matvec_golden.golden must equal ab.cpu_golden for the same inputs."""
        if not AB or not os.path.exists(AB):
            self.skipTest("sibling harness path not provided via W73_MATVEC_AB")
        global ab
        ab_spec = importlib.util.spec_from_file_location("ab", AB)
        assert ab_spec and ab_spec.loader
        ab = importlib.util.module_from_spec(ab_spec)
        ab_spec.loader.exec_module(ab)
        for r in (8, 16, 32, 64):
            x = mg.gen_x(r)
            w = np.random.default_rng(r).random((mg.K, mg.N)).astype(np.float16)
            a = mg.golden(x, w).astype(np.float16)
            b = ab.cpu_golden(x.astype(np.float16), w)
            self.assertEqual(a.shape, b.shape)
            # cpu_golden rounds to f16; the conventions must agree exactly there
            np.testing.assert_array_equal(a, b.astype(np.float16))

    def test_transposed_convention_exceeds_tolerance(self):
        """A transposed golden differs from the program convention by more than
        tol - the band rows 3579/3585 misreported as driver damage. With the
        real kit weight the band is exactly 1.7-2.2 (Nov 25-26 supply rows)."""
        w = np.random.default_rng(7).random((mg.K, mg.N)).astype(np.float16) * 0.02
        x = mg.gen_x(64)
        delta = mg.orientation_delta(x, w)
        self.assertGreater(delta, mg.TOL)
        y = (x.astype(np.float32) @ w.astype(np.float32)).astype(np.float16)
        wrong_view = mg.transposed_view(y, 64)
        self.assertGreater(
            float(np.max(np.abs(wrong_view - mg.golden(x, w).astype(np.float32)))),
            mg.TOL,
            "transposed view must exceed tol - it is not a numeric fault",
        )

    def test_real_kit_weight_reproduces_the_november_band(self):
        """Pin the exact November numbers when the real weight file is present."""
        weight = os.environ.get("W73_WEIGHT_KN", "")
        if not weight or not os.path.exists(weight):
            self.skipTest("kit weight not staged on this host")
        w = mg.load_weight(weight)
        # the runner consumes ONE sequential rng across arms (8,16,32,64);
        # replicate that so the x streams match the November run exactly
        rng = np.random.default_rng(mg.SEED)
        for r, band_lo, band_hi in ((8, 1.846, 1.849), (16, 1.821, 1.824),
                                    (32, 1.744, 1.746), (64, 2.213, 2.216)):
            x = ((rng.random((r, mg.K)) - 0.5)).astype(np.float16)
            delta = mg.orientation_delta(x, w)
            self.assertGreaterEqual(delta, band_lo, f"r={r}")
            self.assertLessEqual(delta, band_hi, f"r={r}")

    def test_program_convention_passes_tolerance_on_f16_rounding(self):
        """y = x@W in f16 against the fp32 golden stays inside tol=0.08."""
        w = np.random.default_rng(11).random((mg.K, mg.N)).astype(np.float16)
        x = mg.gen_x(64)
        y = (x @ w).astype(np.float16)
        max_abs = float(
            np.max(np.abs(y.astype(np.float32) - mg.golden(x, w).astype(np.float32)))
        )
        self.assertLessEqual(max_abs, mg.TOL)


if __name__ == "__main__":
    unittest.main()
