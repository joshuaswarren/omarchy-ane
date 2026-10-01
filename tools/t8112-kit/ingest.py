#!/usr/bin/env python3
"""ingest.py: check a T8112 or T6021 kit result against the driver's per-SoC data.

  ingest.py RESULT_DIR --archive IMAGE.macho
  ingest.py --macos t8112-macos-*.tar.gz

RESULT_DIR is the --out directory of collect-m1n1.py (live or replay). Every
field is computed again from the raw segments and the archive; a result whose
JSON, files or cross-checks disagree is refused (exit 1). Then the values
iBoot wrote are compared with ane_t6021's data for that SoC
(ane/t6021/ane_t6021_fwload.c, ane/t6021/ane_fw_validate.h): the ASC
tunables block, the pmgr page, RTK_soc_revision (T8112: the driver reads it
from the eFuse window, so the result only shows it), and on T6021 the
t602x-ane.dtsi alias IOVA. A difference is refused: the capture wins and the
driver data needs a fix. --macos reads a collect-macos.sh tarball.
"""
import argparse
import hashlib
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
import json
from pathlib import Path
import plistlib
import re
import struct
import sys
import tarfile

sys.dont_write_bytecode = True
KIT = Path(__file__).resolve().parent
REPO = KIT.parents[1]
_spec = spec_from_loader("collect", SourceFileLoader("collect", str(KIT / "collect-m1n1.py")))
collect = module_from_spec(_spec)
_spec.loader.exec_module(collect)

HEADER = "ane/t6021/ane_fw_validate.h"
RVBAR_ADDR_MASK = 0xFF7EFFFFFFFFF800  # ane/t6021/ane_t6021_boot.h ANE_T6021_RVBAR_ADDR_MASK
PAGE = 0x4000


class Bad(Exception):
    pass


def check(ok, msg):
    if not ok:
        raise Bad(msg)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load(rdir):
    """The result JSON and its two segments, after the file checks."""
    result = json.loads((rdir / "t8112-result.json").read_text())
    check(result.get("schema") == collect.SCHEMA, f"schema {result.get('schema')!r} != {collect.SCHEMA!r}")
    soc = int(result["soc"], 16)
    check(soc in collect.IMAGES, f"soc {soc:#x} has no image in collect-m1n1.py")
    sums = dict(reversed(line.split("  ", 1)) for line in (rdir / "SHA256SUMS").read_text().splitlines())
    check("t8112-result.json" in sums, "SHA256SUMS does not cover t8112-result.json")
    for name, digest in sums.items():
        check(sha((rdir / name).read_bytes()) == digest, f"SHA256SUMS: {name} changed")
    segs = {}
    for s in result["segments"]:
        data = (rdir / s["file"]).read_bytes()
        check(sha(data) == s["sha256"] and hex(len(data)) == s["size"], f"{s['file']}: size or sha256 != the JSON")
        segs[s["name"]] = data
    return result, soc, segs


def verify(result, soc, segs, archive):
    """Recompute the decode and cross-check the entry IOVA sources; return (fields, sources)."""
    img = collect.IMAGES[soc]
    d = result["decode"]
    check("error" not in d, f"collect-m1n1 reported: {d.get('error')}")
    check(result["archive"] == sha(archive), "the result was decoded against another archive")
    check(collect.decode(soc, archive, segs["text-segment"], segs["data-segment"]) == d,
          "the JSON decode != a fresh decode of its own segment files")
    outside = [r for r in d["ranges"] if not r["field"]]
    check(not outside, f"{len(outside)} differing ranges outside the iBoot fields: {outside[:3]}")
    f = d["fields"]
    check(int(f["soc"], 16) == soc, f"RTK_soc {f['soc']} != {soc:#x}")
    check(int(f["cpu_pa"], 16) == img.engine + 0x1000000 and int(f["wrapper_pa"], 16) == img.engine + 0x1400000,
          f"cpu/wrapper {f['cpu_pa']}/{f['wrapper_pa']} != engine {img.engine:#x} + 0x1000000/0x1400000")
    check(f["pmu_base"] is not None, f"no mov/movk/movk at the SetPMUBaseAddress site {img.pmu_site:#x}")
    if result.get("engine") or result.get("darts"):
        check(all(int(result["power"][n], 16) & 0xFF == 0xFF for n in collect.GATE),
              "engine or DART registers were read while a PS word was not on")

    entry = int(f["entry_iova"], 16)
    sources = {"DATA base - DATA vmaddr": entry}
    dvm, dvs = img.segs[1][:2]
    sr = result.get("segment_ranges") or []
    if sr:
        check(len(sr) == 2, f"{len(sr)} segment-ranges records")
        text, data = ({k: int(v, 16) for k, v in r.items()} for r in sr)
        check((text["iova"], text["size"], data["iova"], data["size"]) == (0, img.segs[0][1], dvm, dvs),
              f"segment-ranges iova/size {sr} != the 13.5 image layout")
        sources["segment-ranges TEXT remap"] = text["remap"]
        sources["segment-ranges DATA remap - DATA iova"] = data["remap"] - data["iova"]
    if result.get("engine"):
        rvbar = int(result["engine"]["rvbar"], 16)
        if rvbar & 1:
            sources["RVBAR (latched)"] = rvbar & RVBAR_ADDR_MASK
    check(len(set(sources.values())) == 1, "entry IOVA sources disagree: "
          + ", ".join(f"{k} {v:#x}" for k, v in sources.items()))
    return f, sources


SOC_DATA = {0x6021: "ane_t6021_soc", 0x8112: "ane_t8112_soc"}


def driver_data(soc):
    """(soc_revision or None for the fuse read, tunables keys, records, pmu_pa) of the driver."""
    fwload = (REPO / "ane/t6021/ane_t6021_fwload.c").read_text()
    hdr = (REPO / HEADER).read_text()
    body = fwload.split(f"const struct ane_t602x_soc {SOC_DATA[soc]} = {{", 1)[1].split("};", 1)[0]
    rev = re.search(r"\.soc_revision = (0x[0-9a-f]+)", body)
    table = re.search(r"\.tunables = &(\w+)", body).group(1)
    tun = hdr.split(f"static const struct ane_asc_tunables {table} = {{", 1)[1].split("\n};", 1)[0]
    keys = [int(k, 16) for k in re.findall(r"0x[0-9a-f]+", re.search(r"\.keys = \{([^}]*)\}", tun).group(1))]
    recs = [tuple(int(x, 16) for x in r)
            for r in re.findall(r"\{ (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+) \}", tun.split(".r = {", 1)[1])]
    pmu = int(re.search(r"\.pmu_pa = (0x[0-9a-f]+)", body).group(1), 16)
    return (None if ".revision_fuse = true" in body else int(rev.group(1), 16)), keys, recs, pmu


def driver_block(soc, rev):
    """The type-1 tunables block the driver writes for chip revision REV."""
    _, keys, recs, _ = driver_data(soc)
    key = next((k for k in keys if k <= rev), None)
    check(key is not None, f"the driver has no ASC tunables entry for revision {rev:#x}")
    return bytes([1, 3, 0x24, len(recs)]) + struct.pack("<I", key) + b"".join(
        struct.pack("<IQQ", *r) for r in recs)


def check_driver(f, soc):
    """A result must reproduce the per-SoC data the driver carries."""
    rev_driver, _, _, pmu = driver_data(soc)
    rev = int(f["soc_revision"], 16)
    tun = bytes.fromhex(f["tunables_hex"])
    want = driver_block(soc, rev)
    checks = [("ASC tunables block vs the driver's table",
               (tun[:len(want)], any(tun[len(want):])), (want, False)),
              ("pmgr page vs pmu_pa", int(f["pmu_base"], 16) & ~(PAGE - 1), pmu)]
    if rev_driver is None:
        print(f"  RTK_soc_revision {rev:#x}: the driver reads it from the eFuse window at load")
    else:
        checks.append(("RTK_soc_revision vs soc_revision", rev, rev_driver))
    if soc == 0x6021:
        alias = re.search(r"iommu-addresses = <&ane (0x[0-9a-f]+) (0x[0-9a-f]+)",
                          (REPO / "packaging/dt/t602x-ane.dtsi").read_text())
        checks.append(("entry IOVA vs the t602x-ane.dtsi alias reservation", int(f["entry_iova"], 16),
                       int(alias.group(1), 16) << 32 | int(alias.group(2), 16)))
    for what, got, wanted in checks:
        check(got == wanted, f"{what}: differs")
        print(f"  matches the driver: {what}")


def macos(tarball):
    with tarfile.open(tarball) as t:
        files = {m.name.split("/", 1)[-1]: t.extractfile(m).read() for m in t.getmembers() if m.isfile()}
    for line in files["SHA256SUMS"].decode().splitlines():
        digest, name = line.split(maxsplit=1)
        check(sha(files[name]) == digest, f"SHA256SUMS: {name} changed")
    for name in ("sw_vers.txt", "hw.model.txt", "hardware.txt", "chosen.txt", "arm-io.txt"):
        print(f"--- {name}\n{files[name].decode().strip()}")
    node = plistlib.loads(files["ioreg-ane.plist"])
    node = node[0] if isinstance(node, list) else node
    seg = node.get("segment-ranges", b"")
    check(len(seg) == 64, f"the ane node has no 2-record segment-ranges ({len(seg)} bytes)")
    (tp, ti, tr, ts, tf), (dp, di, dr, ds, df) = struct.iter_unpack("<QQQII", seg)
    print(f"--- segment-ranges\n  TEXT phys {tp:#x} iova {ti:#x} remap {tr:#x} size {ts:#x} flags {tf:#x}\n"
          f"  DATA phys {dp:#x} iova {di:#x} remap {dr:#x} size {ds:#x} flags {df:#x}")
    check(tr - ti == dr - di, "TEXT and DATA remaps give different entry IOVAs")
    print(f"entry IOVA {tr - ti:#x}: the IOVA of vm 0 for the image macOS booted. On T6021 the TEXT remap\n"
          "was 0x10000000000 under both macOS 27 (ioreg) and the 13.5 stub (m1n1 ADT), the latched RVBAR entry.")
    rev = re.search(rb'"chip-revision" = <([0-9a-f]{8})>', files["arm-io.txt"])
    if rev:
        print(f"/arm-io chip-revision {int.from_bytes(bytes.fromhex(rev.group(1).decode()), 'little'):#x}: "
              "a candidate for RTK_soc_revision only (on T6001 both are 0x11; no T6021 record links them).")
    print("Not from macOS: the ASC tunables (iBoot's own table) and a 13.5 preload; run collect-m1n1.py.")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("result", nargs="?", type=Path, help="collect-m1n1.py --out directory")
    ap.add_argument("--archive", help="the pinned 13.5 image of the result's SoC; fetched here when absent")
    ap.add_argument("--macos", type=Path, help="collect-macos.sh tarball")
    args = ap.parse_args(argv)
    try:
        if args.macos:
            macos(args.macos)
            return 0
        check(args.result and args.archive, "give RESULT_DIR and --archive (or --macos TARBALL)")
        result, soc, segs = load(args.result)
        f, sources = verify(result, soc, segs, collect.load_archive(soc, args.archive))
        tag = sha((args.result / "t8112-result.json").read_bytes())[:12]
        print(f"{args.result}: ok ({result['mode']}, soc {soc:#x}, result sha256 {tag}...)")
        for k in ("soc", "soc_revision", "cpu_pa", "wrapper_pa", "data_base", "entry_iova", "pmu_base",
                  "stack_guard"):
            print(f"  {k:13} {f[k]}")
        print(f"  tunables      {len(f['tunables'])} records, header {f['tunables_hex'][:16]}")
        for k, v in sources.items():
            print(f"  entry source  {k}: {v:#x}")
        if result.get("power"):
            print("  power        ", " ".join(f"{k}={v}" for k, v in result["power"].items()),
                  "| engine", result.get("engine"))
        check_driver(f, soc)
    except (Bad, collect.Refuse, OSError, KeyError, ValueError) as e:
        print(f"ingest: REFUSED: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
