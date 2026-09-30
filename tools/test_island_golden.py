import unittest

import numpy as np

from island_golden import bmm_verdict, metrics_bmm


class BmmVerdictTests(unittest.TestCase):
    def test_insufficient_numeric_in_band_rate_fails(self):
        self.assertEqual(
            bmm_verdict({
                "pad_zero": True,
                "nan_count": 0,
                "inf_count": 0,
                "rel_l2": 0.002,
                "in_band_pct": 90.0,
            }),
            "FAIL",
        )

    def test_documented_in_band_rate_and_zero_padding_pass(self):
        self.assertEqual(
            bmm_verdict({
                "pad_zero": True,
                "nan_count": 0,
                "inf_count": 0,
                "rel_l2": 0.002,
                "in_band_pct": 99.5,
            }),
            "PASS",
        )
    def test_wrong_numeric_output_fails_through_metrics(self):
        shape = {"N": 1, "C": 1, "H": 1, "W": 1, "row_bytes": 64}
        x = np.ones((1, 1, 1, 1), dtype=np.float16)
        w = np.ones((1, 1, 1, 1), dtype=np.float16)
        ref = np.ones((1, 1, 1, 1), dtype=np.float16)
        dev = np.zeros_like(ref)

        metrics = metrics_bmm(dev, ref, shape, w, x)

        self.assertEqual(metrics["in_band_pct"], 0.0)
        self.assertEqual(bmm_verdict(metrics), "FAIL")


if __name__ == "__main__":
    unittest.main()
