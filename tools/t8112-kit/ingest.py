#!/usr/bin/env python3
"""ingest.py: check a T8112 kit result and print the driver and overlay values it gives.

  ingest.py RESULT_DIR --archive h14_ane_fw_bia_j4xx.macho [--send-empty-irq N]
  ingest.py --macos t8112-macos-*.tar.gz

RESULT_DIR is the --out directory of collect-m1n1.py (live or replay). Every
field is computed again from the raw segments and the archive; a result whose
JSON, files or cross-checks disagree is refused (exit 1). For a T8112 result
the output is the derived values, the per-SoC driver data, and unified diffs
for ane/t6021/ane_fw_validate.h and (with --send-empty-irq, a lab choice)
packaging/dt/t8112-ane.dts; apply them with git apply from the repository
root. For a T6021 replay the values are checked against the driver's T6021
constants instead. --macos reads a collect-macos.sh tarball.
"""
import argparse
import difflib
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
OVERLAY = "packaging/dt/t8112-ane.dts"
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


def rows(tun):
    """C rows in the ane_fw_validate.h layout: the 8-byte header, then 20-byte records."""
    out = [tun[:8]] + [tun[i:i + 20] for i in range(8, len(tun), 20)]
    while len(out) > 1 and not any(out[-1]):
        out.pop()
    return ["\t" + ", ".join(f"0x{b:02x}" for b in row) + ",\n" for row in out]


def header_rows(text, name):
    body = text.split(f"static const u8 {name}[ANE_FW_TUNABLES_LEN] = {{\n", 1)[1].split("};\n", 1)[0]
    return [line for line in body.splitlines(keepends=True) if "/*" not in line]


def define(text, name):
    return int(re.search(rf"#define {name}\s+(0x[0-9a-f]+)", text).group(1), 16)


def check_t6021(f):
    """A T6021 replay must reproduce the constants the driver already carries."""
    hdr = (REPO / HEADER).read_text()
    fwload = (REPO / "ane/t6021/ane_t6021_fwload.c").read_text()
    alias = re.search(r"iommu-addresses = <&ane (0x[0-9a-f]+) (0x[0-9a-f]+)",
                      (REPO / "packaging/dt/t602x-ane.dtsi").read_text())
    tun = bytes.fromhex(f["tunables_hex"])
    for what, got, want in (
            ("RTK_soc_revision vs ANE_T602X_SOC_REVISION", int(f["soc_revision"], 16),
             define(hdr, "ANE_T602X_SOC_REVISION")),
            ("tunables vs ane_t602x_asc_tunables, as C rows", rows(tun), header_rows(hdr, "ane_t602x_asc_tunables")),
            ("PMU page vs ANE_T6021_PMU_PA", int(f["pmu_base"], 16) & ~(PAGE - 1), define(fwload, "ANE_T6021_PMU_PA")),
            ("entry IOVA vs the t602x-ane.dtsi alias reservation", int(f["entry_iova"], 16),
             int(alias.group(1), 16) << 32 | int(alias.group(2), 16))):
        check(got == want, f"{what}: {got} != {want}")
        print(f"  matches the driver: {what}")


def unified(path, old, new):
    return "".join(difflib.unified_diff(old, new, f"a/{path}", f"b/{path}"))


def header_diff(f, tag):
    old = (REPO / HEADER).read_text().splitlines(keepends=True)
    start = old.index("static const u8 ane_t602x_asc_tunables[ANE_FW_TUNABLES_LEN] = {\n")
    end = old.index("};\n", start) + 1
    tun = bytes.fromhex(f["tunables_hex"])
    block = ["\n",
             "/* T8112 values that iBoot wrote into the h14_ane_fw_bia_j4xx preload of\n",
             f" * a T8112 boot (tools/t8112-kit result {tag}). */\n",
             f"#define ANE_T8112_SOC_REVISION\t{f['soc_revision']}\n",
             "\n",
             "static const u8 ane_t8112_asc_tunables[ANE_FW_TUNABLES_LEN] = {\n",
             f"\t/* header, then {tun[3]} entries {{u32 offset, u64 mask, u64 value}} */\n",
             *rows(tun),
             "};\n"]
    return unified(HEADER, old, old[:end] + block + old[end:])


OVERLAY_NOTE = (" * Not here, because Apple data does not complete them: the ANE mailbox\n"
                " * (0x26b408000; stock apple-mailbox needs a send-empty IRQ, and no ADT node\n"
                " * names one) and the firmware IOVA reservation (memory-region; the entry\n"
                " * IOVA is in the ane segment-ranges, which iBoot writes at boot, and no IPSW\n"
                " * ADT has it). ane_t6021 needs both.\n")
OVERLAY_ANE = "\t\t\tane@26a000000 {\n"
OVERLAY_PD = "\t\t\t\t\t\t<&ps_ane_set4>;\n"


def overlay_diff(entry, irq, tag):
    old = (REPO / OVERLAY).read_text()
    for anchor in (OVERLAY_NOTE, OVERLAY_ANE, OVERLAY_PD):
        check(old.count(anchor) == 1, f"{OVERLAY} changed since ingest.py was written; update its anchors")
    note = (" *  - mailbox@26b408000: the ASC mailbox v4 at ASC + 0x8000, as on T6021.\n"
            " *    recv-not-empty is the ADT ane interrupt 520 (on T6021 the ADT ane\n"
            f" *    interrupt 884 is that line). send-empty {irq} is a lab choice: no ADT\n"
            " *    node names one (T6021 uses the unused AIC2 line 1833).\n"
            " * No memory-region: ane_t6021 runs the firmware from its own memory and\n"
            f" * aliases it at the entry IOVA {entry:#x} (T8112 capture, tools/t8112-kit\n"
            f" * result {tag}).\n")
    mbox = ("\t\t\tane_mbox: mailbox@26b408000 {\n"
            "\t\t\t\tcompatible = \"apple,t8112-ane-mailbox\", \"apple,asc-mailbox-v4\";\n"
            "\t\t\t\treg = <0x2 0x6b408000 0x0 0x4000>;\n"
            "\t\t\t\tinterrupt-parent = <&aic>;\n"
            "\t\t\t\tinterrupt-names = \"recv-not-empty\", \"send-empty\";\n"
            f"\t\t\t\tinterrupts = <0 520 4>, <0 {irq} 4>;\n"
            "\t\t\t\t#mbox-cells = <0>;\n"
            "\t\t\t\tstatus = \"okay\";\n"
            "\t\t\t};\n\n")
    new = (old.replace(OVERLAY_NOTE, note).replace(OVERLAY_ANE, mbox + OVERLAY_ANE)
           .replace(OVERLAY_PD, OVERLAY_PD + "\t\t\t\tmboxes = <&ane_mbox>;\n"))
    return unified(OVERLAY, old.splitlines(keepends=True), new.splitlines(keepends=True))


def report_t8112(f, sources, tag, irq):
    img = collect.IMAGES[0x8112]
    member, name, size, digest = collect.fetch_tool().FETCH[img.fetch_key]
    pmu = int(f["pmu_base"], 16)
    segs = ", ".join("{" + ", ".join(f"{x:#x}" for x in s) + "}" for s in img.segs)
    patch = " ".join(f"{t.decode()} {vm:#x}" for (_, t, _), vm in zip(collect.RECORDS, img.patch))
    print(f"""
apple,t8112-ane per-SoC data for ane_t6021 (one reviewed commit; the driver has no T8112 entry yet):
  firmware           {name}  {size} B  sha256 {digest}
  segments           {segs}
  patch records      {patch}; tunables {img.tunables:#x}; DATA base u64 {collect.DATA_BASE_VM:#x}
  RTK_soc            {f['soc']}
  RTK_soc_revision   {f['soc_revision']}
  ASC tunables       {len(f['tunables'])} records (diff below)
  cpu / wrapper PA   {f['cpu_pa']} / {f['wrapper_pa']}  (engine + 0x1000000 / + 0x1400000)
  entry IOVA         {f['entry_iova']}  ({'; '.join(sources)})
  pmgr page          {pmu & ~(PAGE - 1):#x}  (firmware PMU base {pmu:#x}, SetPMUBaseAddress+0x6c at vm {img.pmu_site:#x};
                     the dart-ane DAPF names 0x23b70c000..0x23b70c03b)
  PWGATE             reg[2] + 0x8b8: write 0, wait for bits 29:28 = 0; off: 0x30000000 (receipts/2026-10-01-t8112-ane)
  overlay            stays disabled in packaging/dt/overlays until the driver binds apple,t8112-ane
""")
    print(header_diff(f, tag), end="")
    if irq is None:
        print(f"\n(no {OVERLAY} diff: give --send-empty-irq N, an unused AIC line the lab picks)")
    else:
        print(overlay_diff(int(f["entry_iova"], 16), irq, tag), end="")


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
    ap.add_argument("--send-empty-irq", type=int, help="AIC line for the mailbox send-empty interrupt")
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
        if soc == 0x6021:
            check_t6021(f)
        else:
            report_t8112(f, sources, tag, args.send_empty_irq)
    except (Bad, collect.Refuse, OSError, KeyError, ValueError) as e:
        print(f"ingest: REFUSED: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
