#!/usr/bin/env python3
"""Per-surface I/O layout of the staged-Qwen ANE programs, from the compiled HWX.

The e5rt io ports on macOS expose only the dense logical view of every surface
(e5rt_port_layout.py); e5rt repacks host-side into the layout the task stream was
compiled against. That layout is recorded in the HWX itself:

- LC_THREAD flavor 1: the channel table, channel k -> base address (0 = task
  stream, 1 = kernel, 3 = scratch, 4.. = I/O surfaces).
- LC_SEGMENT __FVMLIB: one section per surface, __const = input, __data = output.
- LC_SYMTAB: the surface name at each base address, plus a STABS array type per
  surface, e.g. (prog_001 beta [16,1,1])
      t0:t24=ar1;0;1;25=s1024n:ar1;0;16;26=s64c:ar1;0;1;27=s64h:ar1;0;1;28=s2w:5
  = dims n,c,h,w, counts 1,16,1,1, byte strides 1024,64,64,2, element float16.

hwxv2-to-anec.py sized and shaped the ANEC header's surface slots in manifest port
order instead of by channel, and wrote dense strides. `plan` recovers each
surface's ANEC channel by comparing the ANEC task stream with the HWX's, TD by TD
(so any selector renumbering is measured, not assumed), and records per channel
the compiled geometry libane's ane_tile/ane_untile read (nchw[channel] = N, C, H,
W, plane, row bytes), the tile count the packed surface needs, and the
role -> channel bind in manifest port order. `apply` writes that plan into the
ANEC headers (checking each header's sha256 first) and the bind into the export
manifest (src_channels/dst_channels), where staged_qwen_runner.py hands it to
pyane_bind_load.

  io_layout.py plan --export DIR --hwx 'H13/prog_%03d/model.hwx' \
      --anec 'ANEC/prog_%03d.anec' --out io-layout.json
  io_layout.py apply --plan io-layout.json --export DIR --anec 'DIR/programs/prog_%03d.anec'
"""
import argparse, hashlib, json, re, struct, sys

ANEC_HEADER = "<QIIQQII32I192Q"
ANEC_HEADER_SIZE = 0x1000
TILE_SHIFT = 14
FIRST_SURFACE = 4
TILE_COUNT = 32
_DIM = re.compile(r"ar1;0;(\d+);\d+=s(\d+)([a-z]):")
_ELEM = {"5": ("float16", 2)}
# H13 tile-DMA selectors: (DMA configuration register, selector bit shift in word 8)
SELECTORS = ((0x13800, 0, "src"), (0x13804, 6, "src"), (0x17800, 12, "dst"))
DMA_DISABLED = 0x00008880


def _cstr(buf, off):
    return buf[off:buf.index(b"\0", off)].decode()


def parse_hwx(path):
    """Surfaces (name, channel, role, layout) and the task stream of one HWX."""
    with open(path, "rb") as f:
        magic, _, _, _, ncmds, sizeofcmds, _, _ = struct.unpack("<IiiIIIII", f.read(32))
        if magic != 0xBEEFFACE:
            raise ValueError(f"{path}: not an ANE HWX (magic {magic:#x})")
        cmds = f.read(sizeofcmds)
        bars, sections, symtab, text = None, {}, None, None
        off = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", cmds, off)
            if cmd == 0x19:  # LC_SEGMENT_64
                seg = _cstr(cmds[off + 8:off + 24] + b"\0", 0)
                nsects = struct.unpack_from("<I", cmds, off + 64)[0]
                for s in range(nsects):
                    so = off + 72 + 80 * s
                    sect = _cstr(cmds[so:so + 16] + b"\0", 0)
                    addr, ssize, foff = struct.unpack_from("<QQI", cmds, so + 32)
                    if seg == "__FVMLIB":
                        sections[addr] = ("in" if sect == "__const" else "out", ssize)
                    elif (seg, sect) == ("__TEXT", "__text"):
                        text = (foff, ssize)
            elif cmd == 0x4 and bars is None:  # LC_THREAD: flavor 1 = channel table
                flavor, count = struct.unpack_from("<II", cmds, off + 8)
                if flavor == 1:
                    bars = struct.unpack_from(f"<{count // 2}Q", cmds, off + 16)
            elif cmd == 0x2:  # LC_SYMTAB
                symtab = struct.unpack_from("<IIII", cmds, off + 8)
            off += size
        if bars is None or symtab is None or text is None:
            raise ValueError(f"{path}: missing channel table, symtab or __text")
        symoff, nsyms, stroff, strsize = symtab
        f.seek(symoff)
        syms = f.read(16 * nsyms)
        f.seek(stroff)
        strtab = f.read(strsize)
        f.seek(text[0])
        stream = f.read(text[1])
    names, stabs = {}, {}
    for k in range(nsyms):
        strx, ntype, _, _, value = struct.unpack_from("<IBBHQ", syms, 16 * k)
        name = _cstr(strtab, strx)
        if ntype == 0xF:
            names[value] = name
        elif ntype == 0x20 and ":" in name:
            sym, _, stab = name.partition(":")
            stabs[sym] = stab
    surfaces = []
    for channel, addr in enumerate(bars[:TILE_COUNT]):
        if channel < FIRST_SURFACE or addr not in sections:
            continue
        role, section_bytes = sections[addr]
        name = names[addr]
        stab = stabs[name]
        dims = _DIM.findall(stab)
        elem = stab.rsplit(":", 1)[1]
        if not dims or elem not in _ELEM:
            raise ValueError(f"{path}: unparsed surface type {name}:{stab}")
        surfaces.append({
            "name": name, "apple_channel": channel, "role": role,
            "section_bytes": section_bytes, "dtype": _ELEM[elem][0],
            "element_size": _ELEM[elem][1], "dims": "".join(d[2] for d in dims),
            "shape": [int(d[0]) for d in dims], "strides": [int(d[1]) for d in dims],
        })
    return surfaces, stream


def anec_geometry(s):
    """[N, C, H, W, plane, row] bytes for libane's ane_tile; raises when the
    compiled layout is outside what that 6-field geometry expresses."""
    (n, c, h, w), (sn, sc, sh, sw) = s["shape"], s["strides"]
    if s["dims"] != "nchw" or sw != s["element_size"] or sh < w * sw or \
            sc % sh or sc // sh < h or (n > 1 and sn != c * sc):
        raise ValueError(f"{s['name']}: layout {s['dims']} {s['shape']}/{s['strides']} "
                         "not expressible as ANEC nchw")
    return [n, c, h, w, sc, sh]


def _word(buf, off, i):
    return struct.unpack_from("<I", buf, off + 4 * i)[0]


def walk_tasks(stream, first_bytes, count):
    """Per task: (offset, bytes, {slot: (channel, live)}) following libane's walk."""
    out, off, size = [], 0, first_bytes
    for i in range(count):
        regs, idx = {}, 10 + (1 if (_word(stream, off, 9) & 3) == 3 else 0)
        words = size // 4
        while idx < words:
            head = _word(stream, off, idx)
            n, base = (head >> 26) + 1, head & 0x03FFFFFF
            for k in range(n):
                if idx + 1 + k < words:
                    regs[base + 4 * k] = _word(stream, off, idx + 1 + k)
            idx += 1 + n
        sel = _word(stream, off, 8)
        slots = {slot: ((sel >> shift) & 0x1F, regs.get(reg, DMA_DISABLED) != DMA_DISABLED)
                 for slot, (reg, shift, _) in enumerate(SELECTORS)}
        out.append((off, size, slots))
        if i + 1 == count:
            break
        size = (((_word(stream, off, 1) >> 16) & 0x1FF) + 1) * 4
        off = _word(stream, off, 7)
    return out


def libane_bind(header, tasks):
    """Port of libane ane_bind_init (diagnostic): role order libane derives."""
    tiles, src_count, dst_count = header["tiles"], header["src_count"], header["dst_count"]
    is_src, is_dst = set(), set()
    for _, _, slots in tasks:
        for slot, (ch, live) in slots.items():
            if live and FIRST_SURFACE <= ch < TILE_COUNT and tiles[ch]:
                (is_dst if SELECTORS[slot][2] == "dst" else is_src).add(ch)
    dst = [c for c in range(FIRST_SURFACE, TILE_COUNT) if c in is_dst]
    src = [c for c in range(FIRST_SURFACE, TILE_COUNT) if c not in is_dst and c in is_src]
    for c in range(FIRST_SURFACE, TILE_COUNT):
        if len(dst) >= dst_count and len(src) >= src_count:
            break
        if c in is_dst or c in is_src or not tiles[c]:
            continue
        (dst if len(dst) < dst_count else src).append(c)
    if len(src) != src_count or len(dst) != dst_count:
        return None
    return {"src": src, "dst": dst}


def read_anec_header(path):
    with open(path, "rb") as f:
        raw = f.read(struct.calcsize(ANEC_HEADER))
        f.seek(ANEC_HEADER_SIZE)
        v = struct.unpack(ANEC_HEADER, raw)
        hdr = {"size": v[0], "td_size": v[1], "td_count": v[2], "tsk_size": v[3],
               "krn_size": v[4], "src_count": v[5], "dst_count": v[6],
               "tiles": list(v[7:39]), "nchw": [list(v[39 + 6 * k:45 + 6 * k]) for k in range(32)]}
        stream = f.read(hdr["tsk_size"])
    return hdr, raw, stream


def program_layout(program, hwx_path, anec_path):
    surfaces, hwx_stream = parse_hwx(hwx_path)
    hdr, raw, anec_stream = read_anec_header(anec_path)
    t_hwx = walk_tasks(hwx_stream, hdr["td_size"], hdr["td_count"])
    t_anec = walk_tasks(anec_stream, hdr["td_size"], hdr["td_count"])
    remap = {}
    for (o1, s1, a), (o2, s2, b) in zip(t_hwx, t_anec):
        if (o1, s1) != (o2, s2):
            raise ValueError(f"{anec_path}: task chain diverges from {hwx_path} at {o1:#x}")
        for slot in a:
            ca, cb = a[slot][0], b[slot][0]
            if ca >= FIRST_SURFACE and remap.setdefault(ca, cb) != cb:
                raise ValueError(f"{anec_path}: channel {ca} maps to {remap[ca]} and {cb}")
    by_name = {s["name"]: s for s in surfaces}
    ports = [(p["port"], "in") for p in program["srcs"]] + \
            [(p["port"], "out") for p in program["dsts"]] + \
            [(p["out_port"], "out") for p in program["states"]]
    if sorted(by_name) != sorted(p for p, _ in ports):
        raise ValueError(f"{hwx_path}: surfaces {sorted(by_name)} != manifest ports {sorted(p for p, _ in ports)}")
    if hdr["src_count"] != sum(r == "in" for _, r in ports) or \
            hdr["dst_count"] != sum(r == "out" for _, r in ports):
        raise ValueError(f"{anec_path}: header src/dst counts disagree with the manifest")
    rows = []
    for port, role in ports:
        s = by_name[port]
        if s["role"] != role:
            raise ValueError(f"{hwx_path}: {port} is an HWX {s['role']} but a manifest {role}")
        if s["apple_channel"] in remap:
            anec_ch = remap[s["apple_channel"]]
        elif all(k == v for k, v in remap.items()):
            anec_ch = s["apple_channel"]   # no task names it; the stream kept Apple's numbering
        else:
            raise ValueError(f"{anec_path}: {port} is unnamed in a renumbered stream")
        geom = anec_geometry(s)
        packed = s["shape"][0] * s["strides"][0]
        rows.append({"port": port, "role": role, "apple_channel": s["apple_channel"],
                     "anec_channel": anec_ch, "named_in_stream": s["apple_channel"] in remap,
                     "shape": s["shape"], "strides": s["strides"], "nchw": geom,
                     "packed_bytes": packed, "section_bytes": s["section_bytes"],
                     "tiles_before": hdr["tiles"][anec_ch],
                     "tiles": max(hdr["tiles"][anec_ch], -(-packed >> TILE_SHIFT)),
                     "nchw_before": hdr["nchw"][anec_ch]})
    chans = [r["anec_channel"] for r in rows]
    if len(set(chans)) != len(chans):
        raise ValueError(f"{anec_path}: two surfaces resolve to one channel: {chans}")
    return {
        "hwx": hwx_path, "anec": anec_path,
        "anec_header_sha256_before": hashlib.sha256(raw).hexdigest(),
        "surfaces": rows,
        "bind": {"src": [r["anec_channel"] for r in rows if r["role"] == "in"],
                 "dst": [r["anec_channel"] for r in rows if r["role"] == "out"]},
        "libane_derived_bind": libane_bind(hdr, t_anec),
    }


def header_bytes(hdr):
    return struct.pack(ANEC_HEADER, hdr["size"], hdr["td_size"], hdr["td_count"],
                       hdr["tsk_size"], hdr["krn_size"], hdr["src_count"], hdr["dst_count"],
                       *hdr["tiles"], *[x for row in hdr["nchw"] for x in row])


def plan(a):
    man = json.load(open(f"{a.export}/manifest.json"))
    out = {"programs": []}
    for ci, program in enumerate(man["programs"]):
        lay = program_layout(program, a.hwx % ci, a.anec % ci)
        hdr, _, _ = read_anec_header(lay["anec"])
        for r in lay["surfaces"]:
            hdr["tiles"][r["anec_channel"]] = r["tiles"]
            hdr["nchw"][r["anec_channel"]] = r["nchw"]
        lay["anec_header_sha256_after"] = hashlib.sha256(header_bytes(hdr)).hexdigest()
        out["programs"].append(lay)
        moved = [f"{r['port']}@{r['anec_channel']}"
                 + ("" if r["nchw"] == r["nchw_before"] else f" nchw {r['nchw_before']}->{r['nchw']}")
                 + ("" if r["tiles"] == r["tiles_before"] else f" tiles {r['tiles_before']}->{r['tiles']}")
                 for r in lay["surfaces"]]
        same = lay["libane_derived_bind"] == lay["bind"]
        print(f"prog_{ci:03d} bind src={lay['bind']['src']} dst={lay['bind']['dst']} "
              f"libane-derived={'same' if same else lay['libane_derived_bind']} | " + "; ".join(moved), flush=True)
    json.dump(out, open(a.out, "w"), indent=1)
    print(f"wrote {a.out}")


def apply(a):
    """Write a plan into the ANEC headers (in place) and the export manifest."""
    progs = json.load(open(a.plan))["programs"]
    man_path = f"{a.export}/manifest.json"
    man = json.load(open(man_path))
    if len(progs) != len(man["programs"]):
        raise ValueError(f"plan has {len(progs)} programs, manifest {len(man['programs'])}")
    for ci, lay in enumerate(progs):
        path = a.anec % ci
        hdr, raw, _ = read_anec_header(path)
        have = hashlib.sha256(raw).hexdigest()
        if have == lay["anec_header_sha256_before"]:
            for r in lay["surfaces"]:
                hdr["tiles"][r["anec_channel"]] = r["tiles"]
                hdr["nchw"][r["anec_channel"]] = r["nchw"]
            new = header_bytes(hdr)
            assert hashlib.sha256(new).hexdigest() == lay["anec_header_sha256_after"]
            with open(path, "r+b") as f:
                f.write(new)
            state = "patched"
        elif have == lay["anec_header_sha256_after"]:
            state = "already patched"
        else:
            raise ValueError(f"{path}: header sha256 {have} is neither the planned input nor output")
        man["programs"][ci]["src_channels"] = lay["bind"]["src"]
        man["programs"][ci]["dst_channels"] = lay["bind"]["dst"]
        print(f"prog_{ci:03d} {state}: src={lay['bind']['src']} dst={lay['bind']['dst']}")
    json.dump(man, open(man_path, "w"), indent=1)
    print(f"bind written to {man_path}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--export", required=True, help="staged export dir (manifest.json)")
    p.add_argument("--hwx", required=True, help="pattern with %%03d")
    p.add_argument("--anec", required=True, help="pattern with %%03d")
    p.add_argument("--out", required=True)
    p = sub.add_parser("apply")
    p.add_argument("--plan", required=True)
    p.add_argument("--export", required=True, help="export dir whose manifest.json gets the bind")
    p.add_argument("--anec", required=True, help="pattern with %%03d (headers patched in place)")
    a = ap.parse_args()
    (plan if a.cmd == "plan" else apply)(a)


if __name__ == "__main__":
    sys.exit(main())
