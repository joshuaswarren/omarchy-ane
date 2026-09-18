#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""H14 (t6021) ANE RTKit first exchange — single-shot, minimal-write.

Preconditions (checked, never forced):
  - ane_cpu pmgr domain already powered (boot firmware leaves it on)
  - fw already booted by iBoot (RVBAR valid bit set)

Protocol per Asahi rtkit.c (RTKit v11/12):
  EP_MGMT=0, EP_CRASHLOG=1, EP_SYSLOG=2, EP_DEBUG=3, EP_IOREPORT=4, EP_OSLOG=8
  MGMT msg types (bits 59..52): HELLO=1 HELLO_REPLY=2 STARTEP=5
    SET_IOP_PWR_STATE=6 SET_IOP_PWR_STATE_ACK=7 EPMAP=8 SET_AP_PWR_STATE=0xb
  HELLO carries min ver [15:0], max ver [31:16]; host replies with chosen ver.
  EPMAP: base [34:32], bitmap [31:0], LAST bit 51; host echoes base+LAST/MORE.
  Buffer request: type(bits 59..52)=1, size pages [51:44], iova [43:0].

Writes performed: only RTKit replies on the A2I mailbox (what the handshake
requires).  No pmgr writes, no RVBAR writes, no register writes otherwise.
"""
import argparse, os, mmap, struct, sys, time

ANE = 0x284000000
ASC = 0x1600000                      # H14 ASC cpu block (kext 0x1600044 control)
MBOX = ASC + 0x8000                  # m1n1 ASCRegs mailbox layout
A2I_CTRL, I2A_CTRL = MBOX + 0x110, MBOX + 0x114
A2I_S0, A2I_S1 = MBOX + 0x800, MBOX + 0x808
I2A_R0, I2A_R1 = MBOX + 0x830, MBOX + 0x838

EP_MGMT, EP_CRASHLOG, EP_SYSLOG, EP_DEBUG, EP_IOREPORT, EP_OSLOG = 0, 1, 2, 3, 4, 8
MGMT_TYPE = 0xFF << 52
M_HELLO, M_HELLO_REPLY, M_STARTEP = 1, 2, 5
M_EPMAP, M_EPMAP_LAST, M_EPMAP_MORE = 8, 1 << 51, 1 << 0
RTKIT_MIN, RTKIT_MAX = 11, 12

ENGINE_KILL = (0x285C04000, 0x285C28000)

def check(addr):
    assert not (ENGINE_KILL[0] <= addr < ENGINE_KILL[1]), f"kill window: {addr:#x}"

class MM:
    def __init__(self):
        self.fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
        self.maps = {}
    def map(self, base, size):
        if base not in self.maps:
            b = base & ~0xFFF
            e = (base + size + 0xFFF) & ~0xFFF
            self.maps[base] = (mmap.mmap(self.fd, e - b, mmap.MAP_SHARED,
                                mmap.PROT_READ | mmap.PROT_WRITE, offset=b), b)
        return self.maps[base]
    def rd32(self, addr):
        check(addr)
        m, b = self.map(addr & ~0xFFF, 0x2000)
        return struct.unpack_from("<I", m, addr - b)[0]
    def wr32(self, addr, val):
        check(addr)
        m, b = self.map(addr & ~0xFFF, 0x2000)
        struct.pack_into("<I", m, addr - b, val)

def log(ev, **kw):
    import json
    print(json.dumps({"ev": ev, **kw}), flush=True)

def recv(mm, timeout=5.0):
    """wait for one I2A message -> (msg0, ep) or None"""
    end = time.time() + timeout
    while time.time() < end:
        c = mm.rd32(I2A_CTRL)
        if not (c >> 17) & 1:            # not empty
            msg0 = mm.rd32(I2A_R0)
            ep = mm.rd32(I2A_R1) & 0xFF
            return msg0, ep
        time.sleep(0.001)
    return None

def send(mm, ep, msg0):
    mm.wr32(A2I_S0, msg0)
    mm.wr32(A2I_S1, ep & 0xFF)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deadline", type=float, default=20.0)
    ap.add_argument("--dry", action="store_true", help="receive only, send nothing")
    a = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("run as root")
    mm = MM()
    # preconditions
    ps = mm.rd32(0x28E080000 + 0x2E0)
    log("pre.ane_cpu", val=f"{ps:#x}", on=bool(ps & 0xF == 0xF))
    if (ps & 0xF) != 0xF:
        sys.exit("ane_cpu off; refusing to raise (read-only mode)")
    rv = mm.rd32(ANE + 0x1600044)
    log("pre.cpu_control", val=f"{rv:#x}", run=bool(rv & 0x10))
    log("pre.i2a_ctrl", val=f"{mm.rd32(I2A_CTRL):#x}")
    log("pre.a2i_ctrl", val=f"{mm.rd32(A2I_CTRL):#x}")

    end = time.time() + a.deadline
    while time.time() < end:
        r = recv(mm, timeout=1.0)
        if r is None:
            continue
        msg0, ep = r
        log("rx", ep=f"{ep:#x}", msg=f"{msg0:#018x}")
        if a.dry:
            continue
        if ep == EP_MGMT and (msg0 & MGMT_TYPE) >> 52 == M_HELLO:
            mn, mx = msg0 & 0xFFFF, (msg0 >> 16) & 0xFFFF
            want = min(RTKIT_MAX, mx)
            log("hello", min_ver=mn, max_ver=mx, want=want)
            send(mm, EP_MGMT, (want & 0xFFFF) | (want << 16) | (2 << 52))
            log("tx", ep="0x0", type="HELLO_REPLY")
        elif ep == EP_MGMT and (msg0 & MGMT_TYPE) >> 52 == M_EPMAP:
            base = (msg0 >> 32) & 0x7
            bitmap = msg0 & 0xFFFFFFFF
            last = bool(msg0 & M_EPMAP_LAST)
            log("epmap", base=base, bitmap=f"{bitmap:#x}", last=last)
            reply = base << 32
            reply |= M_EPMAP_LAST if last else M_EPMAP_MORE
            send(mm, EP_MGMT, (8 << 52) | reply)
            log("tx", ep="0x0", type="EPMAP_REPLY")
        else:
            log("note", text="non-mgmt/other message; logged only")
    log("done")

if __name__ == "__main__":
    main()
