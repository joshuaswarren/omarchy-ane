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
#
# Verdicts: each domain passes when its readback word's ACTUAL field
# (bits 7:4) reads 0xf, cross-checked against the payload's own ps_ok bit;
# the scratch probe passes when the stage-4 readback equals the word the
# payload wrote (0xa5b3c7d1).

import mmap
import struct
import sys

MARKER = 0xBD2BC0000
SIZE = 0x100

MAGIC = 0x424D4E33          # "BMN3"
SEAL = 0xDEADBEE5
SCRATCH_PROBE = 0xA5B3C7D1  # word the payload writes to ANE_SCRATCH0

PS_ACTUAL_MASK = 0xF0       # register bits 7:4
PS_ACTUAL_ACTIVE = 0xF0     # ACTUAL == 0xf means the domain reached active

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


def parse_marker(d):
    """Parse SIZE marker bytes into a result dict, or None if no marker."""
    magic, ver = struct.unpack_from("<II", d, 0)
    if magic != MAGIC:
        return None
    (cntpct,) = struct.unpack_from("<Q", d, 0x08)
    stage, ok_mask = struct.unpack_from("<II", d, 0x14)
    ps = []
    for i, name in enumerate(PS_NAMES):
        (v,) = struct.unpack_from("<I", d, 0x20 + i * 4)
        ps.append(
            {
                "name": name,
                "value": v,
                "actual": (v & PS_ACTUAL_MASK) >> 4,
                "target": v & 0xF,
                "ok": (v & PS_ACTUAL_MASK) == PS_ACTUAL_ACTIVE,
                "mask_bit": bool(ok_mask & (1 << i)),
            }
        )
    r = {
        "ver": ver,
        "cntpct": cntpct,
        "stage": stage,
        "ok_mask": ok_mask,
        "ps": ps,
        "seal": struct.unpack_from("<I", d, 0x54)[0],
        "scratch": [struct.unpack_from("<I", d, 0x40 + i * 4)[0] for i in range(4)],
        "probe": struct.unpack_from("<I", d, 0x50)[0],
    }
    # the mask the payload recorded must agree with the words it stored;
    # a disagreement means the marker itself is suspect
    r["mask_consistent"] = all(p["ok"] == p["mask_bit"] for p in ps)
    return r


def scratch_verdict(r):
    """One-line verdict for the stage-4 scratch write/readback probe."""
    if r["stage"] < 4:
        return f"scratch-readback: NOT REACHED (stage={r['stage']}, needed 4)"
    if r["seal"] != SEAL:
        return f"scratch-readback: FAIL (seal={r['seal']:#010x}, expected {SEAL:#010x})"
    if r["probe"] == SCRATCH_PROBE:
        return f"scratch-readback: PASS (wrote {SCRATCH_PROBE:#010x}, read {r['probe']:#010x})"
    return f"scratch-readback: FAIL (wrote {SCRATCH_PROBE:#010x}, read {r['probe']:#010x})"


def print_report(r):
    print(f"marker: v{r['ver']} stage={r['stage']} cntpct={r['cntpct']} seal={r['seal']:#x}")
    print(f"ps_ok mask: {r['ok_mask']:#04x}" + ("" if r["mask_consistent"] else "  <-- disagrees with readback words"))
    for p in r["ps"]:
        v = p["value"]
        print(f"  {'PASS' if p['ok'] else 'FAIL'} {p['name']:12s} ps={v:#010x}  actual={p['actual']:#x} target={p['target']:#x}")
    if r["stage"] >= 3:
        for i, v in enumerate(r["scratch"]):
            print(f"scratch[{i}] = {v:#010x}")
    print(scratch_verdict(r))


def main():
    with open("/dev/mem", "rb") as f:
        m = mmap.mmap(f.fileno(), SIZE, mmap.MAP_SHARED, mmap.PROT_READ, offset=MARKER)
        d = m[:SIZE]
    r = parse_marker(d)
    if r is None:
        print(f"no BMN3 marker at {MARKER:#x} (first words: {d[:16].hex()})")
        sys.exit(1)
    print_report(r)


if __name__ == "__main__":
    main()
