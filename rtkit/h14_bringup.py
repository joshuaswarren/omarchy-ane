#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""H14 (t6021) ANE RTKit/ASC bring-up — userland, /dev/mem, staged.

THE RULE (device-proven, 2026-09-18/19):
  Every t6021 ANE-block access that ran WITHOUT the full eight-word pmgr
  chain raised ended in a machine reset or a hang:
    - W3 kernel bind: six-domain genpd chain -> first block read
      (RVBAR, eng+0x1050000 = 0x285050000) froze the box, watchdog reset
      ~60 s later (receipts/2026-09-19-h14-w3-live-probe.md).
    - phase1 zero-write probe: ane_cpu only (other seven gated) -> read of
      the +0x1600000 cpu-control/mailbox family hung (receipt
      2026-09-18-h14-rtkit-port-phase1.md §3).
  The one session that raised ALL EIGHT words parent-first (phase1 S1,
  RMW target=0xf, poll ACTUAL) read the whole ASC status set cleanly
  (RVBAR=0x1 valid, VERS=0xe3044, RTB status=0x1, GPIO acks 0, EDPRCR 0).
  This script therefore hard-gates every block read behind the completed
  eight-word raise and refuses any address outside the phase1-proven set.

Kext-derived context (AppleH11ANEInterface 10.19.2, T6021):
  iBoot boots selene before Linux (RVBAR bit0 = 1), so the kext's runtime
  path performs NO Chinook bootup: no RVBAR write, no CPU_CONTROL 0->0x10,
  no SCRATCH0-7 boot args. Power is IOService/PMGR platform work, not kext
  MMIO. This script reproduces the power-up (the eight ps words; the ADT
  additionally names clock-gate 473 "ANE-SYS-V" + clock-ids 318-321 for
  ane0, which Linux does not model — see receipt, open item) and then
  reads exactly the status surface the kext itself uses at runtime.

Stages (run in order; --stage N runs 0..N):
  0  snapshot: pmgr ane-chain PS register values (read-only)
  1  power: raise the eight ANE ps words parent-first, phase1 RMW
     semantics (clear AUTO_ENABLE/flag bits, target 0xf, poll ACTUAL)
  2  status: flushed reads of the phase1-proven ASC status set

Never in any stage:
  - reads in +0x1600000..+0x1700000 (kext writes it only on cold boot;
    never reads it at runtime; one hang association)
  - reads/writes in +0x1c04000..+0x1c28000 (old H13 TM window, two
    netconsole-named external aborts)
  - any address outside the whitelist below (no "just probe one offset")

Opt-in, refused by default, never part of the ladder:
  --set-raise  write 0xf to the SET-window ps words (+0xc000+0x00..0x30).
               m1n1's t8103 power_up() does exactly this before any
               engine access, and t6021 has never had it done (W3 read
               set+0 = 0x00000000). BUT ps-word writes in this window are
               also the write class that hard-reset t6001 kernel-side
               (receipt 2026-09-16-tm-recovery-bisect.md) — run only on
               purpose, netconsole armed, never as a first write.

Discipline (2026-09-18 lane rules):
  - first anomaly = stop; netconsole + this log are the capture
  - every read prints its address BEFORE the access and fsyncs, so a
    freeze pins the exact address in the netconsole/log seam

Run on jw14m2-linux as root:  python3 h14_bringup.py --stage 2
"""
import argparse, fcntl, json, mmap, os, struct, sys, time

ANE_BASE = 0x284000000          # ADT ane0 range0 (t6021, +0x200000000 translation)
ANE_SIZE = 0x2000000
PMGR_BASE = 0x28E080000         # pmgr1,t6021
PMGR_SPAN = 0x8000
SET_BASE = 0x28E08C000          # ADT ane0 range2 (the +0xc000 ps window)
ENGINE_KILL = (0x285C04000, 0x285C28000)   # hard-excluded window (W2 kills)
CPUCTL_BLOCK = (0x285600000, 0x285700000)  # +0x1600000 family: no reads ever
                                           # (cpu ctl +0x44/+0x48, mailbox
                                           # +0x1608xxx; VERS +0x1840000 is
                                           # ABOVE this and whitelisted)

# live-DT power-controller@<off> nodes under power-management@28e080000,
# parent-first; values: (name, pmgr offset).  ALL EIGHT are mandatory
# before any block access — this is the phase1-proven raise set.
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
PS_AUTO_ENABLE = 1 << 28
PS_CLEAR = (1 << 31) | (1 << 28) | (0xF << 24) | (0xF << 16) | (1 << 12) | (1 << 10) | PS_TARGET_MASK
PS_ACTIVE = 0xF

# Whitelist of readable addresses inside the ANE block: exactly the
# phase1-proven status set (kext runtime surface: RVBAR, VERS, RTBuddy
# status + unk 7c, GPIO acks 0-7, EDPRCR).  rd32 refuses everything else.
READ_WHITELIST = (
    [ANE_BASE + 0x1050000]                      # ASC_IO_RVBAR
    + [ANE_BASE + 0x1010310]                    # ASC_EDPRCR
    + [ANE_BASE + 0x1840000]                    # VERS
    + [ANE_BASE + 0x184007C]                    # RTBuddy unk 7c
    + [ANE_BASE + 0x1840088]                    # RTBuddy status (kext polls < 2)
    + [ANE_BASE + 0x1840048 + 4 * i for i in range(8)]  # GPIO acks 0-7
)

def log(ev, **kw):
    line = {"t": round(time.time(), 3), "ev": ev}
    line.update(kw)
    print(json.dumps(line), flush=True)
    sys.stdout.flush()
    os.fsync(sys.stdout.fileno())

def in_kill(addr):
    lo, hi = ENGINE_KILL
    if lo <= addr < hi:
        return "old TM kill window"
    lo, hi = CPUCTL_BLOCK
    if lo <= addr < hi:
        return "+0x1600000 cpuctl/mailbox block (read-fatal association)"
    return None

def check(addr, whitelist=False):
    why = in_kill(addr)
    assert not why, f"refusing address in {why}: {addr:#x}"
    if whitelist:
        assert addr in READ_WHITELIST, f"address {addr:#x} not in read whitelist"

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

    def _locate(self, addr):
        for (base, size) in list(self.maps):
            if base <= addr < base + size:
                m, b, delta = self.maps[base, size]
                return m, delta + (addr - base)
        raise KeyError(f"address {addr:#x} outside mapped windows")

    def rd32(self, addr):
        check(addr, whitelist=True)
        m, off = self._locate(addr)
        return struct.unpack_from("<I", m, off)[0]

    def wr32(self, addr, val):
        check(addr)
        m, off = self._locate(addr)
        struct.pack_into("<I", m, off, val)

def stage0(d):
    d.window(PMGR_BASE, PMGR_SPAN)
    for name, off in PS_CHAIN:
        reg = PMGR_BASE + off
        v = _pmgr_rd(d, reg)
        log("ps.snapshot", name=name, addr=f"{reg:#x}", val=f"{v:#010x}",
            target=v & PS_TARGET_MASK, actual=(v & PS_ACTUAL_MASK) >> 4,
            auto_enable=bool(v & PS_AUTO_ENABLE))

def _pmgr_rd(d, addr):
    # pmgr words are not in the ANE-block whitelist; read via direct map
    m, off = d._locate(addr)
    return struct.unpack_from("<I", m, off)[0]

def stage1(d):
    d.window(PMGR_BASE, PMGR_SPAN)
    for name, off in PS_CHAIN:
        reg = PMGR_BASE + off
        v = _pmgr_rd(d, reg)
        nv = (v & ~PS_CLEAR) | PS_ACTIVE
        log("ps.raise", name=name, addr=f"{reg:#x}", old=f"{v:#010x}", new=f"{nv:#010x}")
        d.wr32(reg, nv)
        deadline = time.time() + 0.5
        while time.time() < deadline:
            cur = _pmgr_rd(d, reg)
            if (cur & PS_ACTUAL_MASK) >> 4 == PS_ACTIVE and (cur & 0x800) == 0:
                break
            time.sleep(0.001)
        else:
            log("ps.raise.TIMEOUT", name=name, val=f"{cur:#010x}")
            sys.exit(2)
        log("ps.raise.ok", name=name, val=f"{cur:#010x}")
    # the phase1 end state: all eight ACTUAL=0xf AND ane_cpu AUTO_ENABLE
    # cleared by the RMW.  W3's kernel run left bit 28 set on ane_cpu
    # (0x1f0003ff) and died at the first block read; refuse to proceed
    # unless the cpu word matches the proven shape.
    cpu = _pmgr_rd(d, PMGR_BASE + 0x2E0)
    log("ps.cpu_word", val=f"{cpu:#010x}",
        auto_enable=bool(cpu & PS_AUTO_ENABLE))
    if cpu & PS_AUTO_ENABLE:
        log("FATAL", why="ane_cpu AUTO_ENABLE still set after raise; "
                         "refusing block access (W3 death discriminator)")
        sys.exit(3)

def stage2(d):
    d.window(ANE_BASE, ANE_SIZE)
    for addr in READ_WHITELIST:
        log("ane.reading", addr=f"{addr:#x}")   # flushed BEFORE the access
        v = d.rd32(addr)
        log("ane.read", addr=f"{addr:#x}", val=f"{v:#010x}")
    rvbar = d.rd32(ANE_BASE + 0x1050000)
    log("verdict", rvbar_valid=bool(rvbar & 1),
        fw_entry=f"{rvbar & ~1:#x}",
        note="bit0=1: selene released by iBoot; no cold-boot path in this tool")

def set_raise(d):
    """Opt-in (--set-raise): m1n1 t8103 power_up() analog — SET-window ps
    words 0x00..0x30 -> 0xf.  t6001 kernel writes to this window class
    hard-reset the box; never run this without netconsole armed and never
    as the first write of a session."""
    d.window(SET_BASE, 0x4000)
    for off in range(0x0, 0x30 + 0x8, 0x8):
        reg = SET_BASE + off
        v = _pmgr_rd(d, reg)
        log("set.raise", addr=f"{reg:#x}", old=f"{v:#010x}", new="0x0000000f")
        d.wr32(reg, 0xF)
        deadline = time.time() + 0.5
        while time.time() < deadline:
            cur = _pmgr_rd(d, reg)
            if (cur & PS_ACTUAL_MASK) >> 4 == PS_ACTIVE:
                break
            time.sleep(0.001)
        log("set.raise.done", addr=f"{reg:#x}", val=f"{cur:#010x}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", type=int, default=2,
                    help="run stages 0..N inclusive (default 2 = full safe ladder)")
    ap.add_argument("--set-raise", action="store_true",
                    help="DANGEROUS: also write SET-window ps words 0xf "
                         "(m1n1 t8103 analog; t6001-fatal write class)")
    a = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("must run as root (/dev/mem)")
    if a.set_raise and a.stage < 1:
        sys.exit("--set-raise requires the stage-1 raise to run first (--stage >= 1)")
    d = DevMem()
    stages = {0: stage0, 1: stage1, 2: stage2}
    for s in range(a.stage + 1):
        log("stage.begin", stage=s)
        stages[s](d)
        log("stage.done", stage=s)
    if a.set_raise:
        log("stage.begin", stage="set-raise (opt-in)")
        set_raise(d)
        log("stage.done", stage="set-raise (opt-in)")

if __name__ == "__main__":
    main()
