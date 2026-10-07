#!/usr/bin/env python3
"""collect-m1n1.py: read the T8112 ANE values that iBoot sets at boot, over the m1n1 proxy.

Read only: it writes no device register. Read README.md before you use it.

  live:   PYTHONPATH=<m1n1>/proxyclient M1N1DEVICE=/dev/ttyACM0 \\
              python3 collect-m1n1.py --archive h14_ane_fw_bia_j4xx.macho --out DIR
  replay: python3 collect-m1n1.py --replay CAPTURE/bytes --soc 0x6021 \\
              --archive t602x_ane0_fw_selene_rc4x.macho --out DIR

A live run (1) dumps the ADT nodes /arm-io/ane*, /arm-io/dart-ane* and
/arm-io/pmgr and a few /, /arm-io and /chosen identity properties; (2) reads
the ANE pmgr power-state words and, only when all of them read ACTUAL =
TARGET = 0xf, reads RVBAR, CPU_STATUS and the TCR, TTBR and ERROR registers of
the three ANE DARTs (an engine read with the islands off hangs the SoC);
(3) reads the firmware that iBoot preloaded from DRAM at the ADT
segment-ranges addresses, as the lab's T6021 capture did. It compares the
preload with the archive image and writes t8112-result.json, the two raw
segments, adt.txt and SHA256SUMS to --out. When --archive does not exist, it
is fetched there first with packaging/omarchy-ane-firmware-fetch.

--replay runs step 3's comparison on a lab capture directory (manifest.json,
text-segment.bin, data-segment.bin), with no device.
"""
import argparse
from collections import namedtuple
import hashlib
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import json
from pathlib import Path
import struct
import sys

SCHEMA = "omarchy-ane.t8112-kit.v1"
REPO = Path(__file__).resolve().parents[2]

# segs: (vmaddr, vmsize, fileoff, filesize) of __TEXT and __DATA. patch: vm of
# the five __rtk_patch records in RECORDS order. tunables: vm of
# __rtk_platform_asc_tunables_block. pmu_site: vm of the mov/movk/movk at
# CPowerControlServiceAneH14::SetPMUBaseAddress+0x6c that builds the pmgr
# address the firmware stores. T6021: ane/t6021/ane_fw_validate.h; T8112:
# receipts/2026-10-01-t8112-ane; pmu_site: the LC_SYMTAB of each image.
Image = namedtuple("Image", "fetch_key engine segs patch tunables pmu_site")
IMAGES = {
    0x6021: Image("apple,t6021", 0x284000000,
                  ((0, 0xC4000, 0x4000, 0xC4000), (0xC4000, 0x438000, 0xC8000, 0x3E8000)),
                  (0xCA848, 0xCA9B3, 0xCA9BF, 0xCA9CB, 0xCA9DB), 0xDCD78, 0x62700),
    0x8112: Image("apple,t8112", 0x26A000000,
                  ((0, 0xB4000, 0x4000, 0xB4000), (0xB4000, 0x438000, 0xB8000, 0x3E8000)),
                  (0xBA688, 0xBA7F3, 0xBA7FF, 0xBA80B, 0xBA81B), 0xCCBB8, 0x621E8),
}
RECORDS = (("stack_guard", b"GKTS", 8), ("soc", b"_COS", 4), ("soc_revision", b"RCOS", 4),
           ("cpu_pa", b"dApC", 8), ("wrapper_pa", b"dArW", 8))
DATA_BASE_VM = 0x423C  # TEXT u64 that iBoot sets to the IOVA of the DATA vmaddr
TUNABLES_LEN = 0x1E8

# T8112 live addresses: receipts/2026-10-01-t8112-ane "Derived values".
DRAM = 0x800000000
ENGINE = IMAGES[0x8112].engine
RVBAR = ENGINE + 0x1050000
CPU_STATUS = ENGINE + 0x1400048
DARTS = (0x26B800000, 0x26B810000, 0x26B820000)  # LLT, BRD, BWR
DART_REGS = (("error", 0x100), ("tcr0", 0x1000), ("ttbr0", 0x1400))  # apple-dart.c, t8110
PS = (("ANE_SYS", 0x23B7004A8), ("ANE_MPM", 0x23B70C000), ("ANE_SYS_CPU", 0x23B70C008),
      ("ANE_TD", 0x23B70C010), ("ANE_BASE", 0x23B70C018), ("ANE_SET1", 0x23B70C020),
      ("ANE_SET2", 0x23B70C028), ("ANE_SET3", 0x23B70C030), ("ANE_SET4", 0x23B70C038))
GATE = ("ANE_SYS", "ANE_SYS_CPU", "ANE_TD", "ANE_BASE", "ANE_SET1", "ANE_SET2", "ANE_SET3", "ANE_SET4")
IDENTITY = (("/", ("compatible", "target-type")),
            ("/arm-io", ("compatible", "chip-revision", "fuse-revision")),
            ("/chosen", ("chip-id", "board-id", "firmware-version", "system-firmware-version")))


class Refuse(Exception):
    pass


def fetch_tool():
    sys.dont_write_bytecode = True  # keep packaging/ free of __pycache__
    path = REPO / "packaging/omarchy-ane-firmware-fetch"
    spec = spec_from_loader("fetch", SourceFileLoader("fetch", str(path)))
    mod = module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_archive(soc, path):
    """The pinned 13.5 image for soc; fetched from Apple's CDN into path when path is absent."""
    fetch = fetch_tool()
    member, _, size, sha256 = fetch.FETCH[IMAGES[soc].fetch_key][:4]
    path = Path(path)
    try:
        if not path.exists():
            data = fetch.im4p_payload(fetch.fetch_member(fetch.IPSW["13.5"], member))
            fetch.verify(data, size, sha256)
            path.write_bytes(data)
        data = path.read_bytes()
        fetch.verify(data, size, sha256)
    except (fetch.Refuse, OSError, KeyError, fetch.http.client.HTTPException, fetch.zipfile.BadZipFile) as e:
        raise Refuse(f"archive {path}: {e}")
    return data


def movz_movk(code):
    """Value of mov xN,#a; movk xN,#b,lsl #16; movk xN,#c,lsl #32, or None."""
    value, rd = 0, None
    for i, w in enumerate(struct.unpack("<3I", code)):
        if w & 0xFF800000 != (0xD2800000 if i == 0 else 0xF2800000) or rd not in (None, w & 0x1F):
            return None
        rd = w & 0x1F
        value |= ((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 3))
    return value


def decode(soc, archive, text, data):
    """Compare a preload (TEXT and DATA at their vm sizes) with the archive.

    Every differing byte must sit in one iBoot field: the DATA base, a
    __rtk_patch value or the tunables block. Ranges outside them have
    field None. Raises ValueError when the preload is not this image's layout."""
    img = IMAGES[soc]
    (_, tvs, tfo, tfs), (dvm, dvs, dfo, dfs) = img.segs
    if (len(text), len(data)) != (tvs, dvs):
        raise ValueError(f"segment sizes {len(text):#x}/{len(data):#x} != the 13.5 image's {tvs:#x}/{dvs:#x}")
    ref = archive[tfo:tfo + tfs] + bytes(tvs - tfs) + archive[dfo:dfo + dfs] + bytes(dvs - dfs)
    live = text + data  # vm-indexed: TEXT vmsize == DATA vmaddr in both images
    spans = {"data_base": (DATA_BASE_VM, 8), "tunables": (img.tunables, TUNABLES_LEN)}
    for (name, tag, n), vm in zip(RECORDS, img.patch):
        if live[vm:vm + 8] != tag + struct.pack("<I", n) or ref[vm:vm + 8] != live[vm:vm + 8]:
            raise ValueError(f"__rtk_patch record {tag.decode()} at vm {vm:#x} != the 13.5 image")
        spans[name] = (vm + 8, n)

    diff = []
    for c in range(0, len(live), 0x4000):
        if live[c:c + 0x4000] != ref[c:c + 0x4000]:
            diff += [k for k in range(c, min(c + 0x4000, len(live))) if live[k] != ref[k]]
    runs = []
    for k in diff:
        if runs and runs[-1][1] == k:
            runs[-1][1] += 1
        else:
            runs.append([k, k + 1])

    def field(a, b):
        return next((name for name, (s, n) in spans.items() if s <= a and b <= s + n), None)

    def le(name):
        s, n = spans[name]
        return int.from_bytes(live[s:s + n], "little")

    tun = live[img.tunables:img.tunables + TUNABLES_LEN]
    count = tun[3]
    if 8 + 20 * count > TUNABLES_LEN:
        raise ValueError(f"tunables header {tun[:8].hex()} names {count} records; the block holds 24")
    fields = {name: hex(le(name)) for name in spans if name != "tunables"}
    pmu = movz_movk(live[img.pmu_site:img.pmu_site + 12])
    fields.update(
        entry_iova=hex(le("data_base") - dvm),
        pmu_base=None if pmu is None else hex(pmu),
        tunables_hex=tun.hex(),
        tunables=[[hex(o), hex(m), hex(v)] for o, m, v in struct.iter_unpack("<IQQ", tun[8:8 + 20 * count])])
    return dict(different_bytes=len(diff), fields=fields, ranges=[
        dict(vm=hex(a), length=b - a, field=field(a, b), archive=ref[a:b].hex(), live=live[a:b].hex())
        for a, b in runs])


def write_result(out, result, blobs):
    """Write blobs and t8112-result.json into the new directory out, then SHA256SUMS."""
    out.mkdir(parents=True, exist_ok=False)
    for name, data in blobs.items():
        (out / name).write_bytes(data)
    (out / "t8112-result.json").write_text(json.dumps(result, indent=1) + "\n")
    names = sorted(p.name for p in out.iterdir())
    (out / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n" for n in names))


def segment_record(name, phys, data, file):
    return dict(name=name, phys=hex(phys), size=hex(len(data)), file=file,
                sha256=hashlib.sha256(data).hexdigest())


def finish(result, soc, archive, text, data):
    try:
        result["decode"] = decode(soc, archive, text, data)
    except ValueError as e:
        result["decode"] = dict(error=str(e))
        return 1
    return 0 if all(r["field"] for r in result["decode"]["ranges"]) else 1


def replay(args, archive):
    src = Path(args.replay)
    manifest = {r["name"]: r for r in json.loads((src / "manifest.json").read_text())}
    segs = {}
    for name in ("text-segment", "data-segment"):
        path = (src / f"{name}.bin").resolve()
        segs[name] = path.read_bytes()
        if hashlib.sha256(segs[name]).hexdigest() != manifest[name]["sha256"]:
            raise Refuse(f"{path}: sha256 != its manifest.json")
    result = dict(schema=SCHEMA, mode="replay", soc=hex(args.soc), source=str(src.resolve()),
                  archive=hashlib.sha256(archive).hexdigest(),
                  segments=[segment_record(n, manifest[n]["address"], segs[n], str(src.resolve() / f"{n}.bin"))
                            for n in segs])
    rc = finish(result, args.soc, archive, segs["text-segment"], segs["data-segment"])
    write_result(Path(args.out), result, {})
    return rc


def connect():
    from m1n1.proxy import M1N1Proxy, UartInterface
    from m1n1.proxyutils import ProxyUtils, bootstrap_port

    # m1n1.setup is not used: it also writes the PMU panic counter.
    iface = UartInterface()
    p = M1N1Proxy(iface, debug=False)
    bootstrap_port(iface, p)
    return iface, p, ProxyUtils(p)


def collect(iface, p, u, archive, out):
    """One live session. Every device access is a read: p.read32/read64 and iface.readmem."""
    from m1n1.adt import build_prop

    chip = p.get_chipid()
    if chip != 0x8112:
        raise Refuse(f"chip id {chip:#x}: this kit is for T8112 (0x8112)")
    adt = u.adt

    def raw(node, keys=None):
        return {k: build_prop(node._path, k, v, node._types.get(k, (None, False))[0]).hex()
                for k, v in node._properties.items() if keys is None or k in keys}

    tops = [n for n in adt["/arm-io"] if n.name.startswith(("ane", "dart-ane")) or n.name == "pmgr"]
    dump = {path: raw(adt if path == "/" else adt[path], keys) for path, keys in IDENTITY}
    dump.update({n._path.removeprefix("/device-tree"): raw(n) for top in tops for n in top.walk_tree()})
    text = "\n".join(f"{path} {dump[path]}" for path, _ in IDENTITY) + "\n" + "\n".join(map(str, tops)) + "\n"

    seg = bytes.fromhex(dump.get("/arm-io/ane", {}).get("segment-ranges", ""))
    ranges = [dict(phys=hex(a), iova=hex(b), remap=hex(c), size=hex(d), flags=hex(e))
              for a, b, c, d, e in struct.iter_unpack("<QQQII", seg[:len(seg) // 32 * 32])]

    power = {name: p.read32(addr) for name, addr in PS}
    powered = all(power[n] & 0xFF == 0xFF for n in GATE)  # ACTUAL = TARGET = 0xf
    print("PS", " ".join(f"{n}={w:#x}" for n, w in power.items()), "powered" if powered else "OFF", flush=True)
    engine = darts = None
    if powered:
        engine = dict(rvbar=hex(p.read64(RVBAR)), cpu_status=hex(p.read32(CPU_STATUS)))
        darts = {hex(d): {k: hex(p.read32(d + off)) for k, off in DART_REGS} for d in DARTS}
    else:
        print("ANE power islands are not all on: no engine or DART read (it would hang the SoC)", flush=True)

    result = dict(schema=SCHEMA, mode="live", soc=hex(chip), archive=hashlib.sha256(archive).hexdigest(),
                  adt=dump, segment_ranges=ranges, power={n: hex(w) for n, w in power.items()},
                  powered=powered, engine=engine, darts=darts, segments=[])
    blobs = {"adt.txt": text.encode()}
    dram_end = DRAM + u.ba.mem_size_actual
    spans = [(int(r["phys"], 16), int(r["size"], 16)) for r in ranges]
    if len(spans) != 2 or not all(DRAM <= a < a + n <= dram_end and n <= 0x1000000 for a, n in spans):
        result["decode"] = dict(error=f"/arm-io/ane segment-ranges {ranges} are not two ranges inside DRAM "
                                      f"{DRAM:#x}..{dram_end:#x} (is the firmware preloaded? is the stub "
                                      "macOS 13.5?); no DRAM read")
        write_result(out, result, blobs)
        return 1
    segs = []
    for name, (phys, size) in zip(("text-segment", "data-segment"), spans):
        print(f"PRELOAD-READ {name} {phys:#x}+{size:#x}", flush=True)
        segs.append(iface.readmem(phys, size))
        if len(segs[-1]) != size:
            raise Refuse(f"short read: {name} {len(segs[-1]):#x} != {size:#x}")
        blobs[f"{name}.bin"] = segs[-1]
        result["segments"].append(segment_record(name, phys, segs[-1], f"{name}.bin"))
    rc = finish(result, 0x8112, archive, *segs)
    write_result(out, result, blobs)
    return rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--archive", required=True, help="the pinned 13.5 image; fetched here when absent")
    ap.add_argument("--out", required=True, help="new result directory")
    ap.add_argument("--replay", help="lab capture directory (manifest.json + segment .bin files)")
    ap.add_argument("--soc", type=lambda s: int(s, 0), default=0x8112, choices=sorted(IMAGES))
    args = ap.parse_args(argv)
    try:
        if Path(args.out).exists():
            raise Refuse(f"{args.out} exists; give a new directory")
        archive = load_archive(args.soc, args.archive)
        if args.replay:
            rc = replay(args, archive)
        else:
            if args.soc != 0x8112:
                raise Refuse("a live run is T8112 only")
            rc = collect(*connect(), archive, Path(args.out))
    except Refuse as e:
        print(f"collect-m1n1: {e}", file=sys.stderr)
        return 1
    d = json.loads((Path(args.out) / "t8112-result.json").read_text())["decode"]
    if "error" in d:
        print(f"collect-m1n1: no decode: {d['error']}", file=sys.stderr)
    else:
        f = d["fields"]
        print(f"{args.out}: {d['different_bytes']} bytes differ from the archive, "
              f"{sum(not r['field'] for r in d['ranges'])} ranges outside the iBoot fields")
        print(f"RTK_soc {f['soc']} rev {f['soc_revision']} cpu {f['cpu_pa']} wrapper {f['wrapper_pa']} "
              f"DATA base {f['data_base']} entry {f['entry_iova']} tunables {len(f['tunables'])} "
              f"PMU base {f['pmu_base']}")
    print(f"collect-m1n1: exit {rc}.")
    if not args.replay:
        print(f"Send {args.out} to the lab privately: it holds Apple firmware bytes.")
    return rc


if __name__ == "__main__":
    sys.exit(main())
