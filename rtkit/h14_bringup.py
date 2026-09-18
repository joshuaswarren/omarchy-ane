#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""H14 (t6021) ANE RTKit/ASC phase-1 bring-up — userland, /dev/mem, staged.

Stages (each strictly read-only until the pmgr stage):
  0  snapshot: pmgr ane-chain PS register values (read-only)
  1  power: raise the ANE pmgr chain parent-first (the exact registers and
     read-modify-write the apple-pmgr-pwrstate genpd raise performs; the six
     domain raises completed cleanly on the three 2026-09-18 bind boots)
  2  probe: reads of the RTKit/ASC region the H14 kext itself touches at
     runtime (RVBAR, VERS, RTBuddy status, GPIO acks, EDPRCR, CPU_STATUS)
  3  mailbox: read-only sweep of standard Apple mailbox layouts inside the
     ANE block to identify the RTKit transport by EMPTY/FULL/pointer patterns

Discipline (2026-09-18 lane rules):
  - the engine window 0x285c04000..0x285c28000 is a named kill site (two
    netconsole-named external aborts at 0x285c2400c) - every address is
    asserted against it
  - no SET-block sidekick writes anywhere; PS writes go to the ps register
    itself, never to a +8 sidekick
  - first anomaly = stop; netconsole + this log are the capture

Run on t6021-test-host-linux as root:  python3 h14_bringup.py --stage N
"""
import argparse, fcntl, json, mmap, os, struct, sys, time

ANE_BASE = 0x284000000          # ADT ane0 range0 (t6021, +0x200000000 translation)
ANE_SIZE = 0x2000000
PMGR_BASE = 0x28E080000         # pmgr1,t6021
PMGR_SPAN = 0x8000
ENGINE_KILL = (0x285C04000, 0x285C28000)   # hard-excluded window

# live-DT power-controller@<off> nodes under power-management@28e080000,
# parent-first; values: (name, pmgr offset)
PS_CHAIN = [
    ("ane_sys_mpm", 0x4000),
    ("ane_td",      0x4008),
    ("ane_base",    0x4010),
    ("ane_set1",    0x4018),
    ("ane_set2",    0x4020),
    ("ane_set3",    0x4028),
    ("ane_set4",    0x4030),
    ("ane_cpu",     0x2E0),     # the ASC cpu itself - raised last
]

# apple-pmgr-pwrstate.c register fields
PS_TARGET_MASK = 0xF
PS_ACTUAL_MASK = 0xF0
PS_CLEAR = (1 << 31) | (1 << 28) | (0xF << 24) | (0xF << 16) | (1 << 12) | (1 << 10) | PS_TARGET_MASK
PS_ACTIVE = 0xF

def log(ev, **kw):
    line = {"t": round(time.time(), 3), "ev": ev}
    line.update(kw)
    print(json.dumps(line), flush=True)

def check(addr):
    lo, hi = ENGINE_KILL
    assert not (lo <= addr < hi), f"refusing address in kill window: {addr:#x}"

class DevMem:
    def __init__(self):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.maps = {}

    def window(self, base, size):
        key = (base, size)
        if key not in self.maps:
            page = 0x1000
            b = base & ~(page - 1)
            e = (base + size + page - 1) & ~(page - 1)
            m = mmap.mmap(self.fd, e - b, mmap.MAP_SHARED,
                          mmap.PROT_READ | mmap.PROT_WRITE, offset=b)
            self.maps[key] = (m, b, base - b)
        m, b, delta = self.maps[key]
        return m, delta

    def rd32(self, addr):
        check(addr)
        for (base, size) in list(self.maps):
            m, b, delta = self.maps[base, size]
            if base <= addr < base + size:
                return struct.unpack_from("<I", m, delta + (addr - base))[0]
        raise KeyError(f"address {addr:#x} outside mapped windows")

    def wr32(self, addr, val):
        check(addr)
        for (base, size) in list(self.maps):
            m, b, delta = self.maps[base, size]
            if base <= addr < base + size:
                struct.pack_into("<I", m, delta + (addr - base), val)
                return
        raise KeyError(f"address {addr:#x} outside mapped windows")

def stage0(d):
    m, delta = d.window(PMGR_BASE, PMGR_SPAN)
    for name, off in PS_CHAIN:
        reg = PMGR_BASE + off
        v = d.rd32(reg)
        log("ps.snapshot", name=name, addr=f"{reg:#x}", val=f"{v:#010x}",
            target=v & PS_TARGET_MASK, actual=(v & PS_ACTUAL_MASK) >> 4)

def stage1(d):
    m, delta = d.window(PMGR_BASE, PMGR_SPAN)
    for name, off in PS_CHAIN:
        reg = PMGR_BASE + off
        v = d.rd32(reg)
        nv = (v & ~PS_CLEAR) | PS_ACTIVE
        log("ps.raise", name=name, addr=f"{reg:#x}", old=f"{v:#010x}", new=f"{nv:#010x}")
        d.wr32(reg, nv)
        deadline = time.time() + 0.5
        while time.time() < deadline:
            cur = d.rd32(reg)
            if (cur & PS_ACTUAL_MASK) >> 4 == PS_ACTIVE and (cur & 0x800) == 0:
                break
            time.sleep(0.001)
        else:
            log("ps.raise.TIMEOUT", name=name, val=f"{cur:#010x}")
            sys.exit(2)
        log("ps.raise.ok", name=name, val=f"{cur:#010x}")

# stage 2: the H14-kext-proven runtime register set (all reads)
S2_REGS = [
    ("VERS",             ANE_BASE + 0x1840000),
    ("RTB_UNK7C",        ANE_BASE + 0x184007C),
    ("RTB_STATUS88",     ANE_BASE + 0x1840088),
] + [(f"GPIO{i}", ANE_BASE + 0x1840048 + 4 * i) for i in range(8)] + [
    ("ASC_IO_RVBAR",     ANE_BASE + 0x1050000),
    ("ASC_EDPRCR",       ANE_BASE + 0x1010310),
    ("CPU_CONTROL",      ANE_BASE + 0x1000044),
    ("CPU_STATUS",       ANE_BASE + 0x1000048),
]

def stage2(d):
    d.window(ANE_BASE, ANE_SIZE)
    for name, addr in S2_REGS:
        v = d.rd32(addr)
        log("ane.read", name=name, addr=f"{addr:#x}", val=f"{v:#010x}")

# stage 3: mailbox candidates, read-only.  Asahi mailbox layouts (mailbox.c):
#   ASC-style (m1n1 ASCRegs, mailbox base = block + 0x8000):
#     A2I_CTRL +0x110, I2A_CTRL +0x114, A2I send0/1 +0x800/+0x808,
#     I2A recv0/1 +0x830/+0x838
#   M3-style: IRQ_EN +0x48, A2I_CTRL +0x50, A2I send0/1 +0x60/+0x68,
#     I2A_CTRL +0x80, I2A recv0/1 +0xA0/+0xA8
#   ctrl word: FULL=bit16 EMPTY=bit17, RPTR/WPTR nibbles 15..8
S3_SETS = [
    ("asc@+8000", [0x8110, 0x8114, 0x8800, 0x8808, 0x8830, 0x8838]),
    ("m3@+4000",  [0x4048, 0x4050, 0x4060, 0x4068, 0x4080, 0x4090, 0x40A0, 0x40A8]),
    ("m3@rtbuddy", [0x1840000 + o for o in (0x40, 0x48, 0x50, 0x60, 0x68, 0x80, 0x90, 0xA0, 0xA8)]),
]

def stage3(d):
    d.window(ANE_BASE, ANE_SIZE)
    for label, offs in S3_SETS:
        for o in offs:
            addr = ANE_BASE + o
            v = d.rd32(addr)
            log("mbox.probe", set=label, addr=f"{addr:#x}", val=f"{v:#010x}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", type=int, default=0,
                    help="run stages 0..N inclusive (default 0 = snapshot only)")
    a = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("must run as root (/dev/mem)")
    d = DevMem()
    for s in range(a.stage + 1):
        log("stage.begin", stage=s)
        {0: stage0, 1: stage1, 2: stage2, 3: stage3}[s](d)
        log("stage.done", stage=s)

if __name__ == "__main__":
    main()
