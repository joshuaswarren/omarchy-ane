#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import unittest
from pathlib import Path
import subprocess

from table_discovery_verdict import judge


class VerdictTests(unittest.TestCase):
    def setUp(self):
        self.prep = ("H17 TABLE_PREPARE dm=0x10000380000 ipc=0x100003c0000 "
                     "ipc_bytes=0xc441 heap=0x100003d0000 descriptor_bytes=0x180 sha256="
                     "d02b270910104f38d5e543aacb9b32a09135903b899da97f8472c159c36098ee")
        self.backings = [
            "H17 TABLE_BACKING index=0 dma=0x3ffffbc0000 alias=0x10000380000 mapped=0x40000",
            "H17 TABLE_BACKING index=1 dma=0x3ffffbb0000 alias=0x100003c0000 mapped=0x10000",
            "H17 TABLE_BACKING index=2 dma=0x3fffe000000 alias=0x100003d0000 mapped=0xf00000",
        ]
        self.copy = "H17 TABLE_COPY address=0x100003c0000 bytes=0x800 count=8 sha256=" + "b" * 64
        self.stop = "H17 TABLE_STOP before SCRATCH3 registration; buffers retained; no doorbell, IRQ ack or task"
        self.result = "ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-table-discovery"
        self.channels = [f"H17 CHANNEL index={i} name_hex=" + "00" * 32 +
                         " type=0x00000000 source=0x00000001 slots=0x00000002 buffer=0xffffffffffffffff"
                         for i in range(8)]
        self.lines = [*self.backings, self.prep, self.copy, *self.channels, self.stop, self.result]

    def check(self, lines):
        return judge("\n".join("[ 12.345] ane_h16 400000000.ane: " + line for line in lines))

    def test_exact_capture(self):
        result = self.check(self.lines)
        self.assertEqual(result["rung"], "PASS")
        self.assertEqual(result["table_sha256"], "b" * 64)

    def test_runner_has_no_default_action(self):
        runner = Path(__file__).with_name("run-table-discovery.sh")
        for arguments in ([], ["--run", "extra"]):
            with self.subTest(arguments=arguments):
                result = subprocess.run(["bash", str(runner), *arguments],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2)
                self.assertIn("usage", result.stderr)

    def test_missing_duplicate_or_malformed_marker(self):
        for line in self.lines:
            for replacement in ([], [line, line], [line + " malformed"]):
                with self.subTest(line=line, replacement=replacement):
                    mutated = self.lines.copy()
                    index = mutated.index(line)
                    mutated[index:index + 1] = replacement
                    self.assertEqual(self.check(mutated)["rung"], "FAIL")

    def test_wake_pass_cannot_substitute(self):
        self.assertEqual(self.check(self.lines[:-1] + [self.result.replace("boot-table-discovery", "boot-wake")])["rung"], "FAIL")

    def test_copy_bounds_sizes_and_preparation(self):
        for before, after in (
            ("address=0x100003c0000", "address=0x100003cbc42"),  # one byte overrun
            ("address=0x100003c0000", "address=0x100003cc000"),  # mapping padding
            ("address=0x100003c0000", "address=0x10000000000"),  # firmware, not IPC
            ("bytes=0x800", "bytes=0x801"),
            ("count=8", "count=9"),
            ("dm=0x10000380000", "dm=0x100003c0000"),  # overlaps IPC
            ("heap=0x100003d0000", "heap=0x50000000000"),
            ("ipc=0x100003c0000", "ipc=0x100003c0001"),
        ):
            with self.subTest(before=before, after=after):
                self.assertEqual(judge("\n".join(self.lines).replace(before, after))["rung"], "FAIL")

    def test_channel_order_and_failures(self):
        lines = self.lines.copy()
        lines[5], lines[6] = lines[6], lines[5]
        self.assertEqual(self.check(lines)["rung"], "FAIL")
        for verdict in ("FAIL", "REFUSED"):
            self.assertEqual(self.check(self.lines + ["verdict=" + verdict])["rung"], "FAIL")

    def test_descriptor_digest_must_match_all_bytes(self):
        for digest in ("a" * 64, "0" * 64,
                       "d02b270910104f38d5e543aacb9b32a09135903b899da97f8472c159c36098ef"):
            with self.subTest(digest=digest):
                lines = self.lines.copy()
                lines[3] = lines[3].split("sha256=")[0] + "sha256=" + digest
                self.assertEqual(self.check(lines)["rung"], "FAIL")

    def test_protocol_order(self):
        for left, right in ((0, 1), (2, 3), (3, 4), (4, 5), (12, 13), (13, 14), (0, 14)):
            with self.subTest(left=left, right=right):
                lines = self.lines.copy()
                lines[left], lines[right] = lines[right], lines[left]
                self.assertEqual(self.check(lines)["rung"], "FAIL")

    def test_incomplete_overlapping_or_wrong_backing(self):
        for before, after in (
            ("mapped=0x40000", "mapped=0x3c000"),
            ("dma=0x3ffffbc0000", "dma=0x10000380000"),
            ("dma=0x3ffffbc0000", "dma=0x3ffffbb0000"),
            ("alias=0x100003c0000", "alias=0x100003c4000"),
            ("dma=0x3ffffbb0000", "dma=0x50000000000"),
        ):
            self.assertEqual(judge("\n".join(self.lines).replace(before, after))["rung"], "FAIL")


if __name__ == "__main__":
    unittest.main()
