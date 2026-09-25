# m1n1 hv module (run_guest.py -m, AFTER m2hv_guest_debug): stage a macOS
# restore ramdisk as the guest root so XNU has a userspace AND no root-mount
# panic. Uploads <rdimg> in 64 MB chunks to the first 1 GB-aligned address
# above the m1n1 heap top inside mapped guest RAM, then rewrites the guest
# ADT: /chosen/memory-map RAMDisk = (addr, size), /chosen RDRamDisk = 0
# (8-byte zero) and RDSize = size. Boot with rd=md0 -rootdmg-ramdisk
# rp=file:///ane-root.dmg so XNU creates md0 from the iBoot RAMDisk entry
# and imageboot reads the inner dmg off the ramdisk root
# (xnu-8796 bsd/kern/{bsd_init.c,imageboot.c}, iokit/bsddev/IOKitBSDInit.cpp).
# Env: M2HV_RDIMG (dmg path), M2HV_RDBASE (optional override address).
import os

RDIMG = os.environ["M2HV_RDIMG"]
CHUNK = 64 << 20


def align(x, a=0x40000000):
    return (x + a - 1) & ~(a - 1)


size = os.path.getsize(RDIMG)
base = int(os.environ.get("M2HV_RDBASE", "0"), 0) or align(u.heap_top)
top = u.ba.phys_base + u.ba.mem_size
assert base + size <= top, f"no room: base {base:#x} + {size:#x} > top {top:#x}"
print(f"ramdisk: {RDIMG} {size:#x} bytes -> {base:#x}")

from construct import Array, Hex, Int64ul

sent = 0
with open(RDIMG, "rb") as f:
    while True:
        buf = f.read(CHUNK)
        if not buf:
            break
        p.writemem(base + sent, buf)
        sent += len(buf)
        print(f"ramdisk: {sent:#x}/{size:#x}")


mm = hv.adt["/chosen"]["memory-map"]
mm._types["RAMDisk"] = (Array(2, Hex(Int64ul)), False)
mm.RAMDisk = [base, size]
ch = hv.adt["/chosen"]
ch._types["RDRamDisk"] = (Int64ul, False)
ch._types["RDSize"] = (Int64ul, False)
ch.RDRamDisk = 0
ch.RDSize = size
print("guest ADT: /chosen/memory-map RAMDisk set, RDRamDisk=0, RDSize set")
