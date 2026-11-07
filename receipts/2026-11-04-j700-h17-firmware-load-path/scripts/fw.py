#!/usr/bin/env python3
"""Re-derive the J700/T8140 ANE firmware placement facts, offline.

Three independent inputs, none of them a device:
  * the retained kernelcache  /var/tmp/neo/kernelcache.bin
  * the retained ADT          /var/tmp/opencode/adt.bin
  * the retained firmware     /usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho

usage: fw.py [adt|rvbar DVA|macho|regs|all]
"""
import hashlib
import struct
import sys

import ane

ADT = '/var/tmp/opencode/adt.bin'
FW = '/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho'
PARSER = '/home/admin/linux-aurora/tools/aurora-sep/aurora-adt-extract.py'

# ---- /arm-io/ane reg[] as {addr u64, size u64} ------------------------------
# DT address -> physical: /arm-io ranges entry 0 is
#   child 0x0 .. 0x2f000000  ->  parent 0x2_1000_0000 .. (+0x2f000000)
WIN = ['ANE', 'PS', 'PWGATE', 'PTD', 'ANEHAL1', 'ANEHAL2']
RANGES = []          # [(child_base, parent_base, size)] from /arm-io


def dt_to_phys(a):
    for c, p, s in RANGES:
        if c <= a < c + s:
            return a - c + p
    return None


def adt_parse():
    import importlib.util
    spec = importlib.util.spec_from_file_location('adt', PARSER)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    root, used, n, p = m.parse_adt(open(ADT, 'rb').read())
    for path, node in m.walk(root):
        props = {pr.name: pr.value for pr in node.props}
        if path == '/arm-io':
            r = props.get('ranges', b'')
            for i in range(len(r) // 24):
                c, pa, s = struct.unpack_from('<QQQ', r, i * 24)
                RANGES.append((c, pa, s))
        if path == '/arm-io/ane':
            ane_props = props
    return ane_props


def do_regs(props):
    print('/arm-io ranges[] (child_base, parent_base, size)')
    for c, p, s in RANGES:
        print('  child %#014x .. %#x  ->  parent %#014x  (+%#x)'
              % (c, c + s, p, p - c))
    reg = props['reg']
    print('/arm-io/ane reg[]  (%d bytes = %d windows)' % (len(reg), len(reg) // 16))
    for i in range(len(reg) // 16):
        a, s = struct.unpack_from('<QQ', reg, i * 16)
        nm = WIN[i] if i < len(WIN) else '?'
        print('  [%d] %-8s dt=%#012x size=%#x  phys=%s'
              % (i, nm, a, s, ('%#012x' % dt_to_phys(a)) if dt_to_phys(a) else '-'))


def do_adt(props):
    print('ANECPU-relevant properties')
    for k in ('ane-type', 'pre-loaded', 'compatible', 'iommu-parent',
              'clock-ids', 'clock-gates', 'power-gates'):
        v = props.get(k)
        if v is None:
            print('  %-14s absent' % k)
        elif len(v) <= 16:
            u = struct.unpack('<%dI' % (len(v) // 4), v) if len(v) % 4 == 0 and v else ()
            print('  %-14s %s  u32=%s' % (k, v.hex(), [hex(x) for x in u]))
        else:
            print('  %-14s %d bytes' % (k, len(v)))
    seg = props['segment-ranges']
    print('\nsegment-ranges  (%d bytes)' % len(seg))
    for i in range(len(seg) // 32):
        phys, off, remap, sz = struct.unpack_from('<QQQQ', seg, i * 32)
        print('  [%d] phys=%#014x iova_offset=%#x remap=%#014x size=%#x flags=%#x'
              % (i, phys, off, remap, sz & 0xffffffff, sz >> 32))
    if 'dart-ane' in str(props):
        pass


def do_rvbar(dva):
    mask = 0xff7efffffffff800
    imm = 0x0081000000000001
    print('RVBAR compose, exactly as ANEHWDevice::ANE_Init does it')
    print('  mask (mov/movk x21) = %#018x' % mask)
    print('  or   (mov/movk x20) = %#018x' % imm)
    print('  dva                 = %#018x' % dva)
    print('  RVBAR               = %#018x' % ((dva & mask) | imm))
    print('  ... and for the ADT __TEXT remap 0x10000000000:')
    print('  RVBAR               = %#018x' % ((0x10000000000 & mask) | imm))
    print('  iBoot-style remap|1 = %#018x' % (0x10000000000 | 1))


def do_macho():
    d = open(FW, 'rb').read()
    print('%s\n  %d bytes sha256 %s' % (FW, len(d), hashlib.sha256(d).hexdigest()))
    magic, = struct.unpack_from('<I', d, 0)
    cputype, cpusub, ftype, ncmds, sizeofcmds, flags = struct.unpack_from('<iiIIII', d, 4)
    print('  magic %#x filetype %d (5 = MH_PRELOAD) cputype %#x cpusub %#x ncmds %d'
          % (magic, ftype, cputype & 0xffffffff, cpusub & 0xffffffff, ncmds))
    off = 32
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', d, off)
        if cmd == 0x19:
            nm = d[off + 8:off + 24].split(b'\0')[0].decode()
            vmaddr, vmsize, foff, fsize = struct.unpack_from('<QQQQ', d, off + 24)
            print('  SEG %-11s vmaddr %#x vmsize %#x fileoff %#x filesize %#x'
                  % (nm, vmaddr, vmsize, foff, fsize))
        off += sz
    # 0x08042006 sites, __TEXT is file 0x4000 == vmaddr 0
    text = d[0x4000:0x4000 + 0xc0000]
    hits = []
    for i in range(0, len(text) - 8, 4):
        w = struct.unpack_from('<I', text, i)[0]
        if (w & 0x7f800000) == 0x52800000 and ((w >> 5) & 0xffff) == 0x2006 and (w >> 21) & 3 == 0:
            w2 = struct.unpack_from('<I', text, i + 4)[0]
            if ((w2 >> 5) & 0xffff) == 0x0804 and (w2 & 0x1f) == (w & 0x1f):
                hits.append(i)
    print('  0x08042006 built as movz/movk pairs at __TEXT VA: %s'
          % ', '.join(hex(h) for h in hits))
    for h in hits:
        print('    VA %#x (file %#x): ... -> %s' % (h, 0x4000 + h,
              ' '.join('%02x' % b for b in text[h:h + 8])))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    props = None if what == 'rvbar' or what == 'macho' else adt_parse()
    if what in ('regs', 'all'):
        do_regs(props)
        print()
    if what in ('adt', 'all'):
        do_adt(props)
        print()
    if what == 'rvbar':
        do_rvbar(int(sys.argv[2], 0))
    elif what in ('macho', 'all'):
        do_macho()


if __name__ == '__main__':
    main()
