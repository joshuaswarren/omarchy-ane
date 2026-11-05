"""Offline arithmetic regressions; successful firmware mapping is an assumption."""
import unittest
from evidence import model


class AddressModelTests(unittest.TestCase):
    def test_rung1_predicts_recorded_address(self):
        r = model(0x3ffffbc0000, 0x3ffffbb0000)
        self.assertEqual(r['channel0_full_pointer'], '0x3ffffbb0800')
        self.assertEqual(r['channel0_narrowed_pointer'], '0xffbb0800')
        self.assertFalse(r['alias_survives_narrowing'])
        self.assertFalse(r['logical_ipc_alias_fits_low32'])

    def test_compatible_arithmetic_not_mapping_authorization(self):
        r = model(0x10000300000, 0x10000400000)
        self.assertTrue(r['alias_survives_narrowing'])
        self.assertTrue(r['reply_equals_original_table'])
        self.assertTrue(r['logical_ipc_alias_fits_low32'])

    def test_mixed_high_halves_break_reply_reconstruction(self):
        r = model(0x3ffffbc0000, 0x10000400000)
        self.assertTrue(r['alias_survives_narrowing'])
        self.assertFalse(r['reply_equals_original_table'])

    def test_low32_boundary_before_first_channel_pointer(self):
        r = model(0x10000300000, 0x100fffffc00)
        self.assertFalse(r['alias_survives_narrowing'])

    def test_first_pointer_fits_but_later_extent_crosses_boundary(self):
        r = model(0x10000300000, 0x100fffff000)
        self.assertTrue(r['alias_survives_narrowing'])
        self.assertFalse(r['logical_ipc_alias_fits_low32'])


if __name__ == '__main__':
    unittest.main()
