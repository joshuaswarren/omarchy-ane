#!/usr/bin/env python3
"""ane-h18-pack: rebuild the procedure (5) and TD-property (7) sections of an H18
program image from the compiled program file (magic 0xBEEFFACE), the way the ANE
kernel extension builds them (InitProcedureSection / InitTextPropertySection,
flavor 4), and write a new image.

The H14-derived builder that produced the existing images emits a fixed
procedure section: one directory entry, one 20-byte per-procedure block and
exactly one 12-byte TD record with tdCount=1, whatever the program declares.
The kernel extension derives tdCount from the program's flavor-4 thread state
(payload+0x820) and emits one 12-byte TD record per TD, so a two-TD program
(matvec) loads with a schedule that names one TD.

usage: ane-h18-pack.py BASE_IMAGE HWX OUT_IMAGE
"""
import struct
import sys

TLVOFF = {1: 0x850, 4: 0x860}


def parse_image(f):
    d = open(f, "rb").read()
    _, _, ns, ni, _ = struct.unpack_from("<IIIIQ", d, 0)
    secs, order, ios = {}, [], []
    for i in range(ns):
        sid, _, sz, off, _ = struct.unpack_from("<IIQQQ", d, 0x20 + i * 0x20)
        secs[sid] = d[off:off + sz]
        order.append(sid)
    for i in range(ni):
        sid, dr, sz, do, ds = struct.unpack_from("<IIQQQ", d, 0x20 + (ns + i) * 0x20)
        ios.append((sid, dr, sz, d[do:do + ds] if ds else b""))
    return secs, order, ios


def threads(hwx):
    """the flavor-4 thread payload plus the per-procedure TD count it declares"""
    d = open(hwx, "rb").read()
    n = struct.unpack_from("<I", d, 16)[0]
    p, out = 32, []
    for _ in range(n):
        cmd, cs = struct.unpack_from("<II", d, p)
        if cmd == 0x4 and cs >= 0x840:
            flav = struct.unpack_from("<I", d, p + 8)[0]
            if flav == 4:
                base = p + 16
                w = lambda o: struct.unpack_from("<I", d, base + o)[0]
                out.append({
                    "id": w(0x818), "td_count": w(0x820), "op_td": w(0x824),
                    "pair": (struct.unpack_from("<i", d, base + 0x828)[0],
                             struct.unpack_from("<i", d, base + 0x82c)[0]),
                    "words": w(0x814), "text_va": w(0x800),
                })
        p += cs
    return out


def build_sec5(th):
    """procedure section: count, directory, one 20-byte block plus 12-byte TD records

    Block: {1, 3, 0, 0, tdCount}; record: tdId (running from payload+0x818),
    depIdx (-1), flags (payload+0x824 == 0 -> 4)."""
    td = th["td_count"]
    recs = b""
    for i in range(td):
        recs += struct.pack("<iii", th["id"] + i, -1, 4 if th["op_td"] == 0 else 0)
    block = struct.pack("<IIIII", 1, 3, 0, 0, td) + recs
    body = struct.pack("<IIQQ", 1, 0, 0x18, len(block)) + block
    return body


def build_sec7(th, old):
    """TD-property section: count then 32-byte records; keeps the validated
    H14 field rules (td count at +0xc, text size at +0x20) with tdId/tdCount
    from the thread payload."""
    rec = bytearray(old[:40])
    struct.pack_into("<I", rec, 8, th["id"])
    struct.pack_into("<I", rec, 0xC, th["td_count"])
    return bytes(rec)


def write_image(path, secs, order, ios):
    pos = 0x20 + (len(order) + len(ios)) * 0x20
    body, srec, irec = b"", b"", b""
    for sid in order:
        b = secs[sid]
        pos = (pos + 15) & ~15
        body += b"\0" * (pos - 0x20 - (len(order) + len(ios)) * 0x20 - len(body)) + b
        srec += struct.pack("<IIQQQ", sid, 0, len(b), pos, 0)
        pos += len(b)
    for sid, dr, sz, data in ios:
        if data:
            pos = (pos + 15) & ~15
            body += b"\0" * (pos - 0x20 - (len(order) + len(ios)) * 0x20 - len(body)) + data
            irec += struct.pack("<IIQQQ", sid, dr, sz, pos, len(data))
            pos += len(data)
        else:
            irec += struct.pack("<IIQQQ", sid, dr, sz, 0, 0)
    img = struct.pack("<IIIIQQ", 0x50454E41, 1, len(order), len(ios), 0, 0) + srec + irec + body
    img = img[:16] + struct.pack("<Q", len(img)) + img[24:]
    open(path, "wb").write(img)


def main():
    base, hwx, out = sys.argv[1:4]
    secs, order, ios = parse_image(base)
    th = [t for t in threads(hwx) if t["td_count"]]
    if len(th) != 1:
        sys.exit("expected exactly one flavor-4 thread with TDs, got %d" % len(th))
    th = th[0]
    secs[5] = build_sec5(th)
    secs[7] = build_sec7(th, secs[7])
    write_image(out, secs, order, ios)
    print("%s: td_count=%d op_td=%d pair=%s words=%#x sec5 %d->%d bytes" %
          (out, th["td_count"], th["op_td"], th["pair"], th["words"],
           0x38, len(secs[5])))


if __name__ == "__main__":
    main()
