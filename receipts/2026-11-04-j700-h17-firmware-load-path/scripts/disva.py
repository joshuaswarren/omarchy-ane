#!/usr/bin/env python3
"""Disassemble a VA range or a whole function from the retained kext.

  dis.py va 0xfffffe0008f74130 48        48 instructions from VA
  dis.py fn 0xfffffe0008f416b4           stop at next symbol boundary (retab heuristic off; fixed count)
"""
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

import ane


def main():
    mode = sys.argv[1]
    lo = int(sys.argv[2], 0)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    hi_off = ane.off(lo)
    data = ane.data()[hi_off:hi_off + n * 4]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    for i in md.disasm(data, lo):
        print(f"{i.address:#x}: {i.bytes.hex(' ')}\t{i.mnemonic}\t{i.op_str}")


main()
