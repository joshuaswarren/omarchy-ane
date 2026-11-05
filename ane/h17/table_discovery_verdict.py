#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Judge a captured table-discovery log; never opens a device."""
import json
import hashlib
from pathlib import Path
import re
import sys

from chanreg_model import (
    VM_BASE, VM_END, FW_BYTES, DM_BYTES, PAGE, round_page,
    BootInputs, Region, build_descriptor,
)


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
        one("ane_h16 RESULT ", r"ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-table-discovery")
        address = r"(0x[0-9a-f]{1,16})"
        digest = r"([0-9a-f]{64})"
        prep = one("H17 TABLE_PREPARE ",
                   r"H17 TABLE_PREPARE dm=" + address + " ipc=" + address +
                   r" ipc_bytes=0xc441 heap=" + address +
                   r" descriptor_bytes=0x180 sha256=" + digest)
        dm, ipc, heap = (int(v, 16) for v in prep.groups()[:3])
        regions = [(VM_BASE, FW_BYTES), (dm, DM_BYTES),
                   (ipc, round_page(0xC441)), (heap, 0xF00000)]
        for base, size in regions:
            if base % PAGE or not VM_BASE <= base <= VM_END - size:
                raise ValueError("invalid preparation DMA extent")
        ordered = sorted(regions)
        if any(a + n > b for (a, n), (b, _) in zip(ordered, ordered[1:])):
            raise ValueError("overlapping preparation DMA extents")
        blob, _ = build_descriptor(
            BootInputs(8, 0xC440, 0xF00000),
            Region("firmware", VM_BASE, FW_BYTES, FW_BYTES, True, True),
            Region("ipc", ipc, 0xC441, round_page(0xC441), True, True),
            Region("dm", dm, DM_BYTES, DM_BYTES, True, True),
            Region("heap", heap, 0xF00000, 0xF00000, True, True),
        )
        if hashlib.sha256(blob).hexdigest() != prep[4]:
            raise ValueError("descriptor digest differs from independent offline model")
        copied = one("H17 TABLE_COPY ", r"H17 TABLE_COPY address=" + address +
                     r" bytes=0x800 count=8 sha256=" + digest)
        table_address = int(copied[1], 16)
        if not ipc <= table_address <= ipc + 0xC441 - 0x800:
            raise ValueError("copied table does not fit logical IPC extent")
        channels = [line for line in lines if "H17 CHANNEL " in line]
        if len(channels) != 8:
            raise ValueError("expected eight channel observations")
        for index, line in enumerate(channels):
            pattern = (rf"H17 CHANNEL index={index} name_hex=[0-9a-f]{{64}} "
                       r"type=0x[0-9a-f]{8} source=0x[0-9a-f]{8} "
                       r"slots=0x[0-9a-f]{8} buffer=0x[0-9a-f]{16}$")
            if not re.search(pattern, line):
                raise ValueError("malformed, duplicate, or out-of-order channel observation")
        one("H17 TABLE_STOP ", r"H17 TABLE_STOP before SCRATCH3 registration; buffers retained; no doorbell, IRQ ack or task")
        ordered_markers = [prep[0], copied[0], *[re.search(r"H17 CHANNEL .*$", line)[0]
                                               for line in channels],
                           "H17 TABLE_STOP ", "ane_h16 RESULT "]
        positions = [next(i for i, line in enumerate(lines) if marker in line)
                     for marker in ordered_markers]
        if positions != sorted(positions):
            raise ValueError("discovery markers are not in protocol order")
        result.update(rung="PASS", reason="owned-ipc-table-copied-registration-skipped",
                      table_address=hex(table_address), table_bytes=0x800,
                      channel_count=8, table_sha256=copied[2],
                      descriptor_sha256=prep[4])
    except ValueError as exc:
        result["reason"] = str(exc)
    return result


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: table_discovery_verdict.py dmesg-rung.log")
    verdict = judge(Path(sys.argv[1]).read_text())
    print(json.dumps(verdict, sort_keys=True))
    raise SystemExit(0 if verdict["rung"] == "PASS" else 1)
