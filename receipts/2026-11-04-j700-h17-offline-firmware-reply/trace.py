#!/usr/bin/env python3
"""Offline bounded disassembly and reference scans of retained H17 firmware."""
import hashlib
import struct
import sys
from pathlib import Path
import capstone

FW = Path('/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho')
SHA = '32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c'
d = FW.read_bytes()
if hashlib.sha256(d).hexdigest() != SHA:
    raise SystemExit('firmware digest mismatch')
segments = []
off = 32
for _ in range(struct.unpack_from('<I', d, 16)[0]):
    cmd, size = struct.unpack_from('<II', d, off)
    if cmd == 0x19:
        name = d[off+8:off+24].rstrip(b'\0').decode()
        va, vs, fo, fs = struct.unpack_from('<QQQQ', d, off+24)
        segments.append((name, va, vs, fo, fs))
    off += size

def fileoff(va):
    for _, base, _, fo, fs in segments:
        if base <= va < base + fs:
            return fo + va - base
    raise ValueError(hex(va))

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata = True

def dis(a, end):
    for i in md.disasm(d[fileoff(a):fileoff(a)+end-a], a):
        print(f'VA {i.address:08x} file {fileoff(i.address):08x}  {i.bytes.hex():8}  {i.mnemonic:8} {i.op_str}'.rstrip())

if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'info':
        print(FW, '\nSHA256', SHA, '\nbytes', len(d), '\ncapstone', capstone.__version__)
        for n, a, s, f, z in segments:
            print(n, 'VA', hex(a), 'vmsize', hex(s), 'file', hex(f), 'filesize', hex(z))
    elif mode == 'dis':
        dis(int(sys.argv[2], 0), int(sys.argv[3], 0))
    elif mode == 'refs':
        target = int(sys.argv[2], 0)
        for i in md.disasm(d[0x4000:0xc4000], 0):
            if i.mnemonic in ('bl', 'b') and i.op_str == f'#{target:#x}':
                print(hex(i.address), i.mnemonic, i.op_str)
    elif mode == 'ptr':
        needle = struct.pack('<Q', int(sys.argv[2], 0))
        for n, a, _, f, z in segments:
            for p in range(f, f+z-7, 8):
                if d[p:p+8] == needle:
                    print(n, 'VA', hex(a+p-f), 'file', hex(p))
