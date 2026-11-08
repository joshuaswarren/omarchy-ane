#!/usr/bin/env python3
"""Offline evidence for the initialization descriptor; never accesses hardware.

Uses ane.py's retained-kernelcache path and VA mapping. Output includes source
identity so a disassembly is not silently attributed to another cache.
"""
import hashlib

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

import ane


WINDOWS = (
    (0x8F55FA0, 0x8F55FE0, "FWIM allocation producer"),
    (0x8F56190, 0x8F56240, "DM arena allocation, zeroing, and offset allocator"),
    (0x8F641E4, 0x8F64224, "zeroed options template producer"),
    (0x8F53150, 0x8F53198, "options template conditional flags"),
    (0xB425F1C, 0xB425F44, "kalloc_data_external forwards flags low three bits"),
    (0x8F461D0, 0x8F461E4, "load-firmware argument provenance"),
    (0x8F462C8, 0x8F462DC, "firmware argument store"),
    (0x8F3E140, 0x8F3E3B0, "CPU release, FIRST wake poll, count read"),
    (0x8F3E754, 0x8F3E7EC, "count bound and IPC surface allocation"),
    (0x8F3E9D0, 0x8F3EA00, "SCRATCH3 optional heap and SCRATCH4 status"),
    (0x8F3EBB0, 0x8F3ECC4, "optional heap surface allocation and descriptor allocation"),
    (0x8F3ECB8, 0x8F3ED90, "Allocate(0x174), descriptor fields +0x00..+0x28"),
    (0x8F3EE24, 0x8F3EF30, "descriptor fields +0x30..+0x170"),
    (0x8F3F040, 0x8F3F0DC, "descriptor ANE address and SECOND wake request"),
    (0x8EF90AC, 0x8EF9190, "DeviceMemoryManager::Allocate rounds to 64 bytes"),
    (0x8F412F8, 0x8F416B4, "both address translators"),
    (0x8F4B6C8, 0x8F4B710, "surface metadata requested size and map outputs"),
    (0x8F4BE3C, 0x8F4BEE0, "surface metadata CPU pointer and output publication"),
)


def main():
    data = ane.data()
    digest = hashlib.sha256(data).hexdigest()
    if digest != "7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe":
        raise SystemExit("refusing unrecognized kernelcache: " + digest)
    print("source:", ane.KC)
    print("sha256:", digest)
    print("VA = 0xfffffe0007004000 + file_offset; offline only")
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    for start, end, label in WINDOWS:
        # Abbreviated receipt VAs retain the kernelcache's high 32 bits.
        lo = 0xFFFFFE0000000000 | start
        hi = 0xFFFFFE0000000000 | end
        print(f"\n== {label}: {lo:#x}..{hi:#x} ==")
        for ins in md.disasm(data[ane.off(lo):ane.off(hi)], lo):
            print((f"{ins.address:#x}: {ins.bytes.hex(' ')}  "
                   f"{ins.mnemonic:8} {ins.op_str}").rstrip())


if __name__ == "__main__":
    main()
