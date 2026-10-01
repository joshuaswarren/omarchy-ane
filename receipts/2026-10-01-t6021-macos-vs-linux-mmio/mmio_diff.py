#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""T6021 ANE: macOS 13.5 hv-trace MMIO vs the Linux ane_t6021 footprint.

  mmio_diff.py REPO DART_IOREG OUTDIR

REPO        omarchy-ane checkout (tools/m2hv_replay-trace-135.txt,
            tools/m2hv_linux_baseline.txt, the iBoot j414c image)
DART_IOREG  macOS `ioreg` text of the dart-ane0 node (ane-linux-experiments
            d3dc1aef receipts/2026-09-23-m2-macos-denominator/m2-macos-window/
            ane-evidence/dart-ane-nodes.txt)
OUTDIR      receives the TSV tables and tlb.txt

Stdlib only. Asserts check the parse against facts stated in the receipts.
"""
import os
import re
import struct
import sys
from collections import OrderedDict

REPLAY = "tools/m2hv_replay-trace-135.txt"
BASELINE = "tools/m2hv_linux_baseline.txt"
IBOOT = "receipts/2026-09-20-iboot-j414c/iboot_j414c_dec.bin"

# Trace anchors (event ordinals of the replay file). The 13.5 guest is
# kernel-only, so no firmware start and no job exist in the trace.
PHASES = ((0, "pre-bringup"), (2813, "ane_sys-power-up"),
          (2816, "af-bridge"), (2842, "ane_cpu-on"),
          (2843, "dart-init"), (2923, "power-down"))

# Traced windows (tools/m2hv_diff.py REGIONS, trace config v3, writes only).
TRACED = (("engine-low", 0x284000000, 0x1000), ("rvbar", 0x285050000, 0x100),
          ("asc-wrapper", 0x285400000, 0x14000),
          ("dart-ane0", 0x285800000, 0x4000), ("dart-ane1", 0x285810000, 0x4000),
          ("dart-ane2", 0x285820000, 0x4000), ("scratch", 0x285840000, 0x100),
          ("pmgr", 0x28E080000, 0x10000), ("venc-root-ps", 0x290280000, 0x400),
          ("venc-leaf-ps", 0x290288000, 0x40))


def block(addr):
    """Physical block name; pmgr is split by function."""
    if addr in (0x28E080100, 0x28E0801E8, 0x28E0801F8, 0x28E080218):
        return "pmgr-ps-fabric(afi,afnc0)"
    if addr in (0x28E080260, 0x28E0802E0):
        return "pmgr-ps-ane_sys/ane_cpu"
    if addr == 0x28E0802C8:
        return "pmgr-ps-pmp"
    if 0x28E084000 <= addr <= 0x28E084033:
        return "pmgr-ps-ane-islands"
    if 0x28E08C000 <= addr < 0x28E090000:
        return "pmgr-SET-window"
    if 0x285804000 <= addr < 0x285808000:
        return "dapf-ane(LLT)"
    for name, base, size in TRACED:
        if base <= addr < base + size:
            return {"engine-low": "engine-low(AXI2AF bridge)",
                    "dart-ane0": "dart-ane0(LLT)", "dart-ane1": "dart-ane1(BRD)",
                    "dart-ane2": "dart-ane2(BWR)"}.get(name, name)
    return "other"


def phase(evt):
    return [n for e, n in PHASES if evt >= e][-1]


def parse_replay(path):
    rows = []
    for ln in open(path):
        if ln.startswith("#") or not ln.strip():
            continue
        f = ln.split(None, 7)
        rows.append(dict(evt=int(f[0]), w=f[1], addr=int(f[2], 16),
                         val=int(f[3], 16), linux=f[4], flags=f[5], region=f[6],
                         reg=f[7].strip()))
    return rows


def parse_baseline(path):
    rows = []
    for ln in open(path):
        if ln.startswith("#") or not ln.strip():
            continue
        f = ln.split(None, 6)
        rows.append(dict(phase=f[0], cond=f[1], addr=int(f[2], 16),
                         val=int(f[4], 16), src=f[6].strip()))
    return rows


def ioreg_props(path):
    t = open(path).read()
    return dict(re.findall(r'"([\w,\-]+)" = <([0-9a-f]*)>', t))


def adt_tunables(hexstr):
    """ADT dart-tunables-instance-N: {u32 off, u32 size, u64 mask, u64 val}."""
    b = bytes.fromhex(hexstr)
    return [struct.unpack_from("<IIQQ", b, i)[::1] for i in range(0, len(b), 24)]


def iboot_tables(path):
    """iBoot tunable tables: {u32 0x20000000|off, u32 mask, u32 val} records,
    each table ended by three 0xffffffff words. Returns only the tables that
    hold the ANE per-SID record (0x800, 0xf007f, 0x60000)."""
    ib = open(path, "rb").read()
    key = struct.pack("<III", 0x20000800, 0xF007F, 0x60000)
    end = b"\xff" * 12
    out = []
    for m in re.finditer(re.escape(key), ib):
        s = ib.rfind(end, 0, m.start()) + 12
        e = ib.find(end, m.start())
        recs = [struct.unpack_from("<III", ib, a) for a in range(s, e, 12)]
        out.append((s, [(r[0] & 0xFFFFFF, r[1], r[2]) for r in recs]))
    return out


def tsv(path, header, rows):
    with open(path, "w") as f:
        f.write("\t".join(header) + "\n")
        for r in rows:
            f.write("\t".join(str(x) for x in r) + "\n")


# Linux footprint, omarchy-ane 603c79b + omarchy-linux 57f8f6deaa3a.
# (block, addresses, access, when, count, source)
LINUX = (
    ("pmgr-ps-fabric(afi,afnc0)", "0x28e080100/1e8/1f8/218", "W (if off) TARGET=f, AUTO", "kernel boot, pmgr probe",
     "<=8 per boot", "pmgr-pwrstate.c:79-123,297; baseline B1"),
    ("pmgr-ps-ane_sys/ane_cpu", "0x28e080260, 0x28e0802e0", "W (if off) TARGET=f, AUTO; R 0x2e0", "genpd raise; ane probe G1 gate",
     "<=4 W + 1 R per boot", "pmgr-pwrstate.c; rtclient_main.c:1930"),
    ("pmgr-ps-pmp", "0x28e0802c8", "W (if off) TARGET=f", "genpd: dart-ane0 power-domains = ps_pmp",
     "<=2 per boot", "packaging/dt/t602x-ane.dtsi:118"),
    ("pmgr-ps-ane-islands", "0x28e084000..0x28e084030 (7 words)", "W (if off) TARGET=f, AUTO; R", "genpd raise (held until reboot); trace_td guard reads",
     "<=14 W per boot; 7 R per TD sample (trace_td=1 only)", "t602x-ane.dtsi:162-164; rtclient_main.c:798-811"),
    ("pmgr-ps-ane-islands", "0x28e084000 page", "DART map IOVA==PA (firmware writes it, not the host)", "fwload",
     "1 map per boot", "ane_t6021_fwload.c:130-152"),
    ("pmgr-SET-window", "0x28e08c000+0x4000", "none (DT reg[2], never mapped by the driver)", "-", "0", "rtclient_main.c:1922 maps reg[1] only"),
    ("engine-low(AXI2AF bridge)", "0x284000000 +0x000,0x038,0x03c,0x400,0x600,0x738,0x798,0x7f8,0x900,0x410,0x420,0x430",
     "W: 9 values from m1n1's T8103 ANE pmgr table, 0x410/0x420/0x430 = 0x1100", "P-1, before CPU release", "12 per boot", "ane_t6021_boot.h:555-582"),
    ("engine-low(AXI2AF bridge)", "0x284000b38/b98/bf8", "none (P0 table skipped, fw_start_table_mode=2)", "-", "0", "ane_t6021_boot.h:524-546; rtclient_main.c:120"),
    ("rvbar", "0x285050000", "R (W skipped when latched)", "probe, fwload, P2, report", "4 R per boot", "rtclient_main.c:1939; fwload.c:158; boot.h:607-610; boot.c:441"),
    ("asc-wrapper", "0x285400044 CPU_CONTROL, 0x285400048 CPU_STATUS", "W 0 then 0x10; R", "P3 release; probe/report", "2 W + 3 R per boot",
     "boot.h:616-619; rtclient_main.c:1938,2017; boot.c:442"),
    ("scratch", "0x285840048..0x285840064 (SCRATCH0-7)", "W clear/select/pulse, publish, wake, ack; R polls", "P1, P4-P7, P8 ack",
     "15 W + up to 2000 R per boot", "boot.h:591-600,630-695; rtclient_main.c:2062; boot.c:377-379,438"),
    ("asc-tick", "0x285160008", "R", "boot report", "2 per boot", "boot.c:429-434"),
    ("ipi-doorbell", "0x285844000 (+0x0 W ring, +0x8000 R pending, +0xc000 W clear)", "W/R", "every ChMan command; every T2H slot return; every fw MALLOC",
     "per CALL: 1 W + 1 R per 50 us poll (+1 W clear if pending); +1 W per T2H slot (2 events per CALL)",
     "rtclient_main.c:460-552,746-787"),
    ("tm-td-word", "0x285c20458", "R", "CALL wait, trace_td=1 only", "1 per 20-40 us sample", "rtclient_main.c:797-814"),
    ("asc-mailbox", "0x285408000", "none (apple-mailbox bound, never started: hello_wait_ms=0)", "-", "0",
     "rtclient_main.c:2048; drivers/soc/apple/mailbox.c:243-266"),
    ("dart-ane0(LLT)/1(BRD)/2(BWR)", "0x2858x0000: TCR[0..15], TTBR, ENABLE_STREAMS, ERROR, ERROR_MASK, TLB_CMD",
     "W/R", "kernel apple-dart probe reset, attach, every iommu map/unmap, IRQ, resume",
     "51 W reset + 4 W attach per DART per boot; 1 TLB_CMD per map/unmap", "apple-dart.c:324-380,514,552-571,1262-1299,1617-1619; baseline B2/B3/L1"),
    ("dart-ane0(LLT)/1(BRD)/2(BWR)", "0x20c,0x220,0x224,0x300,0x308,0x310,0x800-0x83c,0x210,TCR[15],0x700-0x788",
     "none", "-", "0", "apple-dart.c has no tunable, DIAG_LOCK, bypass or PERF code"),
    ("dapf-ane(LLT)", "0x285804000", "none (no DAPF code)", "-", "0", "findings 'DAPF windows unwritten'"),
    ("aic", "IRQ 885 (DART errors)", "kernel IRQ path; ANE IRQ 884 not requested", "-", "-", "apple-dart.c:1262; no request_irq in ane/t6021"),
    ("pmp", "0x28e700000 / ASC 0x28ec00000", "none (pmp node disabled)", "-", "0", "findings section 27, AneSpeed live FDT"),
    ("dcs/amcc/fabric-qos/clpc/smc", "-", "none on the ANE path", "-", "0", "no reference in ane/t6021"),
)

# Ranked diff: what macOS programs that Linux does not (or the reverse),
# by plausibility of moving ANE throughput or clock.
# (rank, block, registers, macOS, Linux, plausibility, reason, evidence)
DIFF = (
    (1, "dart-ane1(BRD), dart-ane2(BWR)", "0x20c, 0x220, 0x224; per-SID 0x800-0x83c",
     "RMW at init (sid0 field[19:16]=6, field[6:0]=0)", "reset default", "high",
     "bulk-read and bulk-write DARTs carry the weight and activation DMA [INFERENCE from the instance names]; 0x220/0x224 carry the same values as m1n1's T8103 ANE DART tunables 0x68/0x6c",
     "trace evt 2857-2878, 2887-2908; ADT dart-tunables-instance-1/2; iBoot j414c tables 0x1de5fc/0x1de710; m1n1 tunables_static.c:92-97"),
    (2, "dart-ane0/1/2", "0x300=enable, 0x308/0x310 = DVA window 0x100_0000_0000..0x3ff_ffff_c000",
     "enabled on BRD/BWR (27.0: also LLT)", "reset default; BO IOVAs below 4 GiB (32-bit DMA mask)", "medium (semantics unknown)",
     "a DVA-qualified DART feature; Linux BOs sit outside the window even if it were enabled",
     "trace evt 2860-2862, 2890-2892; ADT vm-base/vm-size; rtclient_main.c:1910"),
    (3, "pmp (DVFS for SOC0_ANE_SYS)", "PMP ASC 0x28ec00000, SRAM 0x28e700000",
     "PMP firmware runs (ApplePMPFirmware)", "pmp node disabled; ps_pmp powered via dart-ane0", "high for the clock-bound 41%; trace cannot see it",
     "the operating point of the ANE is PMP/firmware-owned on T6021 (no AP pmgr perf path)",
     "findings sections 19 and 27; not in the trace windows"),
    (4, "engine-low(AXI2AF bridge)", "0x038, 0x03c, 0x600, 0x738, 0x798, 0x7f8, 0x900, 0x410, 0x420, 0x430",
     "never written", "written at P-1 (7 with m1n1 T8103 values, 0x410/0x420/0x430 = 0x1100)", "medium",
     "Linux-only writes in the AXI2AF bridge page (AfBridge: bridge base = engine base); AfBridgeRun S3 kept them",
     "boot.h:555-582; m1n1 v1.6.1 src/tunables_static.c:79-89; trace evt 2816-2841 (macOS writes 26 other offsets)"),
    (5, "dart-ane0(LLT)", "0x20c, 0x220, 0x224, 0x300, 0x308, 0x310",
     "27.0 ADT carries them; the 13.5 trace writes none", "reset default", "low-medium",
     "LLT carries the firmware's own fetches and descriptors, not the bulk streams [INFERENCE from the instance names]",
     "ADT dart-tunables-instance-0; trace evt 2843-2850"),
    (6, "dapf-ane(LLT) + TCR[15]=BYPASS", "0x285804000; TCR[15]",
     "TCR[15]=0x2; DAPF allow-list: ANE island ps words, ps_ane_sys, 3 foreign 4-byte registers", "neither; the PMU page is mapped IOVA==PA through sid 0",
     "low-medium", "the firmware's direct MMIO path (power service, cross-IP doorbells); ps_ane_sys and the foreign registers are unreachable on Linux",
     "trace evt 2846/2854/2884; ADT dapf-instance-0; fwload.c:130-152"),
    (7, "pmgr-SET-window", "+0x0, +0x30/+0x34/+0x38 = 0x80000000; +0x2dc = 0xf0040305",
     "written", "never (external-abort class from Linux)", "low (unknown semantics)",
     "+0x0 was applied once without effect on boot; never tested for speed",
     "trace evt 15, 334-507, 2930; findings section 6"),
    (8, "pmgr-ps-ane_sys_mpm", "0x28e084000", "kept off (0x300) under load", "on (always-on in the DT)", "low",
     "power form, not a clock", "findings section 16"),
    (9, "venc-root-ps, venc-leaf-ps", "0x2902803e0, 0x290288000", "cycled, then off", "not touched by 603c79b", "none",
     "VENC rails are off on macOS during ANE use", "trace evt 1063-1201; findings section 17"),
    (10, "dart-ane0/1/2", "DIAG_LOCK 0x210; ERROR_DISABLE=0xffffffff; DISABLE_STREAMS", "written at init/power-down",
     "ERROR_MASK=0, no DIAG_LOCK", "none", "error reporting and teardown only", "trace evt 2843, 2917-2922"),
)


def main(repo, ioreg, out):
    os.makedirs(out, exist_ok=True)
    rep = parse_replay(os.path.join(repo, REPLAY))
    assert len(rep) == 151, len(rep)
    bridge = [r for r in rep if 2816 <= r["evt"] <= 2841]
    assert len(bridge) == 26 and all(block(r["addr"]).startswith("engine-low") for r in bridge)
    assert not [r for r in rep if r["region"] in ("asc-wrapper", "rvbar", "scratch")]

    # Table 1a: every write, with block and phase.
    tsv(os.path.join(out, "macos-trace-writes.tsv"),
        ("evt", "width", "addr", "value", "block", "phase", "register", "linux", "flags"),
        [(r["evt"], r["w"], hex(r["addr"]), "0x%08x" % r["val"], block(r["addr"]),
          phase(r["evt"]), r["reg"].split("  #")[0].strip(), r["linux"], r["flags"]) for r in rep])

    # Table 1b: per block.
    blocks = OrderedDict()
    for r in rep:
        b = blocks.setdefault(block(r["addr"]), dict(n=0, evts=[], regs=OrderedDict(), lin={}, ph=OrderedDict()))
        b["n"] += 1
        b["evts"].append(r["evt"])
        b["regs"].setdefault(r["reg"].split("  #")[0].strip(), []).append("0x%x" % r["val"])
        b["lin"][r["linux"]] = b["lin"].get(r["linux"], 0) + 1
        b["ph"][phase(r["evt"])] = 1
    rows = []
    for name, b in blocks.items():
        regs = "; ".join("%s=%s" % (k, "/".join(v)) for k, v in b["regs"].items())
        lin = " ".join("%s:%d" % kv for kv in sorted(b["lin"].items()))
        rows.append((name, b["n"], "not traced", min(b["evts"]), max(b["evts"]), ",".join(b["ph"]),
                     "before fw start (none traced); before first job (none traced)", lin, regs))
    for name in ("asc-wrapper", "rvbar", "scratch"):
        rows.append((name, 0, "not traced", "-", "-", "-", "traced window, no write", "-", "-"))
    rows.append(("non-ANE pmgr/east ps words (ISP, i2c6, ...)", 3075 - 148, "not traced", "-", 3170,
                 "after ANE power-down", "receipt m2hv-diff-tool: 3075 MMIO events minus the 148 MMIO rows here", "-",
                 "raw trace-135.log not on this host"))
    rows.append(("PMGR-HOOK events (ps_afi and hooked UART0/ATC parents)", 36 - 3, 60, "-", "-", "-",
                 "receipt: 96 hook events (36 W, 60 R); 3 ps_afi W are in this table", "-", "raw log needed for the 60 reads"))
    tsv(os.path.join(out, "macos-trace-blocks.tsv"),
        ("block", "writes", "reads", "first_evt", "last_evt", "phases", "vs_fw_start_and_first_job",
         "linux_same/diff/none", "registers=values (trace order)"), rows)

    # Table 2: Linux footprint + baseline counts per block.
    base = parse_baseline(os.path.join(repo, BASELINE))
    cnt = OrderedDict()
    for r in base:
        k = (block(r["addr"]), r["phase"], r["cond"])
        cnt[k] = cnt.get(k, 0) + 1
    tsv(os.path.join(out, "linux-footprint.tsv"),
        ("block", "addresses", "access", "when", "count", "source"), LINUX)
    tsv(os.path.join(out, "linux-baseline-counts.tsv"),
        ("block", "baseline_phase", "cond", "writes", "in_603c79b_default"),
        [(b, p, c, n, "no" if p.startswith(("L2-venc", "P0", "RTK", "W16", "OBS")) else "yes")
         for (b, p, c), n in cnt.items()])

    # Table 3: DART tunables, ADT (macOS 27 ioreg) vs trace vs iBoot vs Linux.
    props = ioreg_props(ioreg)
    inst = bytes.fromhex(props["instance"])
    names = [inst[i + 8:i + 16].rstrip(b"\0").decode() for i in range(0, len(inst), 16)]
    assert names == ["DARTLLT", "DARTBRD", "DARTBWR", "DAPFLLT"], names
    tw = {(r["addr"]): r["val"] for r in rep}  # last trace write per address
    dbase = {0: 0x285800000, 1: 0x285810000, 2: 0x285820000}
    ib = iboot_tables(os.path.join(repo, IBOOT))
    trows = []
    for i in range(3):
        tun = adt_tunables(props["dart-tunables-instance-%d" % i])
        for off, sz, mask, val in tun:
            a = dbase[i] + off
            tv = tw.get(a)
            known = ""
            if tv is not None:
                assert tv & mask == val & mask, (hex(a), hex(tv))
                known = "bits outside the mask before the write: 0x%08x (of 0x%08x)" % (
                    tv & ~mask & 0xFFFFFFFF, ~mask & 0xFFFFFFFF)
            inb = [s for s, recs in ib if (off, mask, val) in recs]
            trows.append((names[i], hex(a), hex(off), "0x%08x" % mask, "0x%08x" % val,
                          "0x%08x" % tv if tv is not None else "not written",
                          known, "iBoot tables at " + ",".join(hex(s) for s in inb) if inb else "-",
                          "reset default (never written)"))
    tsv(os.path.join(out, "dart-tunables.tsv"),
        ("instance", "pa", "off", "adt_mask", "adt_value", "trace135_value", "derived_reset_bits",
         "iboot_j414c", "linux"), trows)
    assert len(ib) == 2 and all(len(r) == 22 for _, r in ib)
    dapf = bytes.fromhex(props["dapf-instance-0"])
    drows = [struct.unpack_from("<QQ", dapf, i) for i in range(0, len(dapf), 52)]
    tsv(os.path.join(out, "dapf-ane.tsv"), ("start", "end", "note"),
        [(hex(s), hex(e), "DAPF LLT allow-window (ADT dapf-instance-0, macOS 27)") for s, e in drows])
    tsv(os.path.join(out, "diff-ranked.tsv"),
        ("rank", "block", "registers", "macOS", "Linux", "plausibility", "reason", "evidence"), DIFF)

    geo = {k: struct.unpack("<Q", bytes.fromhex(props[k]))[0] for k in ("vm-base", "vm-size")}
    page = struct.unpack("<I", bytes.fromhex(props["page-size"]))[0]
    tlb(out, page, geo)


def tlb(out, page, geo):
    # Inputs: AneSpeed analysis (kDMA 413.6 MB, TileDMA est 1,806 + 1,248 MB),
    # TraceTd family sums (kDMA 411.1 MB, tile est 3,040.9 MB), findings
    # section 24 (61 MB scratch), NativeVsCross / macOS timing. MB = 1e6 B.
    lin_ms, mac_ms = 253.10, 89.3
    kdma, tile, scratch = 413.6e6, (1806 + 1248) * 1e6, 61e6
    total = kdma + tile
    pages_w = kdma / page
    pages_s = scratch / page
    touches = total / page
    gap = (lin_ms - mac_ms) * 1e-3
    lines = [
        "DART geometry (macOS ioreg dart-ane0): page-size %d B, vm-base %#x, vm-size %#x" % (page, geo["vm-base"], geo["vm-size"]),
        "0x308/0x310 tunables in 4 KiB units: %#x .. %#x = vm-base .. vm-base+vm-size" % (geo["vm-base"] >> 12, (geo["vm-base"] + geo["vm-size"]) >> 12),
        "Per encoder CALL (3.47 GB estimate):",
        "  weight pages (kDMA %.1f MB / 16 KiB): %d distinct, each read once per CALL" % (kdma / 1e6, round(pages_w)),
        "  scratch pages (61 MB slot-3 working set): %d distinct" % round(pages_s),
        "  page-equivalents of traffic (%.2f GB / 16 KiB): %d" % (total / 1e9, round(touches)),
        "  TLB entries needed to hold the activation working set: %d" % round(pages_s),
        "Average traffic: Linux %.1f GB/s (%.3f us per 16 KiB), macOS %.1f GB/s (%.3f us per 16 KiB) if the native build moves the same bytes"
        % (total / (lin_ms * 1e-3) / 1e9, lin_ms * 1e3 / touches, total / (mac_ms * 1e-3) / 1e9, mac_ms * 1e3 / touches),
        "Gap %.1f ms = %.3f us per page-equivalent, or %.2f us per distinct page (weights + scratch)"
        % (gap * 1e3, gap * 1e6 / touches, gap * 1e6 / (pages_w + pages_s)),
        "Serialized walk bound, ASSUMED 0.10 us (one DRAM read, upper levels cached) to 0.60 us (four cold reads at 0.15 us) per miss:",
        "  one miss per distinct page:   %.1f-%.1f ms" % ((pages_w + pages_s) * 0.10e-3, (pages_w + pages_s) * 0.60e-3),
        "  one miss per page-equivalent: %.1f-%.1f ms" % (touches * 0.10e-3, touches * 0.60e-3),
    ]
    open(os.path.join(out, "tlb.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
