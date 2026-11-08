#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline registration-log judge tests; never claim device qualification."""
import unittest
from pathlib import Path

from channel_registration_verdict import CHANNELS, INITIALIZED, judge


def capture():
    lines = [
        "H17 ALIAS_RESERVATION start=0x10000380000 bytes=0xf50000 type=IOMMU_RESV_RESERVED",
        "H17 TABLE_BACKING index=0 dma=0x3ffffbc0000 alias=0x10000380000 mapped=0x40000",
        "H17 TABLE_BACKING index=1 dma=0x3ffffbb0000 alias=0x100003c0000 mapped=0x10000",
        "H17 TABLE_BACKING index=2 dma=0x3fffe000000 alias=0x100003d0000 mapped=0xf00000",
        "H17 TABLE_PREPARE dm=0x10000380000 ipc=0x100003c0000 ipc_bytes=0xc441 heap=0x100003d0000 descriptor_bytes=0x180 sha256=d02b270910104f38d5e543aacb9b32a09135903b899da97f8472c159c36098ee",
        "H17 TABLE_COPY address=0x100003c0000 bytes=0x800 count=8 sha256=05e5c12d7e24fc033f6eb0dd246ae4753004375426b0d07c8c211fab575e08ae",
    ]
    for index, (name, typ, source, slots, buffer) in enumerate(CHANNELS):
        name_hex = name.encode().ljust(32, b"\0").hex()
        lines.append(f"H17 CHANNEL index={index} name_hex={name_hex} type=0x{typ:08x} source=0x{source:08x} slots=0x{slots:08x} buffer=0x{buffer:016x}")
    for index, offset, size, entries in INITIALIZED:
        lines.append(f"H17 CHANNEL_INIT index={index} offset=0x{offset:x} bytes=0x{size:x} entries=0x{entries:x}")
    lines.extend((
        "H17 CHANNEL_REGISTER request=0x8042006 scratch3=0x188002c",
        "H17 CHANNEL_REGISTER_ACK value=0",
        "H17 CHANNEL_REGISTER_STOP after ack; no doorbell, IRQ ack or task",
        "ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-channel-registration",
    ))
    return "\n".join(lines) + "\n"


class RegistrationVerdictTests(unittest.TestCase):
    def test_exact_capture(self):
        result = judge(capture())
        self.assertEqual(result["rung"], "PASS")
        self.assertEqual(result["channel_count"], 8)
        self.assertEqual(result["initialized_channels"], [1, 2, 3, 7])

    def test_missing_or_duplicate_ack_fails(self):
        text = capture().replace("H17 CHANNEL_REGISTER_ACK value=0\n", "")
        self.assertNotEqual(judge(text)["rung"], "PASS")
        self.assertNotEqual(judge(capture() + "H17 CHANNEL_REGISTER_ACK value=0\n")["rung"], "PASS")

    def test_wrong_reply_geometry_or_channel_record_fails(self):
        for old, new in (
            ("bytes=0x800 count=8", "bytes=0x7ff count=8"),
            ("slots=0x00000010 buffer=0x00000100003c8800",
             "slots=0x00000010 buffer=0x00000100003c8900"),
            ("offset=0x8e00 bytes=0x1000", "offset=0x8e00 bytes=0x0fff"),
        ):
            with self.subTest(old=old):
                self.assertNotEqual(judge(capture().replace(old, new))["rung"], "PASS")

    def test_failure_and_protocol_reordering_never_pass(self):
        failed = capture().replace("verdict=PASS", "verdict=FAIL")
        self.assertNotEqual(judge(failed)["rung"], "PASS")
        lines = capture().splitlines()
        request = next(i for i, line in enumerate(lines) if "CHANNEL_REGISTER request=" in line)
        ack = next(i for i, line in enumerate(lines) if "CHANNEL_REGISTER_ACK" in line)
        lines[request], lines[ack] = lines[ack], lines[request]
        self.assertNotEqual(judge("\n".join(lines))["rung"], "PASS")

    def test_runner_is_separate_default_off_one_shot(self):
        runner = Path(__file__).with_name("run-channel-registration.sh").read_text()
        self.assertIn("${1:-} != --run", runner)
        self.assertIn("h17-registration.pending", runner)
        self.assertIn("--kind registration --claim", runner)
        self.assertIn("chanreg=1 register_channels=1", runner)
        self.assertLess(runner.index("table_preflight.py"), runner.index("insmod "))
        self.assertNotIn("rmmod", runner)


if __name__ == "__main__":
    unittest.main()
