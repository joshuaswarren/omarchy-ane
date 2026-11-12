#!/usr/bin/env python3
"""Offline check for ane-h18-pack.py.

For the add program the two packed sections must equal the sections of the
hardware-validated image (the add fixture runs 100 of 100 calls on the Mac
mini; only sections 5 and 7 are compared - the rebuilt image's header and its
pad bytes differ from the original). For the matvec program no
hardware-validated reference exists; the check is that the procedure section
grows to two TD records and the TD-property record carries the declared TD
count.

usage: test_h18_pack.py
"""
import struct
import subprocess
import sys
import tempfile
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ADD_BASE = os.path.join(HERE, "fixtures", "add.prog.bin")
ADD_HWX = os.path.join(HERE, "fixtures", "add.hwx")
MV_BASE = os.path.join(HERE, "fixtures", "mv64.prog.bin")
MV_HWX = os.path.join(HERE, "fixtures", "mv64.hwx")


def secs(f):
    d = open(f, "rb").read()
    ns = struct.unpack_from("<I", d, 8)[0]
    out = {}
    for i in range(ns):
        sid, _, sz, off, _ = struct.unpack_from("<IIQQQ", d, 0x20 + i * 0x20)
        out[sid] = d[off:off + sz]
    return out


def run(base, hwx):
    out = tempfile.mktemp(suffix=".bin")
    r = subprocess.run([sys.executable, os.path.join(HERE, "ane-h18-pack.py"), base, hwx, out],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("pack failed: %s %s" % (r.stdout, r.stderr))
    print(r.stdout.strip())
    return secs(out), out


add, add_path = run(ADD_BASE, ADD_HWX)
ref = secs(ADD_BASE)
assert add[5] == ref[5], "add sec5 differs:\n%s\n%s" % (add[5].hex(), ref[5].hex())
assert add[7] == ref[7], "add sec7 differs:\n%s\n%s" % (add[7].hex(), ref[7].hex())
print("add: sec5 and sec7 equal the hardware-validated image's sections (image header and pad bytes are not compared)")

mv, mv_path = run(MV_BASE, MV_HWX)
count, tdcount = struct.unpack_from("<I", mv[5], 0)[0], struct.unpack_from("<I", mv[5], 0x28)[0]
off, size = struct.unpack_from("<QQ", mv[5], 8)
assert count == 1 and tdcount == 2, (count, tdcount)
assert size == 20 + 12 * 2 and len(mv[5]) == 8 + 16 + size, (hex(len(mv[5])), size)
r0 = struct.unpack_from("<iii", mv[5], 8 + 16 + 20)
r1 = struct.unpack_from("<iii", mv[5], 8 + 16 + 20 + 12)
assert r0 == (0, -1, 4) and r1 == (1, -1, 4), (r0, r1)
assert struct.unpack_from("<I", mv[7], 0xC)[0] == 2
print("mv64: sec5 %d bytes, two TD records %r %r, sec7 td count 2" % (len(mv[5]), r0, r1))
os.unlink(add_path)
os.unlink(mv_path)
print("PASS")
