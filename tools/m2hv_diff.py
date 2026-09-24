#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Diff an m1n1 hv MMIO trace of the macOS T6021 ANE start against the
Linux ane_t6021 write baseline (tools/m2hv_linux_baseline.txt).

  m2hv_diff.py /tmp/m2hv            newest trace.log at or under the dir
  m2hv_diff.py DIR/trace.log [--all] [--context N]
  m2hv_diff.py --linux              print the named Linux baseline
  m2hv_diff.py --selftest

Trace lines are m1n1 proxyclient PrintTracer.event_mmio
(m1n1/trace/__init__.py), one per 8-byte-or-smaller access:
  # [cpuN] [0xPC] MMIO: W.4   0xADDR (DEV, offset 0xOFF) = 0xDATA
each followed by a p.writeN() replay line (ignored). hv.log lines may
carry a [CNTPCT] prefix; "TTY> " text is the guest console and is kept
as ordering context. There are no timestamps on MMIO lines: time is the
event ordinal (#evt) and the log line number.
"""
import argparse
import os
import re
import sys
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, "m2hv_linux_baseline.txt")
ENGINE = 0x284000000
EAST = 0x290280000
M32 = 0xFFFFFFFF

# trace config v3 (/tmp/m2kstart/trace_ane_v3.py), writes only
REGIONS = (
    ("engine-low", 0x284000000, 0x1000),
    ("rvbar", 0x285050000, 0x100),
    ("asc-wrapper", 0x285400000, 0x14000),
    ("dart-ane0", 0x285800000, 0x4000),
    ("dart-ane1", 0x285810000, 0x4000),
    ("dart-ane2", 0x285820000, 0x4000),
    ("scratch", 0x285840000, 0x100),
    ("pmgr", 0x28E080000, 0x10000),
    ("venc-root-ps", 0x290280000, 0x400),
    ("venc-leaf-ps", 0x290288000, 0x40),
)

# ane/t6021/ane_t6021_boot.h ane_t6021_boot_run() P0 table + P-1 tunables
_TUN = (0x000, 0x038, 0x03C, 0x400, 0x600, 0x738, 0x798, 0x7F8, 0x900,
        0x410, 0x420, 0x430)
ENG_NAMES = {0xB38: "ENG_TABLE0(P0-1)", 0xB98: "ENG_TABLE1(P0-2)",
             0xBF8: "ENG_TABLE2(P0-3)"}
ENG_NAMES.update({o: f"ENG_TUN_{o:03X}(P-1{c})"
                  for c, o in zip("abcdefghijkl", _TUN)})
# ane_t6021.h ANE_ASC_* (engine+0x1400000 = ASC base; mailbox at +0x8000,
# m1n1 hw/asc.py ASCRegs + upstream apple_mbox_asc_hw)
ASC_NAMES = {0x44: "CPU_CONTROL", 0x48: "CPU_STATUS",
             0x8110: "A2I_CTRL", 0x8114: "I2A_CTRL(outbox)",
             0x8800: "A2I_SEND0", 0x8808: "A2I_SEND1",
             0x8810: "A2I_RECV0", 0x8818: "A2I_RECV1",
             0x8820: "I2A_SEND0", 0x8828: "I2A_SEND1",
             0x8830: "I2A_RECV0", 0x8838: "I2A_RECV1"}
RVBAR_NAMES = {0x0: "RVBAR", 0x4: "RVBAR_HI"}
# ane_t6021.h ANE_ASC_VERS / ANE_MBI_SCRATCH0..7 / ANE_ASC_RTB_STATUS*
SCRATCH_NAMES = {0x0: "RTB_VERS", 0x7C: "RTB_STATUS_UNK7C",
                 0x88: "RTB_STATUS"}
SCRATCH_NAMES.update({0x48 + 4 * i: f"SCRATCH{i}" for i in range(8)})
# t8110 DART: m1n1 hw/dart8110.py DART8110Regs; Linux apple-dart.c names
# ERROR_DISABLE "ERROR_MASK" and ERR_SECONDARY "ERROR_STREAMS"
DART_NAMES = {0x0: "PARAMS_0", 0x4: "PARAMS_4", 0x8: "PARAMS_8",
              0xC: "PARAMS_C", 0x80: "TLB_OP", 0x84: "TLB_OP_IDX",
              0x88: "TLB_TAG_LO", 0x8C: "TLB_TAG_HI", 0x90: "TLB_PA_LO",
              0x94: "TLB_PA_HI", 0x98: "TLB_START_DVA_PAGE",
              0xA0: "TLB_END_DVA_PAGE", 0x100: "ERROR",
              0x104: "ERROR_DISABLE", 0x170: "ERROR_ADDR_LO",
              0x174: "ERROR_ADDR_HI", 0x200: "PROTECT", 0x204: "UNPROTECT",
              0x208: "PROTECT_LOCK", 0x210: "DIAG_LOCK", 0x228: "TLIMIT",
              0x22C: "TEQRESERVE", 0x500: "TZ_CONFIG", 0x504: "TZ_SELECT",
              0x508: "TZ_REGION0_START", 0x510: "TZ_REGION0_END",
              0x518: "TZ_REGION0_OFFSET", 0x520: "TZ_REGION1_START",
              0x528: "TZ_REGION1_END", 0x530: "TZ_REGION1_OFFSET",
              0x538: "TZ_REGION2_START", 0x540: "TZ_REGION2_END",
              0x548: "TZ_REGION2_OFFSET", 0x700: "PERF_INTR_ENABLE",
              0x704: "PERF_INTR_STATUS", 0x760: "PERF_TLB_MISS",
              0x764: "PERF_TLB_FILL", 0x768: "PERF_TLB_HIT",
              0x770: "PERF_ST_MISS", 0x774: "PERF_ST_FILL",
              0x778: "PERF_ST_HIT", 0x780: "PERF_CTC_MISS",
              0x784: "PERF_CTC_FILL", 0x788: "PERF_CTC_HIT"}
DART_ARRAYS = ((0x120, 8, "STREAM_UNK_SET"), (0x140, 8, "STREAM_UNK_CLR"),
               (0x1C0, 8, "ERR_SECONDARY"), (0x230, 4, "TRANS"),
               (0x720, 8, "PERF_UNK1"), (0x740, 8, "PERF_UNK2"),
               (0x800, 256, "UNK_TUNABLES"), (0xC00, 8, "ENABLE_STREAMS"),
               (0xC20, 8, "DISABLE_STREAMS"), (0x1000, 256, "TCR"),
               (0x1400, 256, "TTBR"))


def _ps_table(s):
    return {int(o, 16): n for o, n in (t.split(":") for t in s.split())}


# ps words, omarchy-linux 078f865d1 arch/arm64/boot/dts/apple/t602x-pmgr.dtsi
# (&pmgr 0x28e080000 all; &pmgr_east 0x290280000 inside the two traced
# windows only)
PMGR_PS = _ps_table("""
100:afi 108:aic 110:dwi 118:pms 120:gpio 128:soc_dpe 130:pms_c1ppt
138:pmgr_soc_ocla 168:amcc0 170:amcc2 178:dcs_00 180:dcs_01 188:dcs_02
190:dcs_03 198:dcs_08 1a0:dcs_09 1a8:dcs_10 1b0:dcs_11 1b8:afnc1_ioa
1d0:afc 1e8:afnc0_ioa 1f0:afnc1_ls 1f8:afnc0_ls 200:afnc1_lw0
208:afnc1_lw1 210:afnc1_lw2 218:afnc0_lw0 220:scodec 228:atc0_common
230:atc1_common 238:atc2_common 240:atc3_common 248:dispext1_sys
250:pms_bridge 258:dispext0_sys 260:ane_sys 268:avd_sys 270:atc0_cio
278:atc0_pcie 280:atc1_cio 288:atc1_pcie 290:atc2_cio 298:atc2_pcie
2a0:atc3_cio 2a8:atc3_pcie 2b0:dispext1_fe 2b8:dispext1_cpu0
2c0:dispext0_fe 2c8:pmp 2d0:pms_sram 2d8:dispext0_cpu0 2e0:ane_cpu
2e8:atc0_cio_pcie 2f0:atc0_cio_usb 2f8:atc1_cio_pcie 300:atc1_cio_usb
308:atc2_cio_pcie 310:atc2_cio_usb 318:atc3_cio_pcie 320:atc3_cio_usb
390:trace_fab 4000:ane_sys_mpm 4008:ane_td 4010:ane_base 4018:ane_set1
4020:ane_set2 4028:ane_set3 4030:ane_set4
""")
EAST_PS = _ps_table("""
100:clvr_spmi0 108:clvr_spmi1 110:clvr_spmi2 118:clvr_spmi3
120:clvr_spmi4 128:ispsens0 130:ispsens1 138:ispsens2 140:ispsens3
148:afnc6_ioa 150:afnc6_ls 158:afnc6_lw0 160:afnc2_ioa 168:afnc2_ls
170:afnc2_lw0 178:afnc2_lw1 180:afnc3_ioa 188:afnc3_ls 190:afnc3_lw0
198:apcie_gp 1a0:apcie_st 1a8:ans2 1b0:disp0_sys 1b8:jpg 1c0:sio
1c8:isp_sys 1d0:disp0_fe 1d8:disp0_cpu0 1e0:sio_cpu 1e8:fpwm0 1f0:fpwm1
1f8:fpwm2 200:i2c0 208:i2c1 210:i2c2 218:i2c3 220:i2c4 228:i2c5
230:i2c6 238:i2c7 240:i2c8 248:spi_p 250:sio_spmi0 258:sio_spmi1
260:sio_spmi2 268:uart_p 270:audio_p 278:sio_adma 280:aes
288:dptx_phy_ps 2d8:spi0 2e0:spi1 2e8:spi2 2f0:spi3 2f8:spi4 300:spi5
308:uart_n 310:uart0 318:amcc1 320:amcc3 328:dcs_04 330:dcs_05
338:dcs_06 340:dcs_07 348:dcs_12 350:dcs_13 358:dcs_14 360:dcs_15
368:uart1 370:uart2 378:uart3 380:uart4 388:uart5 390:uart6 398:mca0
3a0:mca1 3a8:mca2 3b0:mca3 3b8:dpa0 3c0:dpa1 3c8:dpa2 3d0:dpa3 3d8:msr0
3e0:venc_sys 3e8:dpa4 3f0:msr0_ase_core 3f8:apcie_gpshr_sys
8000:venc_dma 8008:venc_pipe4 8010:venc_pipe5 8018:venc_me0
8020:venc_me1
""")
# ANE islands, ps_ane_sys's always-on fabric parents (dtsi power-domains
# chain), the VENC rails behind ADT clock-ids 318-321 (+VENC_SYS 299)
RELEVANT_PS = {"ane_sys", "ane_cpu", "ane_sys_mpm", "ane_td", "ane_base",
               "ane_set1", "ane_set2", "ane_set3", "ane_set4",
               "afnc0_lw0", "afnc0_ls", "afnc0_ioa", "afi",
               "venc_sys", "venc_dma", "venc_pipe4", "venc_pipe5",
               "venc_me0", "venc_me1"}
PS_BITS = ((1 << 31, "RESET"), (1 << 28, "AUTO"), (1 << 12, "PS_RESET"),
           (1 << 10, "DEV_DISABLE"))
MGMT = {1: "HELLO", 2: "HELLO_REPLY", 4: "PING", 5: "STARTEP",
        6: "SET_IOP_PWR", 7: "IOP_PWR_ACK", 8: "EPMAP", 0xB: "SET_AP_PWR"}


def reg_name(addr):
    """(region, register name, ANE-relevant) for an absolute address."""
    for reg, base, size in REGIONS:
        if base <= addr < base + size:
            off = addr - base
            break
    else:
        return "-", f"{addr:#x}", False
    if reg == "engine-low":
        return reg, ENG_NAMES.get(off, f"eng+{off:#x}"), True
    if reg == "rvbar":
        return reg, RVBAR_NAMES.get(off, f"rvbar+{off:#x}"), True
    if reg == "asc-wrapper":
        return reg, ASC_NAMES.get(off, f"asc+{off:#x}"), True
    if reg == "scratch":
        return reg, SCRATCH_NAMES.get(off, f"scratch+{off:#x}"), True
    if reg.startswith("dart"):
        if off in DART_NAMES:
            return reg, DART_NAMES[off], True
        for start, n, name in DART_ARRAYS:
            if start <= off < start + 4 * n:
                return reg, f"{name}[{(off - start) // 4}]", True
        return reg, f"dart+{off:#x}", True
    if reg == "pmgr":
        if off >= 0xC000:   # ane0 "set" reg window 0x28e08c000
            return reg, f"ane_SET+{off - 0xC000:#x}", True
        n = PMGR_PS.get(off)
        return (reg, f"ps_{n}", n in RELEVANT_PS) if n else \
            (reg, f"pmgr+{off:#x}", False)
    n = EAST_PS.get(addr - EAST)
    return (reg, f"ps_{n}", n in RELEVANT_PS) if n else \
        (reg, f"pmgr_east+{addr - EAST:#x}", False)


def eng_off(addr):
    off = addr - ENGINE
    return f" [eng+{off:#x}]" if 0 <= off < 0x2000000 else ""


def decode(name, val):
    if name.startswith("ps_"):
        return f"T={val & 0xF:x} A={(val >> 4) & 0xF:x}" + "".join(
            " " + k for b, k in PS_BITS if val & b)
    if name == "CPU_CONTROL":
        return "RUN" if val & 0x10 else "RUN=0"
    if name == "A2I_SEND0":
        t = (val >> 52) & 0xFF
        return f"type {t:#x} {MGMT.get(t, '?')}"
    if name == "TLB_OP":
        op = (val >> 8) & 7
        return f"{('FLUSH_ALL', 'FLUSH_SID')[op] if op < 2 else f'op{op}'} sid {val & 0xFF}"
    if name.startswith("TCR["):
        return "|".join(k for b, k in ((1, "XLATE"), (2, "BYP_DART"),
                        (4, "BYP_DAPF"), (8, "4LVL"), (0x80, "REMAP"))
                        if val & b) or "off"
    return ""


# ---------------------------------------------------------------- trace
Ev = namedtuple("Ev", "idx line ts cpu pc rw w addr val multi dev ctx")
MMIO_RE = re.compile(
    r"(?:\[(?P<ts>0x[0-9a-fA-F]+)\])?\[cpu(?P<cpu>\d+)\] \[(?P<pc>0x[0-9a-fA-F]+)\]"
    r" MMIO: (?P<rw>[RW])\.(?P<w>\d+)\s*(?P<multi>\+?)\s+(?P<addr>0x[0-9a-fA-F]+)"
    r" \((?P<dev>[^()]*), offset 0x[0-9a-fA-F]+\) = (?P<val>0x[0-9a-fA-F]+)")
# m1n1 hv map_essential() PMGR HACK hooks (hv/__init__.py wh/rh): the ps
# words UART0/ATC-USB depend on (ps_afi, an ANE fabric ancestor, among
# them) are RESERVED, not traced; the guest's accesses are logged
#   [cpuN] PMGR W 28e080100+0:32 = 0xf0000ff: Dangerous write
#   [cpuN] PMGR R 28e080100+0:32 = 0xf0000ff -> 0xf0000ff
# A W value is what the guest wrote; hardware got only bits [9:0] as
# (v | 0xf) & ~0x400. These are logged synchronously while MMIO lines are
# ASYNC, so their order against MMIO lines is approximate. pc = 0.
HOOK_RE = re.compile(
    r"(?:\[(?P<ts>0x[0-9a-fA-F]+)\])?\[cpu(?P<cpu>\d+)\] PMGR (?P<rw>[RW])"
    r" (?P<base>[0-9a-fA-F]+)\+(?P<off>[0-9a-fA-F]+):(?P<bits>\d+) = (?P<val>0x[0-9a-fA-F]+)")
HOOK_DEV = "PMGR-HOOK"


def parse_trace(lines):
    evs, ctx = [], ""
    for no, ln in enumerate(lines, 1):
        if "TTY> " in ln:
            t = ln.rsplit("TTY> ", 1)[1].split("# [", 1)[0].strip()
            ctx = t or ctx
        m = MMIO_RE.search(ln)
        if m:
            g = m.groupdict()
            evs.append(Ev(len(evs), no, g["ts"], int(g["cpu"]), int(g["pc"], 16),
                          g["rw"], int(g["w"]), int(g["addr"], 16),
                          int(g["val"], 16), bool(g["multi"]), g["dev"], ctx))
            continue
        m = HOOK_RE.search(ln)
        if m:
            g = m.groupdict()
            evs.append(Ev(len(evs), no, g["ts"], int(g["cpu"]), 0, g["rw"],
                          int(g["bits"]) // 8,
                          int(g["base"], 16) + int(g["off"], 16),
                          int(g["val"], 16), False, HOOK_DEV, ctx))
    return evs


def pc_str(e):
    return f"{'pmgr-hook':<18}" if e.dev == HOOK_DEV else f"{e.pc:#018x}"


def pick_trace(path):
    """A file is used as given. A dir (flat, or <stamp>/ subdirs): the
    directory with the newest *.log first, inside it trace.log, run.log,
    then other *.log newest first; the first with MMIO lines wins. With
    none, the first log found (the report then states zero events and
    the traced windows)."""
    if os.path.isfile(path):
        return path
    rank = {"trace.log": 0, "run.log": 1}
    groups = []
    for d in [path] + [os.path.join(path, s) for s in os.listdir(path)]:
        if not os.path.isdir(d):
            continue
        logs = [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".log")]
        if logs:
            groups.append((max(map(os.path.getmtime, logs)), sorted(
                logs, key=lambda p: (rank.get(os.path.basename(p), 2),
                                     -os.path.getmtime(p)))))
    cands = [p for _, logs in sorted(groups, reverse=True) for p in logs]
    for p in cands:
        if any(MMIO_RE.search(ln) for ln in open(p, errors="replace")):
            return p
    if cands:
        return cands[0]
    sys.exit(f"m2hv_diff: no *.log at or under {path}")


PT_RE = re.compile(r"# PT\[([0-9a-fA-F]+):([0-9a-fA-F]+)\] -> (.*)")


def untraced(lines):
    """Subranges of the v3 regions the hv page-table dump (# PT[a:b] ->
    MODE) maps as anything but a PrintTracer trace, e.g. m1n1's
    RESERVED PMGR HACK ps words. None when the log has no PT dump."""
    pt = [(int(a, 16), int(b, 16), m.strip())
          for a, b, m in (x.groups() for x in map(PT_RE.search, lines) if x)]
    if not pt:
        return None
    return [(reg, max(a, base), min(b, base + size), m)
            for reg, base, size in REGIONS for a, b, m in pt
            if max(a, base) < min(b, base + size) and "PrintTracer" not in m]


# ------------------------------------------------------------- collapse
Row = namedtuple("Row", "ev n last period")


def _rle(items, key, maxp):
    """Run-length encode repeated blocks of period <= maxp:
    [(block, repeats, all items)]."""
    ks = [key(x) for x in items]
    out, i = [], 0
    while i < len(items):
        bp, br = 1, 1
        for p in range(1, maxp + 1):
            r = 1
            while ks[i + r * p:i + (r + 1) * p] == ks[i:i + p]:
                r += 1
            if (r - 1) * p > (br - 1) * bp:
                bp, br = p, r
        out.append((items[i:i + bp], br, items[i:i + bp * br]))
        i += bp * br
    return out


def collapse(evs):
    """Polls (runs of reads of one register, any value) and repeated
    write blocks (period <= 8, e.g. per-page TLB flushes on 3 DARTs)
    become one row each, counted."""
    key = lambda e: (e.rw, e.addr, e.val if e.rw == "W" else None)
    rows = [Row(blk[0], r, allv[-1], 1) for blk, r, allv in _rle(evs, key, 1)]
    out = []
    for blk, r, allv in _rle(rows, lambda x: key(x.ev), 8):
        p = len(blk)
        for j, row in enumerate(blk):
            mine = allv[j::p]
            out.append(Row(row.ev, sum(x.n for x in mine), mine[-1].last,
                           p if r > 1 else 1))
    return out


# ------------------------------------------------------------- baseline
B = namedtuple("B", "seq phase cond addr w val mask src")
CONDS = ("live", "if-off", "observer", "gated")
STATUS = {"live": "MATCH", "if-off": "MATCH-C", "observer": "OBS",
          "gated": "GATED"}
SEVERITY = ("MATCH", "MATCH-C", "OBS", "GATED", "VALUE", "MISSING")


def load_baseline(path):
    out = []
    for ln in open(path):
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        p, c, a, w, v, m, s = ln.split(None, 6)
        w, a = int(w), int(a, 16)
        assert c in CONDS, f"baseline: bad cond {c!r}: {ln}"
        assert reg_name(a)[0] != "-", f"baseline: {a:#x} outside trace v3: {ln}"
        full = (1 << (8 * w)) - 1
        out.append(B(len(out), p, c, a, w, int(v, 16),
                     full if m == "-" else int(m, 16), s.strip()))
    return out


def granules(addr, w, val, mask):
    """32-bit comparison granules (a 64-bit store is its two halves)."""
    if w == 8:
        return ((addr, val & M32, mask & M32),
                (addr + 4, val >> 32, (mask >> 32) & M32))
    return ((addr, val, mask),)


# ----------------------------------------------------------------- diff
Res = namedtuple("Res", "row region name rel status hits cands seq order_after")


def diff(rows, base):
    idx = {}
    for b in base:
        for ga, gv, gm in granules(b.addr, b.w, b.val, b.mask):
            idx.setdefault(ga, []).append((b, gv, gm))
    res, hwm, hwm_res, matched = [], -1, None, set()
    for row in rows:
        e = row.ev
        region, name, rel = reg_name(e.addr)
        if e.rw != "W" or not rel:
            res.append(Res(row, region, name, rel,
                           "read" if e.rw == "R" else "other", [], [], None, None))
            continue
        worst, hits, cands = "MATCH", [], []
        for ga, gv, _ in granules(e.addr, e.w, e.val, (1 << (8 * e.w)) - 1):
            c = idx.get(ga, [])
            h = [b for b, bv, bm in c if (gv ^ bv) & bm == 0]
            if not c:
                st = "MISSING"
            elif not h:
                st = "VALUE"
            else:
                best = min(CONDS.index(b.cond) for b in h)
                h = [b for b in h if CONDS.index(b.cond) == best]
                st = STATUS[CONDS[best]]
            if SEVERITY.index(st) >= SEVERITY.index(worst):
                worst = st
            hits = hits or h
            cands += [b for b, _, _ in c if b not in cands]
        seq = order_after = None
        if worst in ("MATCH", "MATCH-C"):
            later = [b.seq for b in hits if b.seq >= hwm]
            seq = min(later) if later else min(b.seq for b in hits)
            if seq < hwm:
                order_after = hwm_res
            matched.update(b.seq for b in hits)
        r = Res(row, region, name, rel, worst, hits, cands, seq, order_after)
        if seq is not None and seq > hwm:
            hwm, hwm_res = seq, r
        res.append(r)
    return res, matched


# --------------------------------------------------------------- report
def fmt_val(w, v):
    return f"{v:#018x}" if w == 8 else f"{v:#010x}"


def linux_note(r, base):
    if r.status in ("read", "other"):
        return ""
    if r.status == "MISSING":
        return "Linux never writes this register"
    if r.status == "VALUE":
        vals = sorted({(b.val, b.mask, b.cond, b.phase) for b in r.cands})
        return "Linux writes only " + "; ".join(
            f"{fmt_val(r.row.ev.w, v)}&{m:#x} ({c} {p})" for v, m, c, p in vals[:4])
    b = base[r.seq] if r.seq is not None else r.hits[0]
    note = f"L{b.seq} {b.phase}"
    if r.status in ("GATED", "OBS"):
        note += f" [{b.cond}] {b.src}"
    if r.order_after is not None:
        note += (f"  ORDER: Linux does this before L{r.order_after.seq}"
                 f" {base[r.order_after.seq].phase} (macOS evt"
                 f" #{r.order_after.row.ev.idx})")
    return note


def row_line(r, base):
    e, row = r.row.ev, r.row
    rep = f"x{row.n}" + (f"/blk{row.period}" if row.period > 1 else "")
    val = fmt_val(e.w, e.val)
    if e.rw == "R" and row.n > 1:
        val += f"..{row.last.val:#x}"
    dec = decode(r.name, e.val)
    return (f"#{e.idx:<6} L{e.line:<7} cpu{e.cpu:<2} {pc_str(e)} {e.rw}.{e.w}"
            f"{'+' if e.multi else ' '} {e.addr:#011x} {r.region:<12} "
            f"{r.name + eng_off(e.addr):<34} {val:<18} {rep:<9} "
            f"{r.status:<8} {dec + '  ' if dec else ''}{linux_note(r, base)}")


def linux_position(res, i, base):
    prev = next((r for r in reversed(res[:i]) if r.seq is not None), None)
    nxt = next((r for r in res[i + 1:] if r.seq is not None), None)
    desc = lambda r: (f"L{r.seq} {base[r.seq].phase} {r.name}="
                      f"{fmt_val(r.row.ev.w, r.row.ev.val)} (macOS #{r.row.ev.idx})")
    return (f"after {desc(prev)}" if prev else "before any matched Linux write",
            f"before {desc(nxt)}" if nxt else "after the last matched Linux write")


def answer(title, res, i, base, ctxn):
    r = res[i]
    e = r.row.ev
    print(f"{title}:")
    print(f"  macOS evt #{e.idx} (log line {e.line}) cpu{e.cpu} pc {pc_str(e).strip()}"
          f" W.{e.w} {e.addr:#x} {r.region} {r.name}{eng_off(e.addr)}"
          f" = {fmt_val(e.w, e.val)}  {decode(r.name, e.val)}  (ADT dev {e.dev})")
    print(f"  status {r.status}: {linux_note(r, base)}")
    after, before = linux_position(res, i, base)
    print(f"  Linux sequence position: {after}")
    print(f"                           {before}")
    if e.ctx:
        print(f"  guest console before it: {e.ctx[:120]}")
    rel = [j for j, x in enumerate(res) if x.rel and x.row.ev.rw == "W"]
    k = rel.index(i)
    print("  macOS relevant writes around it:")
    for j in rel[max(0, k - ctxn):k + ctxn + 1]:
        print(("  >> " if j == i else "     ") + row_line(res[j], base))
    print()


def report(path, show_all, ctxn, base_path):
    lines = open(path, errors="replace").read().splitlines()
    evs = parse_trace(lines)
    base = load_baseline(base_path)
    rows = collapse(evs)
    res, matched = diff(rows, base)
    nw = sum(e.rw == "W" for e in evs)
    relw = [i for i, r in enumerate(res) if r.rel and r.row.ev.rw == "W"]
    nhook = sum(e.dev == HOOK_DEV for e in evs)
    print(f"m2hv_diff: {path}: {len(lines)} lines, {len(evs)} events "
          f"({len(evs) - nhook} MMIO + {nhook} PMGR-hook; {nw} W, {len(evs) - nw} R), "
          f"{len(rows)} rows after collapse, {len(relw)} ANE-relevant write rows")
    print(f"baseline: {base_path} ({len(base)} Linux writes)")
    holes = untraced(lines)
    blind = lambda a: any(lo <= a < hi and "PMGR HACK" not in m
                          for _, lo, hi, m in holes or ())
    if holes is None:
        print("traced windows: no '# PT[...]' dump in this log; assuming trace v3")
    else:
        print(f"traced windows: v3 regions minus {len(holes)} hv holes. PMGR HACK"
              " holes log guest accesses as PMGR W/R hook lines (parsed, pc"
              " 'pmgr-hook', order vs MMIO approximate); other holes are blind:")
        for reg, lo, hi, m in holes:
            nw_h = sum(e.dev == HOOK_DEV and e.rw == "W" and lo <= e.addr < hi
                       for e in evs)
            print(f"  {reg:<13} {lo:#x}-{hi:#x} {reg_name(lo)[1]:<18} {m}"
                  + (f"  ({nw_h} hook W)" if "PMGR HACK" in m else "  (BLIND)"))
    print()
    if not evs:
        print("no MMIO events parsed")
        return 1
    counts = {s: sum(res[i].status == s for i in relw) for s in SEVERITY}
    print("relevant write rows: " + ", ".join(f"{s} {n}" for s, n in counts.items()))
    print("\n== ANSWER ==")
    firsts = (
        ("FIRST DIVERGENCE vs the Linux driver (MISSING/VALUE/GATED/OBS)",
         ("OBS", "GATED", "VALUE", "MISSING")),
        ("FIRST WRITE NO LINUX PATH HAS EVER ISSUED LIVE (MISSING/VALUE/GATED)",
         ("GATED", "VALUE", "MISSING")))
    shown = None
    for title, sts in firsts:
        i = next((i for i in relw if res[i].status in sts), None)
        if i is None:
            print(f"{title}: none\n")
        elif i == shown:
            print(f"{title}: same row (#{res[i].row.ev.idx})\n")
        else:
            answer(title, res, i, base, ctxn)
        shown = i
    # Linux boot-phase writes (B*) all precede insmod, so a macOS boot
    # interleaving them is expected; lead with inversions that involve
    # the insmod sequence (fwload, VENC, P-1..P6, RTKit).
    inv = [i for i in relw if res[i].order_after is not None]
    boot = lambda r: base[r.seq].phase.startswith("B")
    i = next((i for i in inv if not (boot(res[i]) and boot(res[i].order_after))),
             inv[0] if inv else None)
    if i is None:
        print("FIRST ORDER INVERSION: none\n")
    else:
        answer(f"FIRST ORDER INVERSION ({len(inv)} rows; insmod-phase ones first):"
               " matched write Linux issues earlier", res, i, base, ctxn)
    print("FIRST DIVERGENCE PER REGION:")
    for reg, _, _ in REGIONS:
        i = next((i for i in relw if res[i].region == reg and
                  res[i].status not in ("MATCH", "MATCH-C")), None)
        n = sum(res[j].region == reg for j in relw)
        print(f"  {reg:<13} {n:>5} rows  " +
              (row_line(res[i], base) if i is not None else "no divergence"))
    print("\n== FULL DIFF TABLE (" + ("all rows" if show_all else
          "ANE-relevant rows; --all adds other pmgr ps") + ") ==")
    for r in res:
        if show_all or r.rel:
            print(row_line(r, base))
    print("\n== LINUX-ONLY: live/if-off baseline writes macOS never issued ==")
    by_phase = {}
    for b in base:
        if b.cond in ("live", "if-off") and b.seq not in matched:
            by_phase.setdefault(b.phase, []).append(b)
    for ph, bs in by_phase.items():
        items = [f"{reg_name(b.addr)[1]}={b.val:#x}" +
                 (" (untraced)" if blind(b.addr) else "") for b in bs]
        print(f"  {ph:<16} {len(bs):>3}: " + ", ".join(items[:8]) +
              (f", +{len(items) - 8} more" if len(items) > 8 else ""))
    return 0


def print_linux(base_path):
    for b in load_baseline(base_path):
        reg, name, _ = reg_name(b.addr)
        print(f"L{b.seq:<4} {b.phase:<16} {b.cond:<8} {b.addr:#011x} "
              f"{reg:<12} {name + eng_off(b.addr):<34} "
              f"{fmt_val(b.w, b.val)} mask {b.mask:#x}  {b.src}")


# ------------------------------------------------------------- selftest
def _line(cpu, pc, rw, w, addr, dev, start, val, multi=False):
    # byte-for-byte PrintTracer.event_mmio (m1n1/trace/__init__.py:166-176)
    ll = (f"[cpu{cpu}] [0x{pc:016x}] MMIO: {rw}.{w:<2}{'+' if multi else ' '} "
          f"0x{addr:x} ({dev}, offset {addr - start:#04x}) = 0x{val:x}")
    stmt = (f"p.write{8 * w}({start:#x} + {addr - start:#x}, {val:#x})"
            if rw == "W" else f"p.read{8 * w}({start:#x} + {addr - start:#x})")
    return [f"# {ll}", stmt]


def selftest():
    A, PM, D0 = 0x284000000, 0x28E080000, 0x285800000
    # PT/Pass lines verbatim from the first live v3 log (/tmp/m2hv/trace-snap.log)
    t = ["# PT[28e080000:28e080100] -> ASYNC. PrintTracer",
         "# PT[28e080100:28e080104] -> RESERVED PMGR HACK",
         "# PT[28e080104:28e0801a8] -> ASYNC. PrintTracer",
         "# [cpu0] Pass: mrs x13, ACC_CFG_EL1 = d (ACC_CFG_EL1)",
         "TTY> AppleH11ANEInterface::start",
         # verbatim from the 13.5 trace (/tmp/m2hv/trace-135.log lines 110-111)
         "# [cpu0] PMGR R 28e080100+0:32 = 0xf0000ff -> 0xf0000ff",
         "# [cpu0] PMGR W 28e080100+0:32 = 0xf0000ff: Dangerous write"]
    assert untraced(t) == [("pmgr", PM + 0x100, PM + 0x104, "RESERVED PMGR HACK")]
    assert reg_name(PM + 0x100)[1:] == ("ps_afi", True)
    W = lambda addr, val, w=4, dev="ane[0]", start=A, multi=False: \
        t.extend(_line(0, 0xFFFFFE0008BC5180, "W", w, addr, dev, start, val, multi))
    W(PM + 0x2E0, 0x1000000F, dev="pmgr[0]", start=PM)     # ps_ane_cpu
    W(PM + 0x238, 0xF, dev="pmgr[0]", start=PM)            # ps_atc2_common
    W(D0 + 0x800, 0x3, dev="dart-ane0[0]", start=D0)       # UNK_TUNABLES[0]
    W(D0 + 0xC00, 0xFFFFFFFF, dev="dart-ane0[0]", start=D0)
    W(D0 + 0x1000, 0x9, dev="dart-ane0[0]", start=D0)      # TCR before TTBR
    W(D0 + 0x1400, 0x1001248D, dev="dart-ane0[0]", start=D0)
    for _ in range(4):
        for b in (D0, 0x285810000, 0x285820000):
            W(b + 0x80, 0x100, dev="dart", start=b)
    for v in (0, 0, 0, 1, 0x08042006):
        t.extend(_line(1, 0xFFFFFE00095E99E4, "R", 4, A + 0x1840064,
                       "ane[0]", A, v))
    t.append("TTY> RTBuddy: startCPU")
    W(A + 0x1408114, 0x20001)                              # _enableOutbox
    W(A + 0x1400044, 0x10)                                 # _runCPU
    W(A + 0x1408800, (2 << 52) | 0xC000C, w=8, multi=True)  # HELLO_REPLY
    W(A + 0x1408808, 0, w=8, multi=True)
    evs = parse_trace(t)
    assert len(evs) == 29 and evs[-2].multi and evs[-2].w == 8, evs[-2:]
    hook = evs[1]
    assert (hook.dev, hook.rw, hook.w, hook.addr, hook.val, hook.pc) == \
        (HOOK_DEV, "W", 4, PM + 0x100, 0xF0000FF, 0), hook
    assert evs[-3].ctx == "RTBuddy: startCPU", evs[-3].ctx
    rows = collapse(evs)
    tlb = [r for r in rows if r.ev.addr in (D0 + 0x80, 0x285810080, 0x285820080)]
    assert [(r.n, r.period) for r in tlb] == [(4, 3)] * 3, tlb
    poll = [r for r in rows if r.ev.rw == "R" and r.ev.dev != HOOK_DEV]
    assert len(poll) == 1 and poll[0].n == 5 and poll[0].last.val == 0x08042006
    base = load_baseline(BASELINE)
    res, _ = diff(rows, base)
    st = {r.name: r for r in res}
    assert st["ps_ane_cpu"].status == "MATCH-C"
    assert st["ps_afi"].status == "MATCH-C" and st["ps_afi"].row.ev.dev == HOOK_DEV
    assert not st["ps_atc2_common"].rel and st["ps_atc2_common"].status == "other"
    assert st["UNK_TUNABLES[0]"].status == "MISSING"
    assert st["TCR[0]"].status == "MATCH" and st["TCR[0]"].order_after is None
    assert st["TTBR[0]"].order_after is st["TCR[0]"], "TTBR after TCR is an inversion"
    assert st["I2A_CTRL(outbox)"].status == "OBS"
    assert st["CPU_CONTROL"].status == "MATCH"
    assert st["A2I_SEND0"].status == "GATED"            # rtkit HELLO_REPLY
    assert st["A2I_SEND1"].status == "OBS"              # ane_obs wakepwr wrote EP0
    rel = [r for r in res if r.rel and r.row.ev.rw == "W"]
    first = next(r for r in rel if r.status not in ("MATCH", "MATCH-C"))
    assert first.name == "UNK_TUNABLES[0]", first
    assert reg_name(0x2902803E0)[1:] == ("ps_venc_sys", True)
    assert reg_name(0x285400044)[1] == "CPU_CONTROL"
    assert reg_name(0x28E08C010)[1] == "ane_SET+0x10"
    print(f"m2hv_diff selftest: OK ({len(evs)} events, {len(rows)} rows, "
          f"baseline {len(base)} writes)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("trace", nargs="?", help="trace.log, or a dir (newest */trace.log)")
    ap.add_argument("--baseline", default=BASELINE)
    ap.add_argument("--all", action="store_true", help="table includes non-ANE pmgr rows")
    ap.add_argument("--context", type=int, default=3)
    ap.add_argument("--linux", action="store_true", help="print the named Linux baseline")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.linux:
        return print_linux(a.baseline)
    if not a.trace:
        ap.error("trace path required")
    return report(pick_trace(a.trace), a.all, a.context, a.baseline)


if __name__ == "__main__":
    sys.exit(main())
