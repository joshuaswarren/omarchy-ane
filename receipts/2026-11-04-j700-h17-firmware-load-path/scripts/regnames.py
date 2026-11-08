#!/usr/bin/env python3
"""Print every `adrp xN,+add` constant-pool string reference in a VA window.

Used to recover the log format strings the driver prints for each register
offset (e.g. ANEHWDevice::DumpANERegisters), which name the registers.

usage: regnames.py <lo> <hi> [reg=x3]
"""
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

import ane

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True


def s(va, maxlen=240):
    d = ane.data()
    o = ane.off(va)
    e = d.find(b'\0', o, o + maxlen)
    return d[o:e].decode('utf-8', 'replace')


def main():
    lo = int(sys.argv[1], 0)
    hi = int(sys.argv[2], 0)
    reg = sys.argv[3] if len(sys.argv) > 3 else 'x3'
    ins = list(md.disasm(ane.data()[ane.off(lo):ane.off(hi)], lo))
    for k, i in enumerate(ins):
        if i.mnemonic == 'adrp' and i.reg_name(i.operands[0].reg) == reg:
            base = i.operands[1].imm % (1 << 64)
            nxt = ins[k + 1] if k + 1 < len(ins) else None
            va = base
            if nxt and nxt.mnemonic == 'add' and nxt.reg_name(nxt.operands[0].reg) == reg:
                va = base + nxt.operands[2].imm
            elif nxt and nxt.mnemonic in ('ldr', 'ldrb', 'str', 'strb'):
                for op in nxt.operands:
                    if op.type == 3 and op.mem.disp:
                        va = base + op.mem.disp
            print('%#014x  %#014x  %r' % (i.address, va, s(va)))


if __name__ == '__main__':
    main()
