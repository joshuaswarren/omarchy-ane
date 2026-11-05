#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Synthetic offline tests, not hardware qualification."""
from dataclasses import replace
import struct
import unittest

from chanreg_model import (
    BootInputs, Region, PAGE, VM_BASE, VM_END, FW_BYTES, DM_BYTES,
    DESCRIPTOR_ALLOCATION, build_descriptor, round_page, table_offset,
)


class DescriptorTests(unittest.TestCase):
    def setUp(self):
        # Values from the recorded scratch snapshot; addresses are SYNTHETIC.
        self.inputs = BootInputs(8, 0xC440, 0xF00000)
        self.fw = Region("firmware", VM_BASE, FW_BYTES, FW_BYTES, True, True)
        self.ipc = Region("ipc", VM_BASE + 0x380000, 0xC441, 0x10000, True, True)
        self.dm = Region("dm", VM_BASE + 0x390000, DM_BYTES, DM_BYTES, True, True)
        self.heap = Region("heap", VM_BASE + 0x400000, 0xF00000, 0xF00000, True, True)

    def build(self, **changes):
        args = dict(inputs=self.inputs, firmware=self.fw, ipc=self.ipc,
                    dm=self.dm, heap=self.heap)
        args.update(changes)
        return build_descriptor(**args)

    def test_snapshot_geometry(self):
        self.assertEqual(self.inputs.ipc_requested, 0xC441)
        self.assertEqual(round_page(self.inputs.ipc_requested), 0x10000)
        self.assertEqual(self.inputs.heap_requested, 0xF00000)
        for size in (-1, (1 << 64) - 1, 1 << 64):
            with self.subTest(size=size), self.assertRaises(ValueError):
                round_page(size)

    def test_every_byte_against_independent_store_list(self):
        got, address = self.build()
        expected = bytearray(0x180)
        values = (self.fw.iova, self.ipc.iova, 0x380000, 0xFC80000,
                  self.heap.iova, 0xF00000)
        struct.pack_into("<6Q", expected, 0, *values)
        struct.pack_into("<3Q", expected, 0x50, 0xC441, self.dm.iova, 0x40000)
        struct.pack_into("<I", expected, 0x68, 0x40)
        struct.pack_into("<I", expected, 0x12C, 4)
        self.assertEqual(address, self.dm.iova)
        self.assertEqual(got, expected)

    def test_optional_heap_absent(self):
        got, _ = self.build(inputs=replace(self.inputs, heap_bytes=0), heap=None)
        self.assertEqual(got[0x20:0x30], bytes(16))

    def test_minimum_allocation_and_all_option_combinations(self):
        inputs = BootInputs(1, 0, 1)
        ipc = replace(self.ipc, size=PAGE, mapped_bytes=PAGE)
        heap = replace(self.heap, size=PAGE, mapped_bytes=PAGE)
        for flags in (4, 12, 20, 28):
            with self.subTest(flags=flags):
                blob, _ = self.build(inputs=replace(inputs, option_flags=flags),
                                     ipc=ipc, heap=heap)
                self.assertEqual(struct.unpack_from("<Q", blob, 0x50)[0], 1)
                self.assertEqual(struct.unpack_from("<Q", blob, 0x28)[0], PAGE)
                self.assertEqual(struct.unpack_from("<I", blob, 0x12C)[0], flags)

    def test_last_descriptor_slot(self):
        _, address = self.build(offset=DM_BYTES - DESCRIPTOR_ALLOCATION)
        self.assertEqual(address + DESCRIPTOR_ALLOCATION, self.dm.iova + DM_BYTES)
        for offset in (-1, 1, DM_BYTES - 0x140, DM_BYTES, 1 << 32):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                self.build(offset=offset)

    def test_invalid_inputs(self):
        for field, values in (
            ("channel_count", (-1, 0, 33, 1 << 32, True)),
            ("ipc_last_offset", (-1, 0xFFFFFFFF, 1 << 32)),
            ("heap_bytes", (-1, 1 << 32)),
            ("option_flags", (0, 1, 5, 0x24)),
            ("firmware_argument", (-1, 1 << 32)),
        ):
            for value in values:
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    self.build(inputs=replace(self.inputs, **{field: value}))

    def test_invalid_regions(self):
        for key, region in (
            ("ipc", replace(self.ipc, owned=False)),
            ("ipc", replace(self.ipc, retained=False)),
            ("ipc", replace(self.ipc, mapped_bytes=PAGE)),
            ("ipc", replace(self.ipc, size=self.ipc.size + 1)),
            ("ipc", replace(self.ipc, iova=self.fw.iova)),
            ("ipc", replace(self.ipc, iova=VM_BASE - PAGE)),
            ("ipc", replace(self.ipc, iova=VM_END)),
            ("ipc", replace(self.ipc, iova=(1 << 64) - PAGE)),
            ("ipc", replace(self.ipc, iova=self.ipc.iova + 1)),
            ("firmware", replace(self.fw, size=FW_BYTES - PAGE)),
            ("dm", replace(self.dm, size=PAGE)),
            ("heap", None),
        ):
            with self.subTest(key=key, region=region), self.assertRaises(ValueError):
                self.build(**{key: region})

    def test_complete_reply_range(self):
        self.assertEqual(table_offset(self.inputs, self.ipc, self.ipc.iova), 0)
        last = self.ipc.iova + self.ipc.size - 0x800
        self.assertEqual(table_offset(self.inputs, self.ipc, last), 0xBC41)
        for address in (last + 1, self.ipc.iova - 1,
                        self.ipc.iova + self.ipc.size, self.dm.iova,
                        self.fw.iova, (1 << 64) - 1, -1):
            with self.subTest(address=address), self.assertRaises(ValueError):
                table_offset(self.inputs, self.ipc, address)

    def test_count_boundaries_and_firmware_argument(self):
        for count in (1, 32):
            inputs = replace(self.inputs, channel_count=count, firmware_argument=0xFFFFFFFF)
            blob, _ = self.build(inputs=inputs)
            self.assertEqual(struct.unpack_from("<I", blob, 0x30)[0], 0xFFFFFFFF)
            self.assertEqual(blob[0x34:0x38], bytes(4))
            self.assertEqual(table_offset(inputs, self.ipc, self.ipc.iova), 0)

    def test_unasserted_ownership_is_refused(self):
        with self.assertRaises(ValueError):
            self.build(ipc=Region("ipc", self.ipc.iova, self.ipc.size, self.ipc.mapped_bytes))

    def test_no_heap_cannot_publish_extra_heap(self):
        with self.assertRaises(ValueError):
            self.build(inputs=replace(self.inputs, heap_bytes=0))

    def test_mapping_padding_is_not_reply_storage(self):
        # Fits mapped 0x10000 bytes, but crosses logical 0xc441 extent.
        with self.assertRaises(ValueError):
            table_offset(self.inputs, self.ipc, self.ipc.iova + 0xC000)


if __name__ == "__main__":
    unittest.main()
