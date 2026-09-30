import unittest

import numpy as np

from island_probes import compare_fp16_surface


class PaddedSurfaceTests(unittest.TestCase):
    def test_logical_comparison_skips_each_rows_padding(self):
        desc = {"N": 1, "C": 1, "H": 2, "W": 3, "row_bytes": 8}
        flat = np.array([1, 2, 3, 99, 4, 5, 6, 88], dtype=np.float16)
        logical = np.array([[[[1, 2, 3], [4, 5, 6]]]], dtype=np.float16)
        ref_bits = logical.view(np.uint16).reshape(-1)

        exact, pad_nonzero = compare_fp16_surface(desc, logical, flat, ref_bits)

        self.assertEqual(exact, 6)
        self.assertEqual(pad_nonzero, 2)


if __name__ == "__main__":
    unittest.main()
