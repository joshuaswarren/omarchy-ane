#!/usr/bin/env python3
"""Offline check for ane-h18-pack.py.

For the add program the packed sections must be byte-identical to the
hardware-validated image (the add fixture runs 100 of 100 calls on the M6 with
exactly these bytes). For the matvec program no hardware-validated reference
exists; the check is that the procedure section grows to two TD records and the
TD-property record carries the declared TD count.

usage: test_h18_pack.py
"""
import struct
import subprocess
import sys
import tempfile
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ADD_BASE = "/home/joshuawarren/scratch/w91-m6h18-prep3/packs/ane_h18_prog-add.bin"
ADD_HWX = "/home/joshuawarren/scratch/w73/m6-prog/add-h18.hwx"
MV_BASE = "/home/joshuawarren/scratch/w91/mv18/ane_h18_prog-mv64.bin"
MV_HWX = "/home/joshuawarren/scratch/w91/mv18/mv64.hwx"


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
print("add: sec5 and sec7 byte-identical to the hardware-validated image")

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
