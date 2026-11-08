#!/usr/bin/env python3
"""Decode AppleT8140ANEHAL af_aneaxi2af tunable (figs) tables.

Chain/fixup base for this kernelcache: VA = 0xfffffe0007004000 + target
where target = u64 & 0xfffffffff (36-bit).  Verified: element ptr field
0x00180000_0061c3c0 -> VA 0xfffffe00076203c0 (HAL __const).
"""
import struct, json, sys

D = open("/var/tmp/neo/kernelcache.bin", "rb").read()
KB = 0xfffffe0007004000

def va2off(va):
    return va - KB

def load(va, n):
    o = va2off(va)
    return D[o:o+n]

def u32(va):
    return struct.unpack_from("<I", load(va, 4))[0]

def u64(va):
    return struct.unpack_from("<Q", load(va, 8))[0]

def ptr_target(v):
    """Return (va, next_units, bind, auth) for a chained-fixup u64."""
    tgt = v & 0xFFFFFFFFF          # 36 bits
    high8 = (v >> 36) & 0xFF
    nxt = (v >> 51) & 0xFFF
    return (KB + tgt + (high8 << 56), nxt)

def symbol_map():
    s = json.load(open("/var/tmp/neo/syms_HAL.json"))
    m = {}
    for e in s:
        m.setdefault(e["value"], e["name"])
    return m

SYMS = symbol_map()

TABLE_VA = 0xfffffe000813baf0   # "figs_1.figs"
TABLES = {
    0xfffffe000813baf0: "_tunableh_af_aneaxi2af_figs_1.figs  (applied by 0x97990e0 figs_13_apply_to)",
    0xfffffe000813bc10: "_tunableh_af_aneaxi2af_figs_4.figs  (applied by 0x979963c figs_4_apply_to)",
    0xfffffe000813bc28: "_tunableh_af_aneaxi2af_figs_3.figs  (applied by 0x9799660 figs_3_apply_to)",
    0xfffffe000813bc58: "_tunableh_af_aneaxi2af_figs_2.figs  (applied by 0x9799684 figs_2_apply_to)",
}

def decode_table(tva):
    out = []
    va = tva
    while True:
        raw = u64(va)
        if raw == 0:
            return out, va + 8, va   # entries, end-of-array (exclusive), terminator va
        eva, nxt = ptr_target(raw)
        w0 = u32(eva)
        width = w0 >> 28
        off = w0 & 0xFFFFFFF
        pv = u64(eva + 4)          # the 12-byte element's pointer field
        pva, pnxt = ptr_target(pv)
        # mask/value record: mask @pva (width bytes), value @pva+width,
        # expression fn ptr @pva+2*width
        if width == 4:
            mask = u32(pva)
            val = u32(pva + 4)
            expr = u64(pva + 8)
        elif width == 2:
            mask = struct.unpack_from("<H", load(pva, 2))[0]
            val = struct.unpack_from("<H", load(pva + 2, 2))[0]
            expr = u64(pva + 4)
        elif width == 1:
            mask = load(pva, 1)[0]
            val = load(pva + 1, 1)[0]
            expr = u64(pva + 2)
        elif width == 8:
            mask = u64(pva)
            val = u64(pva + 8)
            expr = u64(pva + 16)
        else:
            raise SystemExit("bad width %d at elem %#x" % (width, eva))
        out.append(dict(slot=va, raw=raw, nxt=nxt, elem=eva, w0=w0, width=width,
                        off=off, pptr=pv, prec=pva, mask=mask, value=val,
                        expr=expr, expr_addr=pva + 2 * width,
                        sym=SYMS.get(eva, "?")))
        va += 8

def main():
    allrows = []
    for tva, label in TABLES.items():
        rows, endva, termva = decode_table(tva)
        print("=" * 100)
        print("TABLE %#x  %s" % (tva, label))
        print("  extent: [%#x, %#x) = %d bytes = %d slots; entries=%d; terminator @%#x = 0"
              % (tva, endva, endva - tva, (endva - tva) // 8, len(rows), termva))
        print("  %-4s %-14s %-12s %-6s %-6s %-8s %-10s %-10s %s" %
              ("#", "element VA", "w0", "width", "off", "mask", "value", "expr", "sym"))
        for i, r in enumerate(rows):
            print("  %-4d %012x %08x %-6d %#06x %#010x %#010x %-10d %s"
                  % (i, r["elem"], r["w0"], r["width"], r["off"], r["mask"],
                     r["value"], r["expr"], r["sym"]))
            allrows.append((tva, i, r))
        print()
    return allrows

if __name__ == "__main__":
    main()
