#!/usr/bin/env python3
"""Whole-kernelcache image index: fileset (kext) table, global symbol table,
and image-wide scans for raw pointers, branch targets, and byte strings.

The kernelcache (MH_FILESET, filetype 12) is one linear mapping:
    VA = 0xfffffe0007004000 + file_offset
across every top-level segment, so ane.py's off() holds image-wide.

Usage:
  python3 kimage.py list [substring]      kext fileset entries with extents
  python3 kimage.py sym <va>              global symbolize (nearest, any kext)
  python3 kimage.py ptr <va>              raw-u64 slots equal to <va>, image-wide
  python3 kimage.py bl <va>               every b/bl whose target is <va>
  python3 kimage.py find <needle>         byte needle anywhere in the image
Index is cached in kc-index.json next to this script.
"""
import bisect
import json
import os
import struct
import sys

import ane

IDX = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kc-index.json')
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
LC_FILESET_ENTRY = 0x80000035


def _parse(base):
    d = ane.data()
    magic, _, _, filetype, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', d, base)[:6]
    assert magic == 0xFEEDFACF, hex(magic)
    o = base + 32
    segs, sym, ents = [], None, []
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<2I', d, o)
        if cmd == LC_SEGMENT_64:
            nm = d[o + 8:o + 24].split(b'\0')[0].decode('ascii', 'replace')
            vm, vs, fo, fs = struct.unpack_from('<4Q', d, o + 24)
            segs.append((nm, vm, vs, fo, fs))
        elif cmd == LC_SYMTAB:
            sym = struct.unpack_from('<4I', d, o + 8)
        elif cmd == LC_FILESET_ENTRY:
            vm, fo = struct.unpack_from('<2Q', d, o + 8)
            nm = d[o + 0x20:o + 0x20 + 64].split(b'\0')[0].decode('ascii', 'replace')
            ents.append((nm, vm, fo))
        o += cs
    return filetype, segs, sym, ents


def build_index():
    kexts = []
    _, _, _, ents = _parse(0)
    d = ane.data()
    for nm, vm, fo in ents:
        _, ksegs, ksym, _ = _parse(fo)
        segl = [{'name': s[0], 'vm': s[1], 'vmaddr': s[1], 'vmsize': s[2],
                 'fileoff': s[3], 'filesize': s[4]} for s in ksegs]
        lo = hi = 0
        if ksym:
            symoff, nsyms, stroff, _ = ksym
            for i in range(nsyms):
                v = struct.unpack_from('<IBBHQ', d, symoff + i * 16)[4]
                if v:
                    lo = v if not lo else min(lo, v)
                    hi = max(hi, v)
        kexts.append({'name': nm, 'lo': lo, 'hi': hi, 'segs': segl,
                      'sym': list(ksym) if ksym else None})
    return {'base': 0xfffffe0007004000, 'kexts': kexts}


def _load():
    if not os.path.exists(IDX):
        json.dump(build_index(), open(IDX, 'w'))
    return json.load(open(IDX))


class Global:
    """Global symbol table: (addr, name, kext) sorted by addr."""

    def __init__(self):
        idx = _load()
        self.kexts = idx['kexts']
        addrs, names, kexts = [], [], []
        d = ane.data()
        for k in self.kexts:
            if not k['sym']:
                continue
            symoff, nsyms, stroff, _ = k['sym']
            for i in range(nsyms):
                n_strx, _t, _s, _d, n_value = struct.unpack_from(
                    '<IBBHQ', d, symoff + i * 16)
                if n_value == 0:
                    continue
                s0 = stroff + n_strx
                s1 = d.index(b'\0', s0)
                addrs.append(n_value)
                names.append(d[s0:s1].decode('ascii', 'replace'))
                kexts.append(k['name'])
        order = sorted(range(len(addrs)), key=lambda i: addrs[i])
        self.addrs = [addrs[i] for i in order]
        self.names = [names[i] for i in order]
        self.kexts_by_sym = [kexts[i] for i in order]

    def kext_of(self, va):
        i = bisect.bisect_left(self.addrs, va)
        if i < len(self.addrs) and self.addrs[i] == va:
            return self.kexts_by_sym[i]
        i = bisect.bisect_right(self.addrs, va) - 1
        return self.kexts_by_sym[i] if i >= 0 else None

    def near(self, va, maxd=0x8000):
        i = bisect.bisect_right(self.addrs, va) - 1
        if i < 0:
            return None
        if va - self.addrs[i] > maxd:
            return None
        return (self.names[i], va - self.addrs[i], self.kexts_by_sym[i])

    def at(self, va):
        i = bisect.bisect_left(self.addrs, va)
        if i < len(self.addrs) and self.addrs[i] == va:
            return self.names[i]
        return None


_G = None


def G():
    global _G
    if _G is None:
        _G = Global()
    return _G


TEXT_EXEC = (0xfffffe0008834000, 0xfffffe000bd3c000)


def scan_bl(target):
    """Every b/bl in __TEXT_EXEC (and BOOT_EXEC) whose PC-relative target == target."""
    d = ane.data()
    hits = []
    for lo, hi in (TEXT_EXEC, (0xfffffe000bd3c000, 0xfffffe000bd44000),
                   (0xfffffe0007004000, 0xfffffe000700c000)):
        o = ane.off(lo)
        for va in range(lo, hi, 4):
            w = int.from_bytes(d[o:o + 4], 'little')
            o += 4
            op = w >> 26
            if op in (0b000101, 0b100101):
                imm = w & 0x03FFFFFF
                if imm & 0x02000000:
                    imm -= 0x04000000
                if va + imm * 4 == target:
                    hits.append((va, op & 1, w))
    return hits


def scan_ptr(target):
    pat = target.to_bytes(8, 'little')
    d = ane.data()
    hits, start = [], 0
    while True:
        i = d.find(pat, start)
        if i < 0:
            break
        start = i + 1
        if i % 8 == 0:
            hits.append(ane.BASE + i)
    return hits


def scan_adrp(target):
    """Every `adrp Xn,#page ; add Xn,Xn,#lo12` pair in __TEXT_EXEC yielding
    `target` (the kernelcache idiom for taking a C-string's address)."""
    d = ane.data()
    page = target & ~0xFFF
    lo12 = target & 0xFFF
    hits = []
    lo, hi = TEXT_EXEC
    for va in range(lo, hi, 4):
        w = int.from_bytes(d[ane.off(va):ane.off(va) + 4], 'little')
        if (w & 0x9F000000) != 0x90000000:  # adrp
            continue
        immlo = (w >> 29) & 3
        immhi = (w >> 5) & 0x7FFFF
        imm = (immhi << 2) | immlo
        if imm & 0x100000:
            imm -= 0x200000
        if (va & ~0xFFF) + (imm << 12) != page:
            continue
        rd = w & 0x1F
        # look ahead up to 2 instructions for: add Xd, Xrd, #lo12
        for k in (1, 2):
            nva = va + 4 * k
            if nva >= hi:
                continue
            w2 = int.from_bytes(d[ane.off(nva):ane.off(nva) + 4], 'little')
            if (w2 >> 22) == 0b1001000100 and (w2 >> 5) & 0x1F == rd \
                    and (w2 >> 10) & 0xFFF == lo12:
                hits.append((nva, w, w2))
                break
    return hits


def scan_find(needle):
    d = ane.data()
    if isinstance(needle, str):
        needle = needle.encode()
    hits, start = [], 0
    while True:
        i = d.find(needle, start)
        if i < 0:
            break
        start = i + 1
        hits.append(ane.BASE + i)
    return hits


if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'list':
        sub = sys.argv[2] if len(sys.argv) > 2 else ''
        idx = _load()
        for k in sorted(idx['kexts'], key=lambda k: k['lo']):
            if sub.lower() in k['name'].lower():
                print('%016x..%016x %s' % (k['lo'], k['hi'], k['name']))
    elif mode == 'sym':
        va = int(sys.argv[2], 16)
        print(G().near(va))
    elif mode == 'ptr':
        va = int(sys.argv[2], 16)
        g = G()
        for va8 in scan_ptr(va):
            print('%#x  %s' % (va8, g.near(va8)))
    elif mode == 'bl':
        va = int(sys.argv[2], 16)
        g = G()
        for va4, is_bl, w in scan_bl(va):
            print('%#x  %s  %08x  %s' % (va4, 'bl' if is_bl else 'b', w, g.near(va4)))
    elif mode == 'adrp':
        va = int(sys.argv[2], 16)
        g = G()
        for nva, w, w2 in scan_adrp(va):
            print('%#x  %08x  %08x  %s' % (nva - 4, w, w2, g.near(nva)))
    elif mode == 'find':
        g = G()
        for va in scan_find(sys.argv[2]):
            print('%#x  %s' % (va, g.near(va)))
