#!/usr/bin/env python3
"""Minimal read-only MH_FILESET kernelcache reader: per-kext symbols, VA->file mapping, disassembly.

Usage:
  kc_fileset.py <kc> syms <kext-bundle-id> [regex]          list symbols (VA, name)
  kc_fileset.py <kc> dis <kext-bundle-id> <symbol-substring> [max_insns]
  kc_fileset.py <kc> read <va-hex> <len>                      hex dump bytes at a VA
Nothing is executed or modified; the KC is parsed with struct + capstone.
"""
import re
import struct
import sys

import capstone

LC_SEGMENT_64, LC_SYMTAB, LC_FILESET_ENTRY = 0x19, 0x2, 0x80000035


class KC:
    def __init__(self, path):
        self.f = open(path, "rb").read()
        self.segs = []  # (vmaddr, vmsize, fileoff, filesize, name)
        self.entries = {}
        self._load_cmds(0, top=True)

    def _load_cmds(self, base, top=False):
        f = self.f
        ncmds = struct.unpack_from("<I", f, base + 16)[0]
        off = base + 32
        out = {"segs": [], "symtab": None}
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from("<II", f, off)
            if cmd == LC_SEGMENT_64:
                name = f[off + 8 : off + 24].split(b"\0")[0].decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", f, off + 24)
                out["segs"].append((vmaddr, vmsize, fileoff, filesize, name))
                if top:
                    self.segs.append((vmaddr, vmsize, fileoff, filesize, name))
            elif cmd == LC_SYMTAB:
                out["symtab"] = struct.unpack_from("<IIII", f, off + 8)
            elif cmd == LC_FILESET_ENTRY and top:
                vmaddr, fileoff, stroff = struct.unpack_from("<QQI", f, off + 8)
                self.entries[f[off + stroff : off + sz].split(b"\0")[0].decode()] = (vmaddr, fileoff)
            off += sz
        return out

    def va2off(self, va):
        for vmaddr, vmsize, fileoff, filesize, _ in self.segs:
            if vmaddr <= va < vmaddr + filesize:
                return fileoff + (va - vmaddr)
        return None

    def read(self, va, n):
        o = self.va2off(va)
        return None if o is None else self.f[o : o + n]

    def kext(self, bundle):
        _, fileoff = self.entries[bundle]
        info = self._load_cmds(fileoff)
        syms = []
        if info["symtab"]:
            symoff, nsyms, stroff, strsize = info["symtab"]
            for i in range(nsyms):
                n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from("<IBBHQ", self.f, symoff + 16 * i)
                if n_value and (n_type & 0x0E) == 0x0E:
                    name = self.f[stroff + n_strx : stroff + n_strx + 512].split(b"\0")[0].decode(errors="replace")
                    syms.append((n_value, name))
        syms.sort()
        return info["segs"], syms


def main():
    kc = KC(sys.argv[1])
    mode = sys.argv[2]
    if mode == "read":
        va, n = int(sys.argv[3], 16), int(sys.argv[4], 0)
        b = kc.read(va, n)
        for i in range(0, len(b), 16):
            print("0x%x: %s" % (va + i, b[i : i + 16].hex(" ")))
        return
    segs, syms = kc.kext(sys.argv[3])
    if mode == "syms":
        rx = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
        for v, n in syms:
            if not rx or rx.search(n):
                print("0x%x %s" % (v, n))
        return
    if mode == "dis":
        needle = sys.argv[4]
        maxn = int(sys.argv[5]) if len(sys.argv) > 5 else 400
        allsyms = []
        for b in kc.entries:
            try:
                allsyms += kc.kext(b)[1]
            except Exception:
                pass
        allsyms.sort()
        names = dict(allsyms)
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        for k, (v, n) in enumerate(syms):
            if needle not in n:
                continue
            end = syms[k + 1][0] if k + 1 < len(syms) else v + 4 * maxn
            code = kc.read(v, min(end - v, 4 * maxn))
            print("\n== %s 0x%x (%d bytes)" % (n, v, end - v))
            for ins in md.disasm(code, v):
                tag = ""
                if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
                    t = int(ins.op_str.lstrip("#"), 16)
                    if t in names:
                        tag = "  ; " + names[t]
                print("  0x%x  %-7s %s%s" % (ins.address, ins.mnemonic, ins.op_str, tag))


if __name__ == "__main__":
    main()
