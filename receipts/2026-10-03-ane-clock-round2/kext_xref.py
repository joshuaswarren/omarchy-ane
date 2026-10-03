#!/usr/bin/env python3
"""Find code that references a C string (ADRP+ADD) or calls a function (BL) in an arm64 kext, and print the
instruction window before each hit with the enclosing symbol. Read-only (LIEF + capstone; nothing executes).

Usage: kext_xref.py <kext> str <substring> [before]
       kext_xref.py <kext> call <symbol-substring> [before]
"""
import bisect
import sys

import capstone
import lief

b = lief.parse(sys.argv[1])
mode, needle = sys.argv[2], sys.argv[3]
before = int(sys.argv[4]) if len(sys.argv) > 4 else 24

funcs = sorted((s.value, s.name) for s in b.symbols if s.value and s.name.startswith("__Z"))
fstarts = [v for v, _ in funcs]


def owner(addr):
    i = bisect.bisect_right(fstarts, addr) - 1
    return "%s+0x%x" % (funcs[i][1], addr - funcs[i][0]) if i >= 0 else "?"


targets = set()
if mode == "str":
    for sec in b.sections:
        if sec.name in ("__cstring", "__os_log", "__const"):
            data = bytes(sec.content)
            pos = data.find(needle.encode())
            while pos >= 0:
                start = data.rfind(b"\0", 0, pos) + 1
                targets.add(sec.virtual_address + start)
                pos = data.find(needle.encode(), pos + 1)
else:
    targets = {v for v, n in funcs if needle in n}
print("targets:", ", ".join("0x%x" % t for t in sorted(targets)))

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
for sec in b.sections:
    if sec.name != "__text":
        continue
    code = bytes(sec.content)
    insns = list(md.disasm(code, sec.virtual_address))
    adrp = {}
    for k, ins in enumerate(insns):
        hit = None
        if ins.mnemonic == "adrp":
            reg, imm = ins.op_str.split(", ")
            adrp[reg] = int(imm.lstrip("#"), 16)
        elif ins.mnemonic == "add" and mode == "str":
            ops = [o.strip() for o in ins.op_str.split(",")]
            if len(ops) == 3 and ops[1] in adrp and ops[2].startswith("#"):
                if adrp[ops[1]] + int(ops[2].lstrip("#"), 16) in targets:
                    hit = ins
        elif ins.mnemonic == "bl" and mode == "call":
            if int(ins.op_str.lstrip("#"), 16) in targets:
                hit = ins
        if hit:
            print("\n== hit 0x%x in %s" % (hit.address, owner(hit.address)))
            for j in insns[max(0, k - before) : k + 3]:
                tag = ""
                if j.mnemonic == "bl":
                    tag = "  ; -> " + owner(int(j.op_str.lstrip("#"), 16))
                print("  0x%x  %-6s %s%s" % (j.address, j.mnemonic, j.op_str, tag))
