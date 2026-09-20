#!/usr/bin/env python3
"""h16_hybrid_boot — userspace T6021 boot release (W16).

Owns the host-write-fatal engine writes the kernel must not touch
(kernel P-1a eng+0x000 wedged jw14m2 into watchdog reset 2026-09-20
17:41:56 -> 17:43:03 boot; netconsole). Mirrors ane_t6021_boot.h
run_sequence verbatim, modes: P0 SKIPPED (diagnostic, the loaded-module
precedent), P-1 grant tunables, S1 scratch clear+pulse, S2 RVBAR
skip-if-latched, S3 CPU release 0 -> 0x10, poll A for FRESH SCRATCH7
READY 0x08042006. STOPS at READY: publication (S5+) waits on the still-
open init-structure prerequisites (boot.c items 4/5) and is NOT faked.

Prerequisite: ane_t6021 loaded with fw_load=1 and the W16 entry alias
confirmed (tools/t6021_dart_walk.py reports entry region 320/320
mapped). Run as root on jw14m2.

Usage: sudo python3 h16_hybrid_boot.py [--polls N] [--dry-run]
"""
import argparse
import json
import mmap
import os
import struct
import sys
import time

ANE_BASE = 0x284000000
ENGINE_KILL = (0x285C04000, 0x285C28000)

# ane_t6021_boot.h
REG_TABLE = (0x00000B38, 0x00000B98, 0x00000BF8)
REG_RVBAR = 0x01050000
REG_CPUCTRL = 0x01400044
REG_CPUSTATUS = 0x01400048
REG_SCRATCH0 = 0x01840048
REG_SCRATCH6 = 0x01840060
REG_SCRATCH7 = 0x01840064
TABLE_VALUE = 0x01FF01FF
CPU_RUN_RELEASE = 0x10
BOOT_ACK = 0x08042006
WAKE_REQ = 0xF7FBDFF9

# P-1 W8 grant tunables (ane_t6021_boot.h run_sequence, w8-run.out
# APERTURE_UNLOCKED — replayed verbatim)
P1_TUNABLES = [
    (0x000, 0x00000010), (0x038, 0x00050020),
    (0x03C, 0x000A0030), (0x400, 0x40010001),
    (0x600, 0x01FFFFFF), (0x738, 0x00200020),
    (0x798, 0x00100030), (0x7F8, 0x0100000A),
    (0x900, 0x00000101), (0x410, 0x00001100),
    (0x420, 0x00001100), (0x430, 0x00001100),
]

ENTRY_IOVA = 0x10000000000
RVBAR_ENTRY_MASK = 0xFF7EFFFFFFFFF800

assert (0x000 << 40) | ENTRY_IOVA == ENTRY_IOVA == (1 << 40)
assert (0x284000000 + REG_SCRATCH7) == 0x285840064
assert all(0x284000000 <= ANE_BASE + off < 0x285C04000
           for off, _ in P1_TUNABLES), "tunable outside engine block"


def log(ev, **kw):
    print(json.dumps({"t": round(time.time(), 3), "ev": ev, **kw}), flush=True)


def check(addr):
    lo, hi = ENGINE_KILL
    assert not (lo <= addr < hi), f"refusing address in kill window: {addr:#x}"


class DevMem:
    PAGE = 0x1000

    def __init__(self):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.maps = {}

    def window(self, base, size):
        key = (base, size)
        if key not in self.maps:
            b = base & ~(self.PAGE - 1)
            e = (base + size + self.PAGE - 1) & ~(self.PAGE - 1)
            m = mmap.mmap(self.fd, e - b, mmap.MAP_SHARED,
                          mmap.PROT_READ | mmap.PROT_WRITE, offset=b)
            self.maps[key] = (m, b, base - b)
        m, b, delta = self.maps[key]
        return m, delta

    def rd32(self, addr):
        check(addr)
        for (base, _size) in list(self.maps):
            m, b, delta = self.maps[base, _size]
            if base <= addr < base + _size:
                return struct.unpack_from("<I", m, delta + (addr - base))[0]
        raise KeyError(f"address {addr:#x} outside mapped windows")

    def wr32(self, addr, val):
        check(addr)
        for (base, _size) in list(self.maps):
            m, b, delta = self.maps[base, _size]
            if base <= addr < base + _size:
                struct.pack_into("<I", m, delta + (addr - base), val)
                return
        raise KeyError(f"address {addr:#x} outside mapped windows")

    def rd64(self, addr):
        return self.rd32(addr) | (self.rd32(addr + 4) << 32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--polls", type=int, default=15000,
                    help="poll A iterations (1 ms cadence)")
    ap.add_argument("--dry-run", action="store_true",
                    help="read state, run P-1/S1 writes only, no CPU release")
    args = ap.parse_args()

    d = DevMem()
    d.window(ANE_BASE, 0x2000000)  # 32 MiB engine block

    # marker rides netconsole for run discrimination
    os.system("echo 'ANE-M2-W16-HYBRID-%d entry-alias boot release' > /dev/kmsg"
              % int(time.time()))

    cpu_status = d.rd32(ANE_BASE + REG_CPUSTATUS)
    rvbar = d.rd64(ANE_BASE + REG_RVBAR)
    s7 = d.rd32(ANE_BASE + REG_SCRATCH7)
    log("prestate", cpu_status=f"{cpu_status:#010x}",
        rvbar=f"{rvbar:#x}", scratch7=f"{s7:#010x}")
    assert rvbar & 1, "RVBAR bit0 not latched — lawful skip branch invalid"
    entry_bits = rvbar & RVBAR_ENTRY_MASK
    assert entry_bits == ENTRY_IOVA, \
        f"entry bits {entry_bits:#x} != expected {ENTRY_IOVA:#x} (alias target)"

    # P0: table writes SKIPPED (diagnostic mode — loaded-module precedent;
    # REQUIRED only under the full kext pre-CPU config, case 1)
    log("P0", mode="skipped-diagnostic")

    # P-1: W8 grant tunables, per-write phase discrimination
    log("P-1.begin", writes=len(P1_TUNABLES))
    for i, (off, val) in enumerate(P1_TUNABLES):
        log("P-1.write", i=i, off=f"{off:#x}", val=f"{val:#010x}")
        if not args.dry_run:
            d.wr32(ANE_BASE + off, val)
        log("P-1.done", i=i)
    log("P-1.end")

    # S1: InitANEScratchRegisters — clear all 8, SCRATCH6=1, pulse 7 1->0
    log("S1.scratch-clear-pulse")
    if not args.dry_run:
        for i in range(8):
            d.wr32(ANE_BASE + REG_SCRATCH0 + 4 * i, 0)
        d.wr32(ANE_BASE + REG_SCRATCH6, 1)
        d.wr32(ANE_BASE + REG_SCRATCH7, 1)
        d.wr32(ANE_BASE + REG_SCRATCH7, 0)
    log("S1.done",
        s6=f"{d.rd32(ANE_BASE + REG_SCRATCH6):#010x}",
        s7=f"{d.rd32(ANE_BASE + REG_SCRATCH7):#010x}")

    if args.dry_run:
        log("dry-run.stop")
        return 0

    # S2: RVBAR skip-if-latched (bit0 set — no write, no reset; the W16
    # entry alias makes this branch survivable)
    log("S2.rvbar", branch="skip-latched", entry=f"{entry_bits:#x}")

    # S3: CPU release — strictly 0 then 0x10
    log("S3.cpu-release")
    d.wr32(ANE_BASE + REG_CPUCTRL, 0)
    d.wr32(ANE_BASE + REG_CPUCTRL, CPU_RUN_RELEASE)
    log("S3.released",
        cputrl=f"{d.rd32(ANE_BASE + REG_CPUCTRL):#010x}",
        cpu_status=f"{d.rd32(ANE_BASE + REG_CPUSTATUS):#010x}")

    # S4: poll A — FRESH SCRATCH7 READY (the S1 pulse made it unambiguous)
    deadline = args.polls
    got = None
    for i in range(deadline):
        v = d.rd32(ANE_BASE + REG_SCRATCH7)
        if v == BOOT_ACK:
            got = i
            break
        if v not in (0, BOOT_ACK) and (i % 500) == 499:
            log("pollA.progress", i=i, s7=f"{v:#010x}")
        time.sleep(0.001)

    if got is None:
        log("pollA.TIMEOUT", polls=deadline,
            s7=f"{d.rd32(ANE_BASE + REG_SCRATCH7):#010x}",
            cpu_status=f"{d.rd32(ANE_BASE + REG_CPUSTATUS):#010x}",
            note="fw did not reach READY — check DART FAULT + walk tool")
        return 2

    log("pollA.READY", poll=got, s7=f"{BOOT_ACK:#010x}",
        cpu_status=f"{d.rd32(ANE_BASE + REG_CPUSTATUS):#010x}",
        scratch=(
            f"{d.rd32(ANE_BASE + REG_SCRATCH1):#010x}"
            f"{d.rd32(ANE_BASE + REG_SCRATCH0):#010x}"),
        note="fw ALIVE — publication (S5+) gated on init-structure "
             "prerequisites, intentionally not attempted")
    return 0


if __name__ == "__main__":
    sys.exit(main())
