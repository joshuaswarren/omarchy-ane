#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Judge a captured first-envelope H17 registration log; never opens a device."""
import json
from pathlib import Path
import re
import sys


CHANNELS = (
    ("TERMINAL", 2, 0, 0x200, 0x100003C0800),
    ("IO", 0, 1, 0x10, 0x100003C8800),
    ("DEBUG", 0, 2, 0x08, 0x100003C8C00),
    ("BUF_H2T", 0, 3, 0x40, 0x100003C8E00),
    ("BUF_T2H", 1, 4, 0x40, 0x100003C9E00),
    ("SHAREDMALLOC", 1, 5, 0x08, 0x100003CAE00),
    ("IO_T2H", 1, 6, 0x40, 0x100003CB000),
    ("DATA_CHAIN_H2T", 0, 7, 0x10, 0x100003CC000),
)
INITIALIZED = ((1, 0x8800, 0x400, 0x10),
               (2, 0x8C00, 0x200, 0x08),
               (3, 0x8E00, 0x1000, 0x40),
               (7, 0xC000, 0x400, 0x10))


def judge(text):
    lines = text.splitlines()
    result = {"rung": "FAIL", "reason": "missing-or-invalid-markers"}

    def one(marker, pattern):
        found = [line for line in lines if marker in line]
        if len(found) != 1:
            raise ValueError(marker + ": expected exactly one line")
        match = re.search(pattern + r"$", found[0])
        if match is None:
            raise ValueError(marker + ": malformed line")
        return match

    try:
        if re.search(r"verdict=(FAIL|REFUSED)", text):
            raise ValueError("module reported failure/refusal")
        one("H17 ALIAS_RESERVATION ",
            r"H17 ALIAS_RESERVATION start=0x10000380000 bytes=0xf50000 type=IOMMU_RESV_RESERVED")
        one("H17 TABLE_PREPARE ",
            r"H17 TABLE_PREPARE dm=0x10000380000 ipc=0x100003c0000 ipc_bytes=0xc441 "
            r"heap=0x100003d0000 descriptor_bytes=0x180 "
            r"sha256=d02b270910104f38d5e543aacb9b32a09135903b899da97f8472c159c36098ee")
        one("H17 TABLE_COPY ",
            r"H17 TABLE_COPY address=0x100003c0000 bytes=0x800 count=8 "
            r"sha256=05e5c12d7e24fc033f6eb0dd246ae4753004375426b0d07c8c211fab575e08ae")

        channels = [line for line in lines if "H17 CHANNEL index=" in line]
        if len(channels) != len(CHANNELS):
            raise ValueError("expected exactly eight channel records")
        for index, (line, (name, channel_type, source, slots, buffer)) in enumerate(
                zip(channels, CHANNELS)):
            name_hex = name.encode("ascii").ljust(32, b"\0").hex()
            expected = (rf"H17 CHANNEL index={index} name_hex={name_hex} "
                        rf"type=0x{channel_type:08x} source=0x{source:08x} "
                        rf"slots=0x{slots:08x} buffer=0x{buffer:016x}")
            if not re.search(expected + r"$", line):
                raise ValueError(f"channel record {index} differs from pinned first-run envelope")

        init_lines = [line for line in lines if "H17 CHANNEL_INIT " in line]
        if len(init_lines) != len(INITIALIZED):
            raise ValueError("expected four type-0 initialization ranges")
        for index, (line, values) in enumerate(zip(init_lines, INITIALIZED)):
            expected = (rf"H17 CHANNEL_INIT index={values[0]} offset=0x{values[1]:x} "
                        rf"bytes=0x{values[2]:x} entries=0x{values[3]:x}")
            if not re.search(expected + r"$", line):
                raise ValueError(f"initialized range {index} differs from validated IPC envelope")

        one("H17 CHANNEL_REGISTER request=",
            r"H17 CHANNEL_REGISTER request=0x8042006 scratch3=0x[0-9a-f]+")
        one("H17 CHANNEL_REGISTER_ACK ",
            r"H17 CHANNEL_REGISTER_ACK value=0")
        one("H17 CHANNEL_REGISTER_STOP ",
            r"H17 CHANNEL_REGISTER_STOP after ack; no doorbell, IRQ ack or task")
        result_line = one("ane_h16 RESULT ",
                          r"ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-channel-registration")
        ordered_markers = ["H17 TABLE_COPY ",
                           *[f"H17 CHANNEL index={i} " for i in range(8)],
                           *[f"H17 CHANNEL_INIT index={v[0]} " for v in INITIALIZED],
                           "H17 CHANNEL_REGISTER request=", "H17 CHANNEL_REGISTER_ACK ",
                           "H17 CHANNEL_REGISTER_STOP ", "ane_h16 RESULT "]
        positions = [next(i for i, line in enumerate(lines) if marker in line)
                     for marker in ordered_markers]
        if positions != sorted(positions):
            raise ValueError("registration markers are not in protocol order")
        result.update(rung="PASS", reason="channel-registration-acknowledged",
                      channel_count=8, initialized_channels=[v[0] for v in INITIALIZED],
                      table_sha256="05e5c12d7e24fc033f6eb0dd246ae4753004375426b0d07c8c211fab575e08ae")
    except ValueError as exc:
        result["reason"] = str(exc)
    return result


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: channel_registration_verdict.py dmesg-rung.log")
    verdict = judge(Path(sys.argv[1]).read_text())
    print(json.dumps(verdict, sort_keys=True))
    raise SystemExit(0 if verdict["rung"] == "PASS" else 1)
