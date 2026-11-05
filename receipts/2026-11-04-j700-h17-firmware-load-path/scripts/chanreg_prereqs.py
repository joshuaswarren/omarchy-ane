#!/usr/bin/env python3
"""Offline evidence for the initialization descriptor; never accesses hardware.

Uses ane.py's retained-kernelcache path and VA mapping. Output includes source
identity so a disassembly is not silently attributed to another cache.
"""
import hashlib

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

import ane


WINDOWS = (
    (0x8F3E140, 0x8F3E3B0, "CPU release, FIRST wake poll, count read"),
    (0x8F3E754, 0x8F3E7EC, "count bound and IPC surface allocation"),
    (0x8F3ECB8, 0x8F3ED90, "Allocate(0x174), descriptor fields +0x00..+0x28"),
    (0x8F3EE24, 0x8F3EF30, "descriptor fields +0x30..+0x170"),
    (0x8F3F040, 0x8F3F0DC, "descriptor ANE address and SECOND wake request"),
    (0x8EF90AC, 0x8EF9190, "DeviceMemoryManager::Allocate rounds to 64 bytes"),
    (0x8F41314, 0x8F4141C, "aneAddressToHostAddress range selection"),
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
            print(f"{ins.address:#x}: {ins.bytes.hex(' ')}  "
                  f"{ins.mnemonic:8} {ins.op_str}")


if __name__ == "__main__":
    main()
