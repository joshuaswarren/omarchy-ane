#!/usr/bin/env python3
"""ascdbg_regs.py — emit ane_ascdbg `itr` command lines that read core
registers of a halted ASC through EDITR, exfiltrating via DBGDTR_EL0
(DTRRX = high half, DTRTX = low half). x18 is the scratch GPR.

usage: ascdbg_regs.py [selftest|probe|gprs|sys|all] > cmds.txt
"""
import sys

ED = 0x1010000
DTRRX = ED + 0x80
T = 18  # scratch register


def itr(ins, tag=""):
    return f"itr {ED:#x} {ins:#010x} 500" + (f"  # {tag}" if tag else "")



def mrs(rt, op0, op1, crn, crm, op2):
    return 0xD5300000 | ((op0 - 2) << 19) | (op1 << 16) | (crn << 12) | (crm << 8) | (op2 << 5) | rt


def msr_dbgdtr(rt):  # msr DBGDTR_EL0, xN  (op0=2 op1=3 CRn=0 CRm=4 op2=0)
    return 0xD5130400 | rt


def movz(rd, imm16, shift):
    return 0xD2800000 | ((shift // 16) << 21) | (imm16 << 5) | rd


def orr_reg(rd, rn, rm):
    return 0xAA000000 | (rm << 16) | (rn << 5) | rd

def movk(rd, imm16, shift):
    return 0xF2800000 | ((shift // 16) << 21) | (imm16 << 5) | rd


def lsl32(rd, rn):  # ubfm xd, xn, #32, #31
    return 0xD3400000 | (32 << 16) | (31 << 10) | (rn << 5) | rd


def mov_from_sp(rd):
    return 0x91000000 | (31 << 5) | rd

DTRTX = ED + 0x8C


def exfil(name):
    """x18 holds the value: DBGDTR_EL0 splits it across DTRRX (hi) and DTRTX (lo)."""
    return [
        itr(msr_dbgdtr(T), f"{name} -> DBGDTR_EL0"),
        f"r32 {DTRRX:#x}  # {name}[63:32]",
        f"r32 {DTRTX:#x}  # {name}[31:0]",
    ]



SYSREGS = {
    # name: (op0, op1, crn, crm, op2)
    "DLR_EL0": (3, 3, 4, 5, 1),
    "DSPSR_EL0": (3, 3, 4, 5, 0),
    "CurrentEL": (3, 0, 4, 2, 2),
    "DAIF": (3, 3, 4, 2, 1),
    "ISR_EL1": (3, 0, 12, 1, 0),
    "ESR_EL1": (3, 0, 5, 2, 0),
    "FAR_EL1": (3, 0, 6, 0, 0),
    "ELR_EL1": (3, 0, 4, 0, 1),
    "SPSR_EL1": (3, 0, 4, 0, 0),
    "VBAR_EL1": (3, 0, 12, 0, 0),
    "SCTLR_EL1": (3, 0, 1, 0, 0),
    "TCR_EL1": (3, 0, 2, 0, 2),
    "TTBR0_EL1": (3, 0, 2, 0, 0),
    "TTBR1_EL1": (3, 0, 2, 0, 1),
    "MAIR_EL1": (3, 0, 10, 2, 0),
    "MIDR_EL1": (3, 0, 0, 0, 0),
    "MPIDR_EL1": (3, 0, 0, 0, 5),
    "SP_EL0": (3, 0, 4, 1, 0),
    "CNTPCT_EL0": (3, 3, 14, 0, 1),
}


def selftest():
    out = [f"r32 {ED + 0x88:#x}  # EDSCR before"]
    out += [itr(movz(T, 0x1234, 16), "x18 = 0x12340000"),
            itr(movk(T, 0x5678, 0), "x18 = 0x12345678")]
    out += exfil("selftest(expect hi 0, lo 12345678)")
    out += [f"r32 {ED + 0x88:#x}  # EDSCR after"]
    return out


def sysregs(names):
    out = []
    for n in names:
        out.append(itr(mrs(T, *SYSREGS[n]), f"mrs x18, {n}"))
        out += exfil(n)
    return out


def gprs():
    out = []
    for r in range(18):
        out += [itr(orr_reg(T, 31, r), f"x18 = x{r}")] + exfil(f"x{r}")
    for r in range(19, 31):
        out += [itr(orr_reg(T, 31, r), f"x18 = x{r}")] + exfil(f"x{r}")
    out += [itr(mov_from_sp(T), "x18 = sp")] + exfil("sp")
    return out


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "selftest"
    if what == "selftest":
        lines = selftest()
    elif what == "probe":
        lines = sysregs(["DLR_EL0", "DSPSR_EL0", "CurrentEL", "ISR_EL1", "DAIF"])
    elif what == "sys":
        lines = sysregs([n for n in SYSREGS if n not in ("DLR_EL0", "DSPSR_EL0", "CurrentEL", "ISR_EL1", "DAIF")])
    elif what == "gprs":
        lines = gprs()
    elif what == "all":
        lines = selftest() + sysregs(list(SYSREGS)) + gprs()
    else:
        sys.exit("unknown selection")
    print("\n".join(lines))
