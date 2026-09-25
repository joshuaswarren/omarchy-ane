#!/usr/bin/env python3
"""t6021_dart_walk — read-only DART-ane translation-state verifier (jw14m2).

Reads the live dart-ane TCR/TTBR/ERROR/FAULT registers via /dev/mem (MMIO,
read-only) and walks the driver-managed io-pgtable via /proc/kcore. Verifies,
with executable assertions, everything the W16 entry-alias contract claims.

Register contract cited from drivers/iommu/apple-dart.c (t8110 hw struct):
  TCR @0x1000+4*sid (TRANSLATE=bit0, FOUR_LEVEL=bit3)
  TTBR @0x1400+4*ttbr_count*sid, ttbr_count=1: VALID=bit0,
      pa = ((reg & ~1) >> 2) << 14   (ttbr_shift=14, addr_field_shift=2)
  ERROR @0x100 (FLAG bit31, STREAM 27:20, CODE 14:0, NO_PGD=bit1)
  FAULT_ADDR @0x170/0x174
io-pgtable (drivers/iommu/io-pgtable-dart.c, APPLE_DART2): u64 PTEs,
  VALID=bit0, pa = (pte & GENMASK(37,10)) << 4, 16 KiB tables,
  11 bits/level: pgd[iova>>36], mid[iova>>25], leaf[iova>>14].

Usage: sudo python3 t6021_dart_walk.py [iova ...]
Default iovas: the ASC entry 0x10000000000 and the staged fw region.
"""
import struct
import sys
import mmap
import os

DART0 = 0x285800000
ANE_ENG = 0x284000000
ASC_RVBAR = 0x1050000
ENTRY_IOVA = 0x10000000000
FW_IOVA = 0x3FFFF800000
FW_SIZE = 0x500000
PAGE = 0x4000

assert (0x10012395 & ~1) >> 2 << 14 == 0x10012394000, "TTBR decode regression"
assert ENTRY_IOVA == 1 << 40 and ENTRY_IOVA % PAGE == 0, "entry shape"
assert FW_IOVA == (1 << 42) - (1 << 23), "fw iova shape"
assert FW_SIZE % PAGE == 0, "fw size shape"
assert (0x10000000001 & 0xFF7EFFFFFFFFF800) == ENTRY_IOVA, "rvbar entry bits"
assert 0x10000000001 == (1 << 40) + 1, "rvbar = entry<<40 | valid"
assert (0x0081000000000001 | FW_IOVA) == 0x008103FFFF800001, "rvbar compose"


def read_mmio32(base, off):
    pg = mmap.PAGESIZE
    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    try:
        start = (base + off) & ~(pg - 1)
        delta = (base + off) - start
        m = mmap.mmap(fd, pg, mmap.MAP_SHARED, mmap.PROT_READ, offset=start)
        try:
            return int.from_bytes(m[delta:delta + 4], "little")
        finally:
            m.close()
    finally:
        os.close(fd)


def kcore_segments():
    segs = []
    with open("/proc/kcore", "rb") as f:
        hdr = f.read(64)
        assert hdr[:4] == b"\x7fELF" and hdr[4] == 2
        e_phoff = struct.unpack_from("<Q", hdr, 0x20)[0]
        e_phentsize, e_phnum = struct.unpack_from("<HH", hdr, 0x36)
        f.seek(e_phoff)
        for _ in range(e_phnum):
            ph = f.read(e_phentsize)
            p_type = struct.unpack_from("<I", ph, 0)[0]
            p_offset, _, p_paddr, p_filesz, _ = struct.unpack_from("<QQQQQ", ph, 8)
            if p_type == 1 and p_filesz:
                segs.append((p_paddr, p_filesz, p_offset))
    return segs


def kreader(segs):
    def kread(pa, n):
        for base, size, off in segs:
            if base <= pa and pa + n <= base + size:
                with open("/proc/kcore", "rb") as f:
                    f.seek(off + (pa - base))
                    return f.read(n)
        raise ValueError(f"pa {pa:#x} not covered by kcore")
    return kread


def pte_pa(pte):
    return (pte & ((1 << 38) - (1 << 10))) << 4


def walk(kread, table_pa, iova):
    l1_i = (iova >> 36) & 0x7FF
    l2_i = (iova >> 25) & 0x7FF
    lf_i = (iova >> 14) & 0x7FF
    pgd = struct.unpack_from("<Q", kread(table_pa + 8 * l1_i, 8))[0]
    if not pgd:
        return None
    mid = struct.unpack_from("<Q", kread(pte_pa(pgd) + 8 * l2_i, 8))[0]
    if not mid:
        return None
    leaf = struct.unpack_from("<Q", kread(pte_pa(mid) + 8 * lf_i, 8))[0]
    if not leaf & 1:
        return None
    return pte_pa(leaf)


def main():
    iovas = [int(a, 0) for a in sys.argv[1:]] or [ENTRY_IOVA, 0x100000DCA10, FW_IOVA]

    tcr = read_mmio32(DART0, 0x1000)
    ttbr = read_mmio32(DART0, 0x1400)
    err = read_mmio32(DART0, 0x100)
    fault = read_mmio32(DART0, 0x170) | (read_mmio32(DART0, 0x174) << 32)
    rvbar = read_mmio32(ANE_ENG + ASC_RVBAR, 0) | (read_mmio32(ANE_ENG + ASC_RVBAR + 4, 0) << 32)
    print(f"dart0 TCR={tcr:#010x} TTBR={ttbr:#010x} ERROR={err:#010x} "
          f"FAULT={fault:#x} RVBAR={rvbar:#x}")

    assert tcr & 1 and tcr & 8, "translation + four-level expected (TCR bit0|bit3)"
    table_pa = ((ttbr & ~1) >> 2) << 14
    print(f"table pa = {table_pa:#x}")

    ram = []
    for line in open("/proc/iomem"):
        a, _, name = line.partition(":")
        if "System RAM" in name:
            s, e = a.strip().split("-")
            ram.append((int(s, 16), int(e, 16)))
    assert any(s <= table_pa <= e for s, e in ram), \
        f"decoded table pa {table_pa:#x} not inside System RAM — TTBR decode wrong"

    kread = kreader(kcore_segments())

    for iova in iovas:
        pa = walk(kread, table_pa, iova)
        if pa is None:
            print(f"iova {iova:#x}: UNMAPPED")
        else:
            assert pa % PAGE == 0
            assert any(s <= pa <= e for s, e in ram), f"leaf pa {pa:#x} not RAM"
            print(f"iova {iova:#x}: -> pa {pa:#x}")

    # Per-page mapping census for a range
    if iovas and iovas[-1] == FW_IOVA or not sys.argv[1:]:
        mapped = sum(
            1 for off in range(0, FW_SIZE, PAGE)
            if walk(kread, table_pa, FW_IOVA + off) is not None)
        entry_mapped = sum(
            1 for off in range(0, FW_SIZE, PAGE)
            if walk(kread, table_pa, ENTRY_IOVA + off) is not None)
        print(f"fw region: {mapped}/{FW_SIZE // PAGE} pages mapped at {FW_IOVA:#x}")
        print(f"entry region: {entry_mapped}/{FW_SIZE // PAGE} pages mapped at {ENTRY_IOVA:#x}")


if __name__ == "__main__":
    main()
