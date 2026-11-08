#!/usr/bin/env python3
"""Shared helper: kernelcache VA<->file mapping, capstone disassembly with
symbol and raw-byte annotation, and symbol lookup for the two ANE kexts.

kernelcache: /var/tmp/neo/kernelcache.bin
             SHA-256 7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe
             VA = 0xfffffe0007004000 + file_offset
"""
import bisect
import json
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

KC = '/var/tmp/neo/kernelcache.bin'
BASE = 0xfffffe0007004000
SYMDIR = '/var/tmp/neo'

_data = None


def data():
    global _data
    if _data is None:
        _data = open(KC, 'rb').read()
    return _data


def off(va):
    return va - BASE


def raw(va, n=4):
    o = off(va)
    return data()[o:o + n]


def u32(va):
    return int.from_bytes(raw(va, 4), 'little')


def u64(va):
    return int.from_bytes(raw(va, 8), 'little')


_syms = {}


def syms(name):
    if name not in _syms:
        _syms[name] = json.load(open('%s/syms_com_apple_driver_%s.json' % (SYMDIR, name)))
    return _syms[name]


class SymTab:
    def __init__(self, name, lo, hi):
        self.lo, self.hi = lo, hi
        self.addrs = []
        self.map = {}
        for s in syms(name):
            v = s['v']
            if lo <= v < hi:
                self.map[v] = s['n']
                self.addrs.append(v)
        self.addrs.sort()

    def at(self, va):
        return self.map.get(va)

    def near(self, va):
        i = bisect.bisect_right(self.addrs, va) - 1
        if i < 0:
            return None
        a = self.addrs[i]
        if va - a > 0x4000:
            return None
        return self.map[a], va - a


IFACE = SymTab('AppleH16ANEInterface', 0xfffffe0008eeb820, 0xfffffe0008ff8dd8)
HAL = SymTab('AppleT8140ANEHAL', 0xfffffe0009799040, 0xfffffe00097a9620)

_md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
_md.detail = True


def dis(va, n=60, sym=None, stop_at=()):
    """Linear disassembly from va for n instructions (or until an instr whose
    mnemonic is in stop_at, after having printed it)."""
    o = off(va)
    tabs = [t for t in (IFACE, HAL, sym) if t is not None]
    prev_blank = False
    for i in _md.disasm(data()[o:o + n * 4], va):
        tag = ''
        for t in tabs:
            hit = t.at(i.address)
            if hit:
                tag = '  <-- ' + hit
                break
        if not tag and not prev_blank and i.address != va:
            for t in tabs:
                nb = t.near(i.address)
                if nb and nb[1] == 0:
                    tag = '  ; ' + nb[0]
                    break
        bs = ' '.join('%02x' % b for b in i.bytes)
        print('%#014x: %-24s %-9s %s%s' % (i.address, bs, i.mnemonic, i.op_str, tag))
        prev_blank = bool(tag)
        if i.mnemonic in stop_at and i.address != va:
            break


if __name__ == '__main__':
    va = int(sys.argv[1], 16)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 60
    dis(va, n)
