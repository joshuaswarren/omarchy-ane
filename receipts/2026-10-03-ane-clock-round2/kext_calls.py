#!/usr/bin/env python3
"""List the calls (BL/B to symbols or import stubs) and immediate MOVs of named functions in an arm64 kext.
Read-only: LIEF + capstone.  Usage: kext_calls.py <kext> <function-substring> [...]"""
import bisect
import sys

import capstone
import lief

b = lief.parse(sys.argv[1])
stubs = {bi.address: bi.symbol.name for bi in b.bindings if bi.symbol}
funcs = sorted((s.value, s.name) for s in b.symbols if s.value and s.name.startswith(("__Z", "___Z")))
starts = [v for v, _ in funcs]
text = next(s for s in b.sections if s.name == "__text")
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)


def name(addr):
    if addr in stubs:
        return "stub:" + stubs[addr]
    i = bisect.bisect_right(starts, addr) - 1
    return "%s+0x%x" % (funcs[i][1], addr - funcs[i][0]) if i >= 0 else hex(addr)


for needle in sys.argv[2:]:
    for k, (start, fname) in enumerate(funcs):
        if needle not in fname or not (text.virtual_address <= start < text.virtual_address + text.size):
            continue
        end = funcs[k + 1][0] if k + 1 < len(funcs) else start + 0x400
        code = bytes(b.get_content_from_virtual_address(start, end - start))
        print("\n== %s 0x%x..0x%x" % (fname, start, end))
        for ins in md.disasm(code, start):
            if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
                t = int(ins.op_str.lstrip("#"), 16)
                if ins.mnemonic == "bl" or not (start <= t < end):
                    print("  0x%x %s %s" % (ins.address, ins.mnemonic, name(t)))
            elif ins.mnemonic in ("blraa", "blr", "braa", "br"):
                print("  0x%x %s %s (indirect)" % (ins.address, ins.mnemonic, ins.op_str))
            elif ins.mnemonic in ("mov", "movk", "movz", "orr") and "#" in ins.op_str and ins.op_str.startswith("w"):
                print("  0x%x %s %s" % (ins.address, ins.mnemonic, ins.op_str))
