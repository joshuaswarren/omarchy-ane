#!/usr/bin/env python3
"""ascdbg_probe.py — emit commands that test which addresses the halted ASC
core can read. Each address: movz/movk x18 = addr, ldr w0,[x18], then read
ESR_EL1/FAR_EL1. A nonzero ESR means the access faulted.
usage: ascdbg_probe.py 0xADDR [0xADDR ...]
"""
import sys
sys.path.insert(0, "/tmp/t6001-ascdbg/ane/h13")
import ascdbg_regs as a

ED = a.ED
LDR = 0xB9400260  # ldr w0, [x19]


def mov_imm(rd, val):
    out = [a.itr(a.movz(rd, val & 0xFFFF, 0), f"x{rd} = {val:#x}")]
    for s in (16, 32, 48):
        part = (val >> s) & 0xFFFF
        if part:
            out.append(a.itr(a.movk(rd, part, s)))
    return out


def probe(addr):
    out = mov_imm(19, addr)
    out.append(a.itr(LDR, f"ldr w0, [{addr:#x}]"))
    out.append(a.itr(a.mrs(a.T, 3, 0, 5, 2, 0), "mrs x18, ESR_EL1"))
    out += a.exfil(f"ESR@{addr:#x}")
    out.append(a.itr(a.mrs(a.T, 3, 0, 6, 0, 0), "mrs x18, FAR_EL1"))
    out += a.exfil(f"FAR@{addr:#x}")
    out.append(a.itr(0xD5185200, "msr ESR_EL1, xzr"))  # clear for next probe
    return out


if __name__ == "__main__":
    lines = []
    for arg in sys.argv[1:]:
        lines += probe(int(arg, 16))
    print("\n".join(lines))
