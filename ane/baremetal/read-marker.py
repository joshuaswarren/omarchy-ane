#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# read-marker.py — read the BMN-03 result marker from jwm1's reserved DRAM
# hole after the post-trial Linux boot.
#
#   sudo python3 read-marker.py
#
# Reads ONLY the isp-heap reserved region (0xbd2bc0000) via /dev/mem — never
# the PMGR PS registers: a kernel-mapped devmem read of 0x23b7xxxx crashed
# jwm1 hard on 2026-11-27. The PS values the payload saw are already inside
# the marker; that is the whole point of the mailbox.

import mmap
import struct
import sys

MARKER = 0xBD2BC0000
SIZE = 0x100

PS_NAMES = [
    "ane_sys",
    "ane_sys_cpu",
    "ane_base",
    "ane_set1",
    "ane_set2",
    "ane_set3",
    "ane_set4",
    "ane_set5",
]

with open("/dev/mem", "rb") as f:
    m = mmap.mmap(f.fileno(), SIZE, mmap.MAP_SHARED, mmap.PROT_READ, offset=MARKER)
    d = m[:SIZE]

magic, ver = struct.unpack_from("<II", d, 0)
if magic != 0x424D4E33:
    print(f"no BMN3 marker at {MARKER:#x} (first words: {d[:16].hex()})")
    sys.exit(1)

(cntpct,) = struct.unpack_from("<Q", d, 0x08)
stage, ok_mask = struct.unpack_from("<II", d, 0x14)
seal = struct.unpack_from("<I", d, 0x54)[0]

print(f"marker: v{ver} stage={stage} cntpct={cntpct} seal={seal:#x}")
print(f"ps_ok mask: {ok_mask:#04x}")
for i, name in enumerate(PS_NAMES):
    (v,) = struct.unpack_from("<I", d, 0x20 + i * 4)
    ok = "ACTIVE" if ok_mask & (1 << i) else "  off "
    print(f"  {ok} {name:12s} ps={v:#010x}  actual={v >> 4 & 0xF:#x} target={v & 0xF:#x}")

if stage >= 3:
    for i in range(4):
        (v,) = struct.unpack_from("<I", d, 0x40 + i * 4)
        print(f"scratch[{i}] = {v:#010x}")
if stage >= 4:
    (v,) = struct.unpack_from("<I", d, 0x50)
    print(f"scratch write probe readback = {v:#010x} (wrote 0xa5b3c7d1)")
