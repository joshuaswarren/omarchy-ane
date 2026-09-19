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

W2 decode adds (receipts/2026-09-18-h14-w2-protocol-decode.md, static-only):
  - ANE app endpoints are 1..6 (K14 kext InitializeRTBuddyEndpoints opens
    exactly these via RTBuddyService; per-EP config table __DATA_CONST.__const
    +0x814e520, 40 B/entry): ep1 "INIT" 64K, ep2 "T2FC" 256K, ep3 "T2FH" 256K,
    ep4 "T2HS" 64K, ep5 "T2HC" 128K, ep6 "T2HT" 64K (ep0 = empty slot).
    Expect EPMAP to announce at least the fw-created subset of 1..6; answer
    MORE/LAST for each accordingly.
  - App-level ring doorbell word (NOT MGMT): u64 =
    offset[43:0] | size_code[51:44] | unit[53:52]
    (unit: 0=bytes, 1=*4K, 2=*1M, 3=*2M; K14 HandleRTBuddyMessage decode at
    0x...95feff0 and SetupEndpoints packer at 0x...95fe660 agree).
    Ring cursors live in the per-EP record (this+0x5c0 + ep*0x40: +0x08 size,
    +0x18 endpoint obj, +0x20 u32 write offset, +0x28 command gate).
  - ep2/ep3 (T2FC/T2FH) are the fw->host command channels the kext delivers
    to processTargetToHostIOCommand; ep6 is polled via receiveMessage in
    drainRtbuddyEndpointQueues. CSNE_CMD ids: see fw id->name table at selene
    vaddr 0xea430 (16 B/entry {u16 id, u32 name-ptr, u32 tag}); INFERENCE_CALL
    = 0x404, PROCEDURE_CALL = 0x204, IPC_ENDPOINT_SET = 0x15, BOOT = 0x10.

Writes performed: RTKit MGMT replies on the A2I mailbox only — HELLO_REPLY,
EPMAP replies, STARTEP (only endpoints the fw itself announced: rtkit.c
system set 1/2/3/4/8/0xa + the kext-evidenced app set 1..6), SET_IOP_PWR_STATE_ACK
(echoes the fw's state), and — with --set-ap-on — SET_AP_PWR_STATE ON (rtkit.c
boot()).  No pmgr writes, no RVBAR writes, no register writes otherwise.
"""
import argparse, os, mmap, struct, sys, time

ANE = 0x284000000
ASC = 0x1600000                      # H14 ASC cpu block (kext 0x1600044 control)
MBOX = ASC + 0x8000                  # m1n1 ASCRegs mailbox layout
A2I_CTRL, I2A_CTRL = MBOX + 0x110, MBOX + 0x114
A2I_S0, A2I_S1 = MBOX + 0x800, MBOX + 0x808
I2A_R0, I2A_R1 = MBOX + 0x830, MBOX + 0x838

EP_MGMT, EP_CRASHLOG, EP_SYSLOG, EP_DEBUG, EP_IOREPORT, EP_OSLOG = 0, 1, 2, 3, 4, 8
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
    """wait for one I2A message -> (msg0, ep) or None.
    OUTBOX0 (+0x830) is Register64 (m1n1 ASCRegs): msg0 needs both halves —
    the MGMT type field lives at bits 59..52, i.e. in the HIGH half."""
    end = time.time() + timeout
    while time.time() < end:
        c = mm.rd32(I2A_CTRL)
        if not (c >> 17) & 1:            # not empty
            msg0 = mm.rd32(I2A_R0) | (mm.rd32(I2A_R0 + 4) << 32)
            ep = mm.rd32(I2A_R1) & 0xFF
            return msg0, ep
        time.sleep(0.001)
    return None

def send(mm, ep, msg0):
    """write one A2I message; m1n1 Mbox.send waits on INBOX_CTRL.FULL
    around the slot writes (1-deep FIFO — never overwrite in-flight)."""
    while (mm.rd32(A2I_CTRL) >> 16) & 1:
        time.sleep(0.001)
    mm.wr32(A2I_S0, msg0 & 0xFFFFFFFF)
    mm.wr32(A2I_S0 + 4, (msg0 >> 32) & 0xFFFFFFFF)
    mm.wr32(A2I_S1, ep & 0xFF)
    while (mm.rd32(A2I_CTRL) >> 16) & 1:
        time.sleep(0.001)

M_SET_IOP_PWR_STATE, M_SET_IOP_PWR_STATE_ACK = 6, 7
M_SET_AP_PWR_STATE, M_SET_AP_PWR_STATE_ACK = 0xB, 0xB
PWR_STATE_ON = 0x20
MGMT_STARTEP_FLAG = 1 << 1          # STARTEP ep at bits 39:32, FLAG bit 1
SYS_EPS = (1, 2, 3, 4, 8, 0xA)      # rtkit.c: crashlog/syslog/debug/ioreport/oslog/tracekit
APP_EPS = (1, 2, 3, 4, 5, 6)        # W2 decode: kext InitializeRTBuddyEndpoints opens 1..6

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deadline", type=float, default=20.0)
    ap.add_argument("--dry", action="store_true", help="receive only, send nothing")
    ap.add_argument("--set-ap-on", action="store_true",
                    help="drive AP power state to ON after IOP ack (kext-equivalent attach)")
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
    announced = set()
    started_sys = started_app = False
    while time.time() < end:
        r = recv(mm, timeout=1.0)
        if r is None:
            continue
        msg0, ep = r
        t = (msg0 >> 52) & 0xFF
        log("rx", ep=f"{ep:#x}", msg=f"{msg0:#018x}", type=f"{t:#x}")
        if a.dry:
            continue
        if ep != EP_MGMT:
            if 1 <= ep <= 6:            # app doorbell: capture only, never ack
                off = msg0 & ((1 << 44) - 1)
                size = (msg0 >> 44) & 0xFF
                unit = (msg0 >> 52) & 3
                log("app_doorbell", ep=f"{ep:#x}", offset=f"{off:#x}",
                    size_code=size, unit=unit,
                    ring_size={1:0x10000,2:0x40000,3:0x40000,4:0x10000,
                               5:0x20000,6:0x10000}.get(ep))
            continue
        if t == M_HELLO:
            mn, mx = msg0 & 0xFFFF, (msg0 >> 16) & 0xFFFF
            want = min(RTKIT_MAX, mx)
            log("hello", min_ver=mn, max_ver=mx, want=want)
            send(mm, EP_MGMT, (M_HELLO_REPLY << 52) | (want << 16) | want)
            log("tx", ep="0x0", type="HELLO_REPLY")
        elif t == M_EPMAP:
            base = (msg0 >> 32) & 0x7
            bitmap = msg0 & 0xFFFFFFFF
            last = bool(msg0 & M_EPMAP_LAST)
            announced |= {32 * base + i for i in range(32) if bitmap >> i & 1}
            log("epmap", base=base, bitmap=f"{bitmap:#x}", last=last,
                announced=sorted(f"{e:#x}" for e in announced))
            send(mm, EP_MGMT, (M_EPMAP << 52) | (base << 32)
                 | (M_EPMAP_LAST if last else M_EPMAP_MORE))
            log("tx", ep="0x0", type="EPMAP_REPLY", last=last)
            if last and not started_sys:
                started_sys = True
                for e in SYS_EPS:
                    if e in announced:
                        send(mm, EP_MGMT, (M_STARTEP << 52) | (e << 32)
                             | MGMT_STARTEP_FLAG)
                        log("tx", ep="0x0", type="STARTEP", target=f"{e:#x}")
        elif t == M_SET_IOP_PWR_STATE:
            state = msg0 & 0xFFFF
            log("iop_pwr_state", state=f"{state:#x}")
            send(mm, EP_MGMT, (M_SET_IOP_PWR_STATE_ACK << 52) | state)
            log("tx", ep="0x0", type="SET_IOP_PWR_STATE_ACK")
        elif t == M_SET_AP_PWR_STATE_ACK:
            log("ap_pwr_state_ack", state=f"{msg0 & 0xFFFF:#x}")
            if not started_app:
                started_app = True      # fw app is live: start announced app EPs
                for e in APP_EPS:
                    if e in announced:
                        send(mm, EP_MGMT, (M_STARTEP << 52) | (e << 32)
                             | MGMT_STARTEP_FLAG)
                        log("tx", ep="0x0", type="STARTEP", target=f"{e:#x}")
            if a.set_ap_on:
                send(mm, EP_MGMT, (M_SET_AP_PWR_STATE << 52) | PWR_STATE_ON)
                log("tx", ep="0x0", type="SET_AP_PWR_STATE_ON")
        else:
            log("note", text="unknown mgmt type; logged only")
    log("done")

if __name__ == "__main__":
    main()
