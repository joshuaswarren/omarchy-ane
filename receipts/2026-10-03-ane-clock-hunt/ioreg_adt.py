#!/usr/bin/env python3
"""Decode the ANE perf/clock/PS items of a macOS `ioreg -l -w0` device-tree text dump.

Usage: ioreg_adt.py [--selfcheck] <ioreg.txt> [<pmgr-ps.bin> ...]
  pmgr-ps.bin: ANERegDump captures of PA 0x28e080000.. (idle, load...); words are printed per device.
Record layouts: m1n1 proxyclient/m1n1/adt.py:53-147 (PMGRPSRegs, PMGRPerfRegs, PMGRDevices, PMGRClocks,
PMGRPowerDomains, PMGREvents), src/pmgr.c:23-48,120-138 (device PS address). Fields not named there are printed raw.
Every item carries its ioreg line (L<n>) for citation.
"""
import re
import struct
import sys

PROP = re.compile(r'^[ |]*"([^"]+)" = (.*)$')
NODE = re.compile(r"^[ |]*\+-o (\S+)")
PS_BASE = 0x28E080000  # regdump index.json pmgr_pa
AIO_BASE = 0x200000000  # /arm-io ranges: child 0x0 -> 0x2_0000_0000 (pmgr reg[0] 0x8e080000 = unit address)


def parse(path):
    nodes, stack = {}, []
    for ln, line in enumerate(open(path, errors="replace"), 1):
        m = NODE.match(line)
        if m:
            depth = line.index("+-o")
            while stack and stack[-1][0] >= depth:
                stack.pop()
            stack.append((depth, m.group(1).split("@")[0]))
            nodes.setdefault("/".join(n for _, n in stack), {"_line": ln})
            continue
        m = PROP.match(line)
        if m and stack:
            v = m.group(2)
            if v.startswith('<"') and v.endswith('">'):
                val = v[2:-2].encode()
            elif re.fullmatch(r"<[0-9a-f]*>", v):
                val = bytes.fromhex(v[1:-1])
            else:
                val = v
            nodes["/".join(n for _, n in stack)][m.group(1)] = (ln, val)
    return nodes


def find(nodes, *suffixes):
    for s in suffixes:
        hits = [k for k in nodes if k == s or k.endswith("/" + s)]
        if len(hits) == 1:
            return nodes[hits[0]]
    return None


def u32s(b):
    return list(struct.unpack("<%dI" % (len(b) // 4), b[: len(b) // 4 * 4]))


def cstr(b):
    return b.split(b"\0")[0].decode(errors="replace")


def decode(nodes, caps):
    out, facts = [], {}
    p = out.append
    pm = find(nodes, "arm-io/pmgr", "pmgr")
    rb = pm["reg"][1]
    preg = [(struct.unpack_from("<Q", rb, i)[0] + AIO_BASE, struct.unpack_from("<Q", rb, i + 8)[0]) for i in range(0, len(rb), 16)]
    p("== pmgr node L%d, reg L%d: %d windows" % (pm["_line"], pm["reg"][0], len(preg)))
    for i, (a, s) in enumerate(preg):
        p("  reg[%2d] 0x%09x +0x%x" % (i, a, s))

    ln, pd = pm["perf-domains"]
    p("== perf-domains L%d: %d x 28 B (bytes b0..b3, u32 x2, name[16]; b0 = voltage-states index by ECPU/PCPU/ANE match)" % (ln, len(pd) // 28))
    for i in range(0, len(pd), 28):
        b = struct.unpack_from("<BBBBII", pd, i)
        p("  [%2d] b=%s u32=%d,%d %s" % (i // 28, list(b[:4]), b[4], b[5], cstr(pd[i + 12 : i + 28])))

    vs = {}
    for key in sorted(k for k in pm if k.startswith("voltage-states")):
        ln, v = pm[key]
        w = u32s(v)
        vs[key] = [(w[j], w[j + 1]) for j in range(0, len(w) - 1, 2)]
        p("== %s L%d: %s" % (key, ln, " ".join("%d/%d" % t for t in vs[key])))
    facts["vs"] = vs

    pw = u32s(pm["perf-regs"][1])
    perf = [pw[i : i + 4] for i in range(0, len(pw), 4)]
    p("== perf-regs L%d (reg, offset, size, unk) -> base = pmgr reg[reg] + offset" % pm["perf-regs"][0])
    for i, (r, off, size, unk) in enumerate(perf):
        p("  perf[%2d] reg=%2d off=0x%05x size=0x%03x unk=0x%08x -> 0x%09x" % (i, r, off, size, unk, preg[r][0] + off))

    def slot(block, idx):
        # INFERENCE: base + 0x100 + idx*0x10 (m1n1 tools/dump_pmgr.py:75 uses the same stride but omits perf.offset,
        # which on this chip would alias other devices' PS words; "size" equals the slot count used by the events).
        r, off = perf[block][0], perf[block][1]
        return preg[r][0] + off + 0x100 + idx * 0x10

    for key in ("clocks", "power-domains"):
        ln, v = pm[key]
        p("== %s L%d: %d x 24 B" % (key, ln, len(v) // 24))
        for i in range(0, len(v), 24):
            a, b, c, d = struct.unpack_from("<BBBB", v, i)
            pidx, pblk, cid = (a, b, d) if key == "clocks" else (b, c, d)
            p("  id=%3d %-16s perf=%d:0x%02x slot=0x%09x" % (cid, cstr(v[i + 8 : i + 24]), pblk, pidx, slot(pblk, pidx)))

    ln, ev = pm["events"]
    p("== events L%d: %d x 24 B (ANE/FAB/SOC/DCS/CLVR names only)" % (ln, len(ev) // 24))
    facts["events"] = {}
    for i in range(0, len(ev), 24):
        u1, u2, u3, eid, p2i, p2b, pi, pb = struct.unpack_from("<8B", ev, i)
        name = cstr(ev[i + 8 : i + 24])
        if re.search("ANE|FAB|SOC_|DCS_|CLVR_EXT", name):
            s2 = " perf2=%d:0x%02x slot2=0x%09x" % (p2b, p2i, slot(p2b, p2i)) if p2i else ""
            p("  id=%3d %-16s unk=%x/%x/%x perf=%d:0x%02x slot=0x%09x%s" % (eid, name, u1, u2, u3, pb, pi, slot(pb, pi), s2))
            facts["events"][name] = (pb, slot(pb, pi))

    if "hw-dpe-reg" in pm:
        ln, hd = pm["hw-dpe-reg"]
        p("== hw-dpe-reg L%d: %d x 40 B (u64 addr, 16 raw bytes, name[16])" % (ln, len(hd) // 40))
        for i in range(0, len(hd), 40):
            p("  0x%09x %s %s" % (struct.unpack_from("<Q", hd, i)[0], hd[i + 8 : i + 24].hex(), cstr(hd[i + 24 : i + 40])))

    ln, dv = pm["devices"]
    psw = u32s(pm["ps-regs"][1])
    psr = [psw[i : i + 3] for i in range(0, len(psw), 3)]
    p("== ps-regs L%d (reg, offset, mask): %s" % (pm["ps-regs"][0], " ".join("[%d,0x%x,0x%x]" % tuple(t) for t in psr)))
    recs = [dv[i : i + 48] for i in range(0, len(dv), 48)]
    u8id = recs[0][3] != recs[1][3]
    p("== devices L%d: %d x 48 B, %s ids; flags bit5 perf, bit4 no_ps, bit1 notify_pmp (m1n1 adt.py:73-82); words = %s" % (ln, len(recs), "u8" if u8id else "u16", "idle/load captures in argv order" if caps else "none"))
    facts["dev"] = {}
    for r in recs:
        flags = r[0]
        parents = (r[4], r[5]) if u8id else struct.unpack_from("<HH", r, 4)
        did = r[3] if u8id else struct.unpack_from("<H", r, 26)[0]
        name = cstr(r[32:48])
        pa, words = None, ""
        if not flags & 0x10:
            ridx, roff, _ = psr[r[11]]
            pa = preg[ridx][0] + roff + (r[10] << 3)
            o = pa - PS_BASE
            if caps and 0 <= o < len(caps[0]):
                words = " ".join("0x%08x" % struct.unpack_from("<I", c, o)[0] for c in caps)
        perfs = " perf=%d:0x%02x slot=0x%09x" % (r[9], r[8], slot(r[9], r[8])) if flags & 0x20 else ""
        facts["dev"][name] = pa
        if re.search("ANE|AFI|AFC|AFNC0|AMCC0|DCS_00|PMP|SOC", name) or (caps and len(set(words.split())) > 1):
            p("  id=%3d %-16s flags=0x%02x parents=%s pa=%s %s%s" % (did, name, flags, list(parents), "0x%09x" % pa if pa else "no_ps", words, perfs))

    aio = find(nodes, "device-tree/arm-io")
    if aio:
        freqs = u32s(aio["clock-frequencies"][1])
        rr = aio["clock-frequencies-regs"][1]
        fregs = struct.unpack("<%dQ" % (len(rr) // 8), rr)
        p("== /arm-io clock-frequencies L%d (%d) clock-frequencies-regs L%d (u64: type<<56 | PA)" % (aio["clock-frequencies"][0], len(freqs), aio["clock-frequencies-regs"][0]))
        n = find(nodes, "arm-io/ane0")
        ids = u32s(n["clock-ids"][1])
        p("  ane0 clock-ids L%d: %s (ids >= 0x100 index the boot clock list, m1n1 tools/dump_pmgr.py:140)" % (n["clock-ids"][0], ["0x%x" % c for c in ids]))
        facts["ane_clk"] = []
        for c in ids:
            i = c - 256
            p("    boot clk[%d] %d Hz type=0x%02x PA=0x%x" % (i, freqs[i], fregs[i] >> 56, fregs[i] & 0xFFFFFFFFFFFFFF))
            facts["ane_clk"].append(fregs[i] & 0xFFFFFFFFFFFFFF)
        p("  ane0 reg L%d: %s" % (n["reg"][0], ["0x%x+0x%x" % (struct.unpack_from("<Q", n["reg"][1], i)[0] + AIO_BASE, struct.unpack_from("<Q", n["reg"][1], i + 8)[0]) for i in range(0, len(n["reg"][1]), 16)]))
        p("  ane0 AAPL,phandle L%d: 0x%x" % (n["AAPL,phandle"][0], u32s(n["AAPL,phandle"][1])[0]))

    clpc = find(nodes, "arm-io/pmgr/clpc")
    if clpc:
        f = clpc["function-ane_perf_ctr"][1]
        p("== clpc L%d: function-ane_perf_ctr L%d = phandle 0x%x '%s'; soc-devices L%d = %s" % (clpc["_line"], clpc["function-ane_perf_ctr"][0], u32s(f)[0], f[4:8][::-1].decode(), clpc["soc-devices"][0], u32s(clpc["soc-devices"][1])))

    nub = find(nodes, "arm-io/pmp/iop-pmp-nub")
    if nub:
        ln, d = nub["dvfs-domain"]
        p("== iop-pmp-nub dvfs-domain L%d: %d x 28 B (u32 id, u32 f2, u32 f3, name[16]) %s" % (ln, len(d) // 28, ["%d:%s" % (struct.unpack_from("<I", d, i)[0], cstr(d[i + 12 : i + 28])) for i in range(0, len(d), 28)]))
        facts["pmp_dvfs"] = [cstr(d[i + 12 : i + 28]) for i in range(0, len(d), 28)]
    return out, facts


def selfcheck(facts):
    """Anchors from independent evidence: the decoder must reproduce them or it is wrong."""
    # ANE PS words the regdump gate reads by offset (ranges-gapwin.txt island 0x260/0x2e0/0x4008..0x4030).
    assert facts["dev"]["ANE_SYS"] == 0x28E080260 and facts["dev"]["ANE_CPU"] == 0x28E0802E0
    assert facts["dev"]["ANE_SET4"] == 0x28E084030
    # E-cluster ladder powermetrics prints on this Mac: 912..2424 MHz (05-powermetrics/powermetrics.txt:15).
    assert [f // 1000000 for f, _ in facts["vs"]["voltage-states1-sram"]] == [912, 1284, 1752, 2004, 2256, 2424]
    # The ANE ladder named by perf-domains b0=8.
    assert [f // 1000000 for f, _ in facts["vs"]["voltage-states8"]] == [600, 852, 1104, 1356, 1596, 1848, 2100]
    assert facts["events"]["ANE0_ADCLK_TRG"][0] == 2
    print("selfcheck OK")


if __name__ == "__main__":
    args = sys.argv[1:]
    check = args[0] == "--selfcheck"
    args = args[1:] if check else args
    lines, facts = decode(parse(args[0]), [open(f, "rb").read() for f in args[1:]])
    print("\n".join(lines))
    if check:
        selfcheck(facts)
