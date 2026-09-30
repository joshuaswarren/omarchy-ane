import unittest

import numpy as np

from island_golden import bmm_reference_from_inputs, bmm_verdict, metrics_bmm


class BmmVerdictTests(unittest.TestCase):
    def test_padded_flat_inputs_reach_bmm_verdict(self):
        desc = {"N": 1, "C": 1, "H": 1, "W": 1, "row_bytes": 64}
        surface = np.zeros(32, dtype=np.float16)
        surface[0] = 1
        island = {"channels": {5: desc, 6: desc}}
        ref = bmm_reference_from_inputs(island, surface, surface, desc)
        device = ref.astype(np.float16).astype(np.float64)

        try:
            metrics = metrics_bmm(device, ref, desc, surface, surface, desc, desc)
        except ValueError as exc:
            self.fail(f"padded input surfaces must be unpacked before einsum: {exc}")

        self.assertEqual(bmm_verdict(metrics), "PASS")

    def test_near_zero_result_compares_to_fp16_rounded_reference(self):
        desc = {"N": 1, "C": 1, "H": 1, "W": 1, "row_bytes": 64}
        x = np.array([[[[0.02]]]], dtype=np.float16)
        w = np.array([[[[0.02]]]], dtype=np.float16)
        ref = x.astype(np.float64) * w.astype(np.float64)
        dev = ref.astype(np.float16).astype(np.float64)

        metrics = metrics_bmm(dev, ref, desc, w, x)

        self.assertEqual(metrics["in_band_pct"], 100.0)

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
