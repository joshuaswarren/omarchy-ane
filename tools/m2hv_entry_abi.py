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
ZERO_WORD_OFF = 0x300
HANDOFF_SIZE = ZERO_WORD_OFF + 8
BOOTARGS_REGION = 0x4000


def build_handoff(base):
    """Handoff struct the 27.0 entry accepts.

    base is the guest physical address the struct will occupy. The entry
    checks, in order: the magic at +0, then a versioned sub-struct at +0x1a0.
    version >= 7 makes it copy the sub-struct fields out to globals; bit 6 of
    the field at sub+0xa8 lands in a global byte that must be non-zero, or the
    entry executes `udf #0` and the CPU dies. The field at sub+0x8 is copied to
    a global the entry dereferences as a table count, so it must point at a
    readable zero word (count 0 reads as an empty table, and the lookup
    returns not-found). Every other field is zero: the consumers null-check or
    range-check before they dereference, and zero fails those checks safely.
    """
    b = bytearray(HANDOFF_SIZE)
    struct.pack_into('<Q', b, 0, HANDOFF_MAGIC)
    struct.pack_into('<I', b, SUB_OFF, HANDOFF_VERSION)
    struct.pack_into('<Q', b, COUNT_PTR_OFF, base + ZERO_WORD_OFF)
    struct.pack_into('<Q', b, BIT6_OFF, 1 << 6)
    return bytes(b)


def arm(hv):
    bootargs = hv.guest_base + hv.bootargs_off
    handoff = bootargs + HANDOFF_OFF
    hv.iface.writemem(handoff, build_handoff(handoff))

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
    base = 0x100000000
    blob = build_handoff(base)
    assert struct.unpack_from('<Q', blob, 0)[0] == HANDOFF_MAGIC
    assert struct.unpack_from('<I', blob, SUB_OFF)[0] == HANDOFF_VERSION
    assert struct.unpack_from('<Q', blob, COUNT_PTR_OFF)[0] == base + ZERO_WORD_OFF
    assert (struct.unpack_from('<Q', blob, BIT6_OFF)[0] >> 6) & 1 == 1
    assert struct.unpack_from('<I', blob, ZERO_WORD_OFF)[0] == 0
    assert HANDOFF_OFF + HANDOFF_SIZE <= BOOTARGS_REGION
    print("entry abi self-check ok")
