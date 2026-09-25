#!/usr/bin/env python3
# m1n1 hv module (run_guest.py -m): the macOS 26/27 kernel entry contract.
#
# The 27.0 kernel entry (LC_UNIXTHREAD pc, the __TEXT_BOOT_EXEC page start)
# dispatches on x0. It does not read a boot_args field there:
#   x0 == 0      cold boot. x1 = boot_args pointer, x2 = handoff struct pointer.
#   x0 == 4 or 5 SPTM entry paths. The M2 kernelcache ships no SPTM binary.
#   anything else  secondary CPU: looks up per-CPU data by MPIDR and spins
#                until the boot CPU publishes it.
# m1n1's HV.start passes x0 = the boot_args pointer, so the boot CPU takes the
# secondary path and spins on the empty cpu-data table (the `cbz x21` at
# entry+0x10c, x21 = [table+8]). Moving the boot_args pointer to x1 changes
# nothing, because the dispatch reads x0 and x0 is still non-zero.
#
# Load this only for a 26/27 kernelcache. A 13.5 kernelcache wants the stock
# x0 = boot_args contract and must not load this module.

import struct

HANDOFF_MAGIC = 0xd00f000000000000
HANDOFF_VERSION = 7
# Placed inside the 0x4000-byte bootargs region, past the boot_args struct
# (BootArgs_r3 ends near 0x480), so it is mapped and the kernel never reads it
# as boot_args.
HANDOFF_OFF = 0x1000
SUB_OFF = 0x1a0
COUNT_PTR_OFF = 0x1a8   # sub+0x8, copied to a global dereferenced as a count
BIT6_OFF = 0x248        # sub+0xa8, bit 6 copied to the global byte the entry checks
COUNT_WORD_OFF = 0x300
ARRAY_PTR_OFF = 0x1b0     # sub+0x10, copied to the region-array global
REGION_OFF = 0x308
REGION_STRIDE = 0x18
PAGE_SHIFT = 14
HANDOFF_SIZE = REGION_OFF + REGION_STRIDE
BOOTARGS_REGION = 0x4000


def build_handoff(base, phys_base, virt_base, mem_size):
    """Handoff struct the 27.0 entry accepts, plus the one derivable table.

    base is the guest physical address the struct occupies. The entry checks
    the magic at +0 and a versioned sub-struct at +0x1a0. version >= 7 copies
    the sub-struct fields to globals; bit 6 of sub+0xa8 must be set or the
    entry executes `udf #0`. sub+0x8 and sub+0x10 become the count pointer and
    the array pointer of the physical-to-virtual region table (globals 0x6d0
    and 0x6d8), which the kernel dereferences, so both must be readable.

    The region table is the address translation behind 159 call sites. Its
    entry is 0x18 bytes: physical base, virtual base, size in 16 KB pages.
    One entry covering RAM is derivable from the boot_args m1n1 already
    builds, and it translates every RAM address. The pointers are physical:
    the 27.0 kernel never writes SCTLR_EL1 and m1n1 leaves the guest MMU off,
    so early boot dereferences them as physical addresses.
    """
    pages = mem_size >> PAGE_SHIFT
    b = bytearray(HANDOFF_SIZE)
    struct.pack_into('<Q', b, 0, HANDOFF_MAGIC)
    struct.pack_into('<I', b, SUB_OFF, HANDOFF_VERSION)
    struct.pack_into('<Q', b, BIT6_OFF, 1 << 6)
    struct.pack_into('<Q', b, COUNT_PTR_OFF, base + COUNT_WORD_OFF)
    struct.pack_into('<Q', b, ARRAY_PTR_OFF, base + REGION_OFF)
    struct.pack_into('<I', b, COUNT_WORD_OFF, 1 if pages else 0)
    struct.pack_into('<Q', b, REGION_OFF, phys_base)
    struct.pack_into('<Q', b, REGION_OFF + 8, virt_base)
    struct.pack_into('<I', b, REGION_OFF + 0x10, pages)
    return bytes(b)


def arm(hv):
    bootargs = hv.guest_base + hv.bootargs_off
    handoff = bootargs + HANDOFF_OFF
    ba = hv.tba
    hv.iface.writemem(handoff, build_handoff(
        handoff, ba.phys_base, ba.virt_base, ba.mem_size))

    real_start = hv.p.hv_start

    def hv_start(entry, *args):
        # HV.start calls hv_start(entry, bootargs): one register argument, which
        # the proxy places in x0. The 26/27 boot CPU wants x0 = 0, x1 = bootargs,
        # x2 = handoff. Any other call shape passes through unchanged.
        if len(args) == 1:
            args = (0, args[0], handoff)
        return real_start(entry, *args)

    hv.p.hv_start = hv_start

    real_secondary = hv.p.hv_start_secondary

    def hv_start_secondary(cpu, entry, *args):
        # A secondary must enter with x0 != 0, or it takes the cold-boot path
        # and re-initialises the kernel. x0 = 1 selects the cpu-data lookup,
        # which waits until the boot CPU publishes the entry.
        if not args:
            args = (1,)
        return real_secondary(cpu, entry, *args)

    hv.p.hv_start_secondary = hv_start_secondary
    print(f"entry abi: x0=0 x1=bootargs {bootargs:#x} x2=handoff {handoff:#x} "
          f"(magic {HANDOFF_MAGIC:#x}, version {HANDOFF_VERSION}, bit6 set)")


if __name__ == "<hv_script>":
    arm(hv)
elif __name__ == "__main__":
    base, phys, virt, mem = 0x100000000, 0x800000000, 0xfffffe0010000000, 0x40000000
    blob = build_handoff(base, phys, virt, mem)
    assert struct.unpack_from('<Q', blob, 0)[0] == HANDOFF_MAGIC
    assert struct.unpack_from('<I', blob, SUB_OFF)[0] == HANDOFF_VERSION
    assert (struct.unpack_from('<Q', blob, BIT6_OFF)[0] >> 6) & 1 == 1
    assert struct.unpack_from('<Q', blob, COUNT_PTR_OFF)[0] == base + COUNT_WORD_OFF
    assert struct.unpack_from('<Q', blob, ARRAY_PTR_OFF)[0] == base + REGION_OFF
    assert struct.unpack_from('<I', blob, COUNT_WORD_OFF)[0] == 1
    assert struct.unpack_from('<Q', blob, REGION_OFF)[0] == phys
    assert struct.unpack_from('<Q', blob, REGION_OFF + 8)[0] == virt
    assert struct.unpack_from('<I', blob, REGION_OFF + 0x10)[0] == mem >> PAGE_SHIFT
    assert HANDOFF_OFF + HANDOFF_SIZE <= BOOTARGS_REGION
    assert build_handoff(base, phys, virt, 0)[COUNT_WORD_OFF:COUNT_WORD_OFF + 4] == b'\x00' * 4
    print("entry abi self-check ok")
