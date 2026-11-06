#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline source-contract tests; not device qualification."""
from dataclasses import replace
import unittest

from chanreg_model import (
    ChannelRecord, IPC_IOVA, Region, registration_init_ranges,
)


class RegistrationInitTests(unittest.TestCase):
    def setUp(self):
        self.ipc = Region("ipc", IPC_IOVA, 0xC441, 0x10000, True, True)
        self.records = [
            ChannelRecord(0, "TERMINAL", 2, 0, 0x200, IPC_IOVA + 0x800),
            ChannelRecord(1, "IO", 0, 1, 0x10, IPC_IOVA + 0x8800),
            ChannelRecord(2, "DEBUG", 0, 2, 0x08, IPC_IOVA + 0x8C00),
            ChannelRecord(3, "BUF_H2T", 0, 3, 0x40, IPC_IOVA + 0x8E00),
            ChannelRecord(4, "BUF_T2H", 1, 4, 0x40, IPC_IOVA + 0x9E00),
            ChannelRecord(5, "SHAREDMALLOC", 1, 5, 0x08, IPC_IOVA + 0xAE00),
            ChannelRecord(6, "IO_T2H", 1, 6, 0x40, IPC_IOVA + 0xB000),
            ChannelRecord(7, "DATA_CHAIN_H2T", 0, 7, 0x10, IPC_IOVA + 0xC000),
        ]

    def test_observed_type_zero_initialization_ranges(self):
        self.assertEqual(registration_init_ranges(self.records, self.ipc), (
            (1, 0x8800, 0x400),
            (2, 0x8C00, 0x200),
            (3, 0x8E00, 0x1000),
            (7, 0xC000, 0x400),
        ))

    def test_type_one_and_two_buffers_are_not_host_initialized(self):
        # This checks only the source-traced host writes; it does not bless
        # firmware-side accesses to these table-provided pointers.
        records = list(self.records)
        records[0] = replace(records[0], buffer_iova=(1 << 64) - 1)
        records[4] = replace(records[4], buffer_iova=(1 << 64) - 1)
        records[5] = replace(records[5], buffer_iova=(1 << 64) - 1)
        records[6] = replace(records[6], buffer_iova=(1 << 64) - 1)
        self.assertEqual(len(registration_init_ranges(records, self.ipc)), 4)

    def test_initialized_range_must_fit_logical_ipc_not_padding(self):
        records = list(self.records)
        records[7] = replace(records[7], buffer_iova=IPC_IOVA + 0xC080)
        with self.assertRaisesRegex(ValueError, "complete range is not owned"):
            registration_init_ranges(records, self.ipc)

    def test_initialized_ranges_must_not_overlap(self):
        records = list(self.records)
        records[2] = replace(records[2], buffer_iova=IPC_IOVA + 0x8900)
        with self.assertRaisesRegex(ValueError, "overlap"):
            registration_init_ranges(records, self.ipc)

    def test_requires_exact_ordered_eight_entries(self):
        for records in (self.records[:-1], [replace(self.records[1], index=0)] + self.records[1:]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                registration_init_ranges(records, self.ipc)

    def test_required_channel_names_must_be_present(self):
        records = list(self.records)
        records[5] = replace(records[5], name="OTHER")
        with self.assertRaisesRegex(ValueError, "required channel names"):
            registration_init_ranges(records, self.ipc)

    def test_duplicate_names_are_rejected(self):
        records = list(self.records)
        records[2] = replace(records[2], name="IO")
        with self.assertRaisesRegex(ValueError, "duplicate channel name"):
            registration_init_ranges(records, self.ipc)

    def test_unowned_ipc_is_rejected(self):
        ipc = replace(self.ipc, owned=False)
        with self.assertRaisesRegex(ValueError, "unowned"):
            registration_init_ranges(self.records, ipc)


if __name__ == "__main__":
    unittest.main()
