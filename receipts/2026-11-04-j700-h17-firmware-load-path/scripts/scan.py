#!/usr/bin/env python3
"""Linear-sweep scanner over a kext __text range.

Modes:
  scan.py disp 0x490            every ld/st whose memory operand displacement is 0x490
  scan.py imm 0x81000000000001   every instruction with that immediate anywhere in its text
  scan.py adrp 0xfffffe00073b0000  every adrp (with the +add folded) yielding that VA
  scan.py str <needle>           every instruction whose text contains <needle>
Each hit prints: VA, raw bytes, instruction, and the enclosing symbol (nearest
preceding __ZN* symbol from LC_SYMTAB) with +delta.
"""
import bisect
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

import ane

RANGES = {
    'iface': (0xfffffe0008eeb820, 0xfffffe0008ff8dd8, ane.IFACE),
    'hal': (0xfffffe0009799040, 0xfffffe00097a9620, ane.HAL),
}


def sweep(lo, hi):
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    d = ane.data()[ane.off(lo):ane.off(hi)]
    return list(md.disasm(d, lo))


def enclosing(ins, tab):
    i = bisect.bisect_right(tab.addrs, ins.address) - 1
    if i < 0:
        return None
    a = tab.addrs[i]
    return '%s+%#x' % (tab.map[a], ins.address - a)


def main():
    mode = sys.argv[1]
    arg = sys.argv[2]
    which = sys.argv[3] if len(sys.argv) > 3 else 'iface'
    lo, hi, tab = RANGES[which]
    ins = sweep(lo, hi)
    val = None
    try:
        val = int(arg, 0)
    except ValueError:
        pass

    # fold adrp + add
    for k, i in enumerate(ins):
        show = False
        note = ''
        if mode == 'disp':
            for op in i.operands:
                if op.type == 3 and op.mem.disp == val:  # ARM64_OP_MEM
                    show = True
        elif mode == 'imm':
            for op in i.operands:
                # ARM64_OP_IMM; capstone hands back a signed 64-bit value for
                # kernel VAs, so fold to unsigned before comparing.
                if op.type == 2 and val is not None and (op.imm % (1 << 64)) == val:
                    show = True
        elif mode == 'str':
            if arg.lower() in ('%s %s' % (i.mnemonic, i.op_str)).lower():
                show = True
        elif mode == 'adrp':
            if i.mnemonic == 'adrp':
                base = i.operands[1].imm
                # look ahead for a single add with the same reg
                if k + 1 < len(ins) and ins[k + 1].mnemonic in ('add', 'ldr', 'str', 'ldrb', 'strb'):
                    nxt = ins[k + 1]
                    tgt = base
                    if nxt.mnemonic == 'add' and nxt.operands[2].type == 2:
                        tgt = base + nxt.operands[2].imm
                    elif nxt.mnemonic in ('ldr', 'str', 'ldrb', 'strb'):
                        for op in nxt.operands:
                            if op.type == 3 and op.mem.disp:
                                tgt = base + op.mem.disp
                    if tgt == val:
                        show = True
                        note = ' -> %#x via %s' % (tgt, nxt.mnemonic)
        if show:
            bs = ' '.join('%02x' % b for b in i.bytes)
            print('%#014x: %-20s %-8s %-42s %s%s' % (
                i.address, bs, i.mnemonic, i.op_str, enclosing(i, tab) or '', note))


if __name__ == '__main__':
    main()
