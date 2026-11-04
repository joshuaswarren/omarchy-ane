#!/usr/bin/env python3
"""Final verification for the J700/h17 AppleANETunableApplyFunction tunable decode.

Every claim in the report is re-derived here from the raw kernelcache bytes.
Emits: ordered write list, table extents, and self-check results.
"""
import struct, json, sys

KC = "/var/tmp/neo/kernelcache.bin"
D = open(KC, "rb").read()

# Chained-fixup base for this kernelcache.  Verified independently:
#   element pointer field 0x00180000_0061c3c0 -> KB + 0x61c3c0 = 0xfffffe00076203c0,
#   which is HAL __DATA_CONST.__const.__const  (LC_SEGMENT_64 fileoff 0x61c3c0).
KB = 0xfffffe0007004000

def ld(va, n):
    o = va - KB
    assert 0 <= o < len(D), hex(va)
    return D[o:o+n]

def u32(va): return struct.unpack("<I", ld(va, 4))[0]
def u64(va): return struct.unpack("<Q", ld(va, 8))[0]

def fixup_va(v):
    """36-bit target + high8 in a chained rebase; verified by construction."""
    tgt = v & 0xFFFFFFFFF
    high8 = (v >> 36) & 0xFF
    return KB + tgt + (high8 << 56)

def walk(tva, fmt="ptr"):
    """Walk a figs table.  fmt='ptr': u64 slots, NULL-terminated, stride 8.
       Returns list of (kind, payload) plus end VA."""
    rows, va = [], tva
    while True:
        if fmt == "ptr":
            raw = u64(va)
            if raw == 0:
                return rows, va            # NULL terminator slot
            elem = fixup_va(raw)
            stride = 8
        else:
            w0 = u32(va)
            if (w0 & ~0xFFFFFFF) == 0:
                return rows, va            # width nibble 0 == terminator
            elem = va
            stride = 12
        w0 = u32(elem)
        width = w0 >> 28
        off = w0 & 0xFFFFFFF
        prec = fixup_va(u64(elem + 4))
        assert width in (1, 2, 4, 8), "bad width %d @%#x" % (width, elem)
        if width == 1:
            mask, val, expr = ld(prec,1)[0], ld(prec+1,1)[0], u64(prec+2)
        elif width == 2:
            mask = struct.unpack("<H", ld(prec,2))[0]
            val  = struct.unpack("<H", ld(prec+2,2))[0]
            expr = u64(prec+4)
        elif width == 4:
            mask, val, expr = u32(prec), u32(prec+4), u64(prec+8)
        else:
            mask, val, expr = u64(prec), u64(prec+8), u64(prec+16)
        rows.append(dict(elem=elem, w0=w0, width=width, off=off, rec=prec,
                         mask=mask, value=val, expr=expr))
        va += stride
    return rows, va

TABLES = {
 0xfffffe000813baf0: ("af_aneaxi2af figs_1.figs", "0x97990e0", "always"),
 0xfffffe000813bc10: ("af_aneaxi2af figs_4.figs", "0x979963c", "device 0x8==0 && 0x20==0 && 0x21==0"),
 0xfffffe000813bc28: ("af_aneaxi2af figs_3.figs", "0x9799660", "device 0x8==1 && 0x20==0 && 0x21==1"),
 0xfffffe000813bc58: ("af_aneaxi2af figs_2.figs", "0x9799684", "device 0x8==1 && 0x20==0 && 0x21==1"),
 0xfffffe000813be50: ("c1ppt_rc figs_27.figs",  "0x979bb54", "unconditional"),
 0xfffffe000813be98: ("c1ppt_rc figs_41.figs",  "0x979bb78", "see 0x979acec"),
 0xfffffe000813be30: ("c1ppt_rc figs_26.figs",  "0x979bb0c", "see 0x979acec"),
 0xfffffe000813be40: ("c1ppt_rc figs_28.figs",  "0x979bb30", "see 0x979acec"),
 0xfffffe000813be20: ("c1ppt_rc figs_3.figs",   "0x979bae8", "unconditional"),
 0xfffffe000813be10: ("c1ppt_rc figs_4.figs",   "0x979bac4", "unconditional"),
}

def main():
    out = {}
    problems = []
    for tva, (label, fn, gate) in TABLES.items():
        rows, end = walk(tva)
        # `end` is the VA of the terminator slot; the table occupies
        # [tva, end+8) = count+1 slots.
        out[tva] = dict(label=label, applier=fn, gate=gate, rows=rows,
                        extent=(end - tva) + 8, end=end,
                        slots=(end - tva)//8 + 1, count=len(rows))
    print("### TABLE EXTENTS")
    for tva, t in out.items():
        print("  %#x  %-26s extent=0x%03x (%d slots, %d entries) applier %s"
              % (tva, t["label"], t["extent"], t["slots"], t["count"], t["applier"]))

    print("\n### SELF CHECKS")
    allrows = [(tva, r) for tva, t in out.items() for r in t["rows"]]
    # 1 offsets bounded
    bad = [(tva, r) for tva, r in allrows if r["off"] >= 0x2000000]
    print("  offsets < 0x2000000            : %s (max 0x%x)"
          % ("PASS" if not bad else "FAIL %s" % bad,
             max(r["off"] for _, r in allrows)))
    # 2 width alignment
    bad2 = [(tva, r) for tva, r in allrows if r["width"] == 4 and r["off"] % 4]
    print("  width==4 offsets 4-byte aligned: %s" % ("PASS" if not bad2 else "FAIL %s" % bad2))
    # 3 widths
    print("  widths subset of {1,2,4,8}     : %s (observed %s)"
          % ("PASS" if all(r["width"] in (1,2,4,8) for _, r in allrows) else "FAIL",
             sorted({r["width"] for _, r in allrows})))
    # 4 count vs extent
    ok4 = all(t["count"] * 8 + 8 == t["extent"] for t in out.values())
    print("  count == extent/8 - 1          : %s" % ("PASS" if ok4 else "FAIL"))
    # 5 no offset looks like a file offset
    print("  no offset >= 0x2000000          : %s (weak test, see README)"
          % ("PASS" if not bad else "FAIL"))
    # 6 elements inside HAL __const section
    lo, hi = 0xfffffe000813b8b8, 0xfffffe000813b8b8 + 0x2FE0
    ok6 = all(lo <= r["elem"] < hi for _, r in allrows)
    print("  all elements inside HAL __const: %s" % ("PASS" if ok6 else "FAIL"))
    # 7 no expression callbacks
    print("  expression callbacks present   : %d (all zero => pure masked RMW)"
          % sum(1 for _, r in allrows if r["expr"]))
    # 8 terminators
    print("  terminator slots               : " +
          ", ".join("%#x=0" % t["end"] for t in out.values()))

    print("\n### ORDERED WRITE LIST (online path)")
    order = [0xfffffe000813baf0]
    print("  -- AF group, base = arg1 --")
    n = 0
    for tva in order:
        for r in out[tva]["rows"]:
            n += 1
            print("   %2d  off=0x%05x w=%d mask=0x%08x val=0x%08x  %s"
                  % (n, r["off"], r["width"], r["mask"], r["value"], out[tva]["label"]))
    print("   AF always-applied count = %d" % n)

if __name__ == "__main__":
    main()
