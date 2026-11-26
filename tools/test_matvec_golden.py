#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Pin the matvec golden orientation (x @ W) against the sibling harness.

The kit programs (h14-matvec.anec, h14-matvec-m8.anec) compute y = x @ W.
The sibling harness (prof/w73-h14-matvec-ab.py) uses cpu_golden = matmul(x, w).
This test asserts matvec_golden.golden agrees with that convention, that the
CLI preserves the runner's single sequential RNG stream across arms, and that
the transposed convention produces the 1.7-2.2 max_abs band the November 25-26
supply rows printed - so a future transposed golden cannot masquerade as a
driver regression.
"""

import importlib.util
import os
import re
import subprocess
import sys
import tempfile
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

    def test_golden_orientation_asymmetric_not_transposed(self):
        """Unconditionally verify golden(x, W) computes x @ W (not x @ W.T)."""
        rng = np.random.default_rng(19)
        x = (rng.random((8, mg.K)) - 0.5).astype(np.float16)
        w = (rng.random((mg.K, mg.N)) - 0.5).astype(np.float16)
        # Break symmetry explicitly so x @ W and x @ W.T cannot coincide.
        w[0, 1] = np.float16(1.5)
        w[1, 0] = np.float16(-1.5)
        expected = np.matmul(x.astype(np.float32), w.astype(np.float32))
        np.testing.assert_array_equal(mg.golden(x, w), expected)
        err_ok, passed_ok = mg.check_output(expected.astype(np.float16), x, w)
        self.assertTrue(passed_ok, f"expected pass, got max_abs={err_ok}")
        wrong = np.matmul(x.astype(np.float32), w.astype(np.float32).T).astype(np.float16)
        err_bad, passed_bad = mg.check_output(wrong, x, w)
        self.assertFalse(passed_bad, f"transposed output must fail, got max_abs={err_bad}")

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

    def test_cli_uses_sequential_rng_stream(self):
        """CLI must consume one sequential RNG across 8/16/32/64, matching the runner."""
        w_f16 = (np.random.default_rng(23).random((mg.K, mg.N)) - 0.5).astype(np.float16) * np.float16(0.1)
        w_f32 = w_f16.astype(np.float32)
        expected = {r: mg.orientation_delta(x, w_f32) for r, x in mg.gen_x_stream()}
        reseeded = {r: mg.orientation_delta(mg.gen_x(r), w_f32) for r in mg.DEFAULT_ROWS}
        # Arms 16/32/64 must differ between sequential stream and per-arm re-seeding.
        for r in (16, 32, 64):
            self.assertNotAlmostEqual(expected[r], reseeded[r], places=4)
        with tempfile.TemporaryDirectory(prefix="matvec-golden-") as td:
            wp = os.path.join(td, "weight-kn.f16")
            w_f16.tofile(wp)
            proc = subprocess.run(
                [sys.executable, os.path.join(HERE, "matvec_golden.py"), wp],
                capture_output=True, text=True, check=True,
            )
        seen = {}
        for line in proc.stdout.splitlines():
            m = re.match(r"^r=(\d+) orientation_delta=([0-9.]+)$", line.strip())
            if m:
                seen[int(m.group(1))] = float(m.group(2))
        self.assertEqual(set(seen.keys()), set(mg.DEFAULT_ROWS))
        for r in mg.DEFAULT_ROWS:
            self.assertAlmostEqual(seen[r], expected[r], places=5, msg=f"r={r}")

    def test_real_kit_weight_reproduces_the_november_band(self):
        """Pin the exact November numbers when the real weight file is present."""
        weight = os.environ.get("W73_WEIGHT_KN", "")
        if not weight or not os.path.exists(weight):
            self.skipTest("kit weight not staged on this host")
        w = mg.load_weight(weight)
        bands = {8: (1.846, 1.849), 16: (1.821, 1.824),
                 32: (1.744, 1.746), 64: (2.213, 2.216)}
        for r, x in mg.gen_x_stream():
            lo, hi = bands[r]
            delta = mg.orientation_delta(x, w)
            self.assertGreaterEqual(delta, lo, f"r={r}")
            self.assertLessEqual(delta, hi, f"r={r}")

    def test_program_convention_passes_tolerance_on_f16_rounding(self):
        """y = x@W in f16 against the fp32 golden stays inside tol=0.08."""
        w = np.random.default_rng(11).random((mg.K, mg.N)).astype(np.float16)
        x = mg.gen_x(64)
        y = (x @ w).astype(np.float16)
        max_abs, passed = mg.check_output(y, x, w)
        self.assertTrue(passed, f"max_abs={max_abs} > tol={mg.TOL}")


if __name__ == "__main__":
    unittest.main()
