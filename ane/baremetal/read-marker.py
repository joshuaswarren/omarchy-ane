#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""read-marker.py — read the BMN-03 result marker or parse a BMN-05 survey."""
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
# Q-86 survey mode: `read-marker.py --survey FILE` parses a BMN-05 MMIO
# survey (one `DOMAIN 0xADDR mapped|unmapped` line per probe, `#` comments
# allowed) into one pass/fail line per hardware domain and exits 0 only
# when every domain the survey must cover is mapped.

import argparse
import mmap
import struct
import sys

MARKER = 0xBD2BC0000
SIZE = 0x100

# The five hardware domains a BMN-05 survey must cover, with the t8103
# bases the kex0 EFI survey walks (kex0-efi.zig). The mailbox sits inside
# the ANE engine window on H17 (engine + 0x1400000), per the t6050 table.
DOMAINS = {
    "WDT": 0x23D2B0000,
    "PMGR": 0x23B700000,
    "ANE-PS": 0x23B7C000,
    "ANE-MMIO": 0x26B800000,
    "MAILBOX": 0x26B800000 + 0x1400000,
}


def parse_survey(text: str) -> dict[str, dict]:
    """Parse survey lines into {DOMAIN: {"mapped": bool, "addr": str|None}}.

    A line names one probe: `DOMAIN 0xADDR mapped` or `... unmapped`
    (whitespace and case tolerant, `#` starts a comment). Unknown domain
    names are kept under their own name; a domain the survey must cover
    but never names reads as unmapped with no address.
    """
    found: dict[str, dict] = {}
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[0].upper()
        verdict = parts[-1].lower()
        addr = parts[1] if len(parts) > 2 and parts[1].lower().startswith("0x") else None
        mapped = "mapped" in verdict and "unmapped" not in verdict
        found[name] = {"mapped": mapped, "addr": addr}
    for domain in DOMAINS:
        if domain not in found:
            found[domain] = {"mapped": False, "addr": None}
    return found


def report_survey(found: dict[str, dict]) -> tuple[list[str], bool]:
    """One pass/fail line per hardware domain. True only when every
    domain in DOMAINS is mapped; an unknown-name probe never satisfies a
    known domain."""
    lines = []
    all_ok = True
    for domain, base in DOMAINS.items():
        entry = found.get(domain, {"mapped": False, "addr": None})
        ok = bool(entry["mapped"])
        all_ok &= ok
        addr = entry.get("addr") or f"{base:#x}"
        lines.append(f"{'PASS' if ok else 'FAIL'} {domain:9s} {addr} {'mapped' if ok else 'unmapped'}")
    for name, entry in sorted(found.items()):
        if name not in DOMAINS:
            lines.append(f"{'PASS' if entry['mapped'] else 'FAIL'} {name:9s} "
                         f"{entry.get('addr') or '?'} {'mapped' if entry['mapped'] else 'unmapped'}")
    return lines, all_ok


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


def read_marker() -> int:
    with open("/dev/mem", "rb") as f:
        m = mmap.mmap(f.fileno(), SIZE, mmap.MAP_SHARED, mmap.PROT_READ, offset=MARKER)
        d = m[:SIZE]

    magic, ver = struct.unpack_from("<II", d, 0)
    if magic != 0x424D4E33:
        print(f"no BMN3 marker at {MARKER:#x} (first words: {d[:16].hex()})")
        return 1

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
    return 0


def read_survey(path: str) -> int:
    with open(path, encoding="utf-8") as f:
        found = parse_survey(f.read())
    lines, all_ok = report_survey(found)
    for line in lines:
        print(line)
    print("SURVEY:", "ALL DOMAINS MAPPED" if all_ok else "GAPS PRESENT")
    return 0 if all_ok else 1


def main(argv: list[str] | None = None) -> int:
    top = argparse.ArgumentParser(description="read the BMN-03 marker or parse a BMN-05 survey")
    top.add_argument("--survey", metavar="FILE",
                     help="parse a BMN-05 MMIO survey instead of reading the DRAM marker")
    args = top.parse_args(argv)
    if args.survey:
        return read_survey(args.survey)
    return read_marker()


if __name__ == "__main__":
    sys.exit(main())
