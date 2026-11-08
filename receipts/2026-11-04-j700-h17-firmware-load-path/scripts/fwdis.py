#!/usr/bin/env python3
"""Disassemble the pinned H17 firmware image, with adrp+add and literal folding.

Image identity (refuses to run otherwise):
  /usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho
  SHA-256 32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c

Segments (from the Mach-O header, `fw.py macho`):
  __TEXT       vmaddr 0x0      fileoff 0x4000   size 0xc0000
  __DATA       vmaddr 0xc0000  fileoff 0xc4000  size 0xbc000

Usage:
  fwdis.py fn   <va> [count]     disassemble count instructions from VA
                                 (default 64), fold adrp+add, annotate
                                 ldr-literal loads with the qword they read
  fwdis.py data <va> [qwords]    dump little-endian qwords from VA, with
                                 target annotation when a qword looks like a
                                 pointer into a mapped segment
  fwdis.py br   <va>             every b/bl/conditional-b in the code window
                                 whose immediate target is <va>
  fwdis.py adrp <va>             every folded adrp+add in the code window
                                 whose result lies on <va>'s page
  Code window for br/adrp scans: VA 0x4000..0x9d298 (the code candidate
  window from scan_fw_dispatch_refs.py).
"""
import hashlib
import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64_const import ARM64_OP_IMM, ARM64_OP_REG, ARM64_OP_MEM

FW = '/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho'
SHA = '32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c'
SEGS = [(0x0, 0xc0000, 0x4000), (0xc0000, 0x2ac000, 0xc4000)]
CODE_LO, CODE_HI = 0, 0x9d298

_d = None


def data():
    global _d
    if _d is None:
        _d = open(FW, 'rb').read()
        got = hashlib.sha256(_d).hexdigest()
        if got != SHA:
            raise SystemExit('unexpected firmware SHA-256: %s' % got)
    return _d


def off(va):
    for vm, vs, fo in SEGS:
        if vm <= va < vm + vs:
            return fo + (va - vm)
    raise ValueError('VA %#x outside mapped segments' % va)


def read(va, n):
    o = off(va)
    return data()[o:o + n]


def qword(va):
    return struct.unpack('<Q', read(va, 8))[0]


def cstr(va, maxlen=240):
    try:
        b = read(va, maxlen)
    except ValueError:
        return None
    z = b.find(b'\0')
    if z <= 0:
        return None
    s = b[:z]
    if any(c < 0x20 and c != 0x0a for c in s):
        return None
    return s.decode('utf-8', 'replace')


def md():
    m = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    m.detail = True
    return m


def sweep(lo, hi):
    m = md()
    m.skipdata = True
    return list(m.disasm(read(lo, hi - lo), lo))


def annotate(ins, regs):
    """Fold adrp+add into regs; annotate folded adds and ldr-literal loads."""
    if ins.mnemonic in ('.byte', '(bad)', ''):
        regs.clear()
        return ''
    ops = ins.operands
    if ins.mnemonic == 'adrp' and ops and ops[0].type == ARM64_OP_REG:
        regs[ops[0].reg] = ops[1].imm
        return ''
    if ins.mnemonic == 'add' and len(ops) == 3 and ops[1].type == ARM64_OP_REG \
            and ops[2].type == ARM64_OP_IMM and ops[1].reg in regs:
        tgt = regs[ops[1].reg] + ops[2].imm
        regs[ops[0].reg] = tgt
        s = cstr(tgt)
        return '  ; = %#x%s' % (tgt, ' "%s"' % s if s else '')
    if ins.mnemonic == 'ldr':
        ops = ins.operands
        if len(ops) == 2 and ops[1].type == ARM64_OP_MEM:
            mem = ops[1].mem
            if mem.base == 0 and mem.index == 0:  # literal: base == #pc
                lit = ins.address + mem.disp
                try:
                    v = qword(lit)
                    s = cstr(v)
                    return '  ; lit %#x -> %#x%s' % (lit, v,
                                                     ' "%s"' % s if s else '')
                except ValueError:
                    return '  ; lit %#x' % lit
    return ''


def fn(va, count):
    regs = {}
    for i in md().disasm(read(va, count * 4), va):
        print('%#x: %s\t%s\t%s%s' % (i.address, i.bytes.hex(' '), i.mnemonic,
                                     i.op_str, annotate(i, regs)))


def dump(va, n):
    for k in range(n):
        v = qword(va + 8 * k)
        s = cstr(v) if 0 < v < 0x36c000 else None
        print('%#x: %#018x%s' % (va + 8 * k, v, ' -> "%s"' % s if s else ''))


def br(target):
    imm = '#' + hex(target)
    for i in sweep(CODE_LO, CODE_HI):
        if i.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or \
                i.mnemonic.startswith('b.'):
            if imm in i.op_str:
                print('%#x: %s\t%s\t%s' % (i.address, i.bytes.hex(' '),
                                           i.mnemonic, i.op_str))


def adrp(target):
    page = target & ~0xfff
    regs = {}
    for i in sweep(CODE_LO, CODE_HI):
        ann = annotate(i, regs)
        if ann:
            try:
                v = int(ann.split('=')[1].split()[0].split('"')[0], 0)
            except (ValueError, IndexError):
                v = None
            if v is not None and v & ~0xfff == page:
                print('%#x: %s\t%s\t%s%s' % (i.address, i.bytes.hex(' '),
                                             i.mnemonic, i.op_str, ann))


def main():
    mode = sys.argv[1]
    va = int(sys.argv[2], 0)
    if mode == 'fn':
        fn(va, int(sys.argv[3]) if len(sys.argv) > 3 else 64)
    elif mode == 'data':
        dump(va, int(sys.argv[3]) if len(sys.argv) > 3 else 16)
    elif mode == 'br':
        br(va)
    elif mode == 'adrp':
        adrp(va)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
