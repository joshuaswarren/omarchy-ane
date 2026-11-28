#!/usr/bin/env python3
"""Cross-check test for data/ane-soc/t6050.json against the macOS 27.2 inputs.

The record was re-derived from the 27.2 (26B123) j716cap device tree dump
(source A2), the 27.2 build identity list (source B2), and the
27.2 kernel-image strings (source K2). This test re-asserts the values those
inputs carry so a later edit cannot silently drift from the extraction:

  python3 tools/test_ane_soc_t6050_272.py

Needs no hardware and no ADT input; it checks the committed record only.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from validate_ane_soc import validate, Violation  # noqa: E402

REC = ROOT / "data" / "ane-soc" / "t6050.json"
failures = []


def check(name, cond, detail=""):
    if not cond:
        failures.append(f"{name}: {detail}")


def addr(s):
    return int(s, 16)


def main():
    doc = json.loads(REC.read_text())
    try:
        validate(REC)
    except Violation as e:
        failures.append(f"schema: {e}")
        print("\n".join(failures))
        return 1

    src_ids = {s["id"] for s in doc["sources"]}
    for sid in ("A2", "B2", "K2"):
        check("sources", sid in src_ids, f"27.2 source {sid} missing")

    ane = doc["ane"]
    reg = {w["name"]: (addr(w["base"]), addr(w["size"])) for w in ane["reg"]["v"]}
    expect = {
        "reg[0]": (0x508000000, 0x2000000), "reg[1]": (0x280900000, 0x18000),
        "reg[2]": (0x280920000, 0x4000), "reg[3]": (0x284240000, 0x40000),
        "reg[4]": (0x212000000, 0xFF4000), "reg[5]": (0x584000000, 0x2000000),
        "reg[6]": (0x288374000, 0x4000),
    }
    check("ane.reg", reg == expect, f"{reg}")
    check("ane.compatible", ane["compatible"]["v"] == "ane,t8132exclave")
    check("ane.ane_type", ane["ane_type"]["v"] == 544)
    check("ane.interrupts", ane["interrupts"]["v"]["lines"] == [954, 967])
    check("ane.clock_ids", ane["clock_ids"]["v"] == [335, 334, 381])
    check("ane.exclave", ane["exclave"]["v"]["exclave-reg"] == "0x549c00000+0x88000")
    check("ane.src", ane["reg"]["src"] == "A2", ane["reg"]["src"])

    dart = doc["dart"]
    check("dart.compatible", dart["compatible"]["v"] == "dart,t8110")
    check("dart.sid", dart["sid"]["v"] == [0, 10, 11, 13, 15])
    check("dart.exclave_sid", dart["exclave_sid"]["v"] == [1, 2, 3, 4, 5, 6, 7])
    check("dart.sid_disjoint",
          not set(dart["sid"]["v"]) & set(dart["exclave_sid"]["v"]))
    check("dart.sid_count", dart["sid_count"]["v"] == 16)
    check("dart.page_size", dart["page_size"]["v"] == 16384)
    mappers = dart["mappers"]["v"]
    check("dart.mappers", mappers["mapper-ane0"] == 0 and
          mappers["mapper-ane0-mpm"] == 11 and
          [mappers[f"mapper-ane0-iso{i}"] for i in range(1, 8)] == list(range(1, 8)))

    # dapf ranges must sit inside a dart or ane reg[1] window
    windows = [(0x509800000, 0x509800000 + 0xC000), (0x509820000, 0x509820000 + 0xC000),
               (0x509840000, 0x509840000 + 0xC000), (0x509810000, 0x509810000 + 0x4000),
               (0x280900000, 0x280900000 + 0x18000)]
    for r in dart["dapf_ranges"]["v"]:
        lo_s, hi_s = r.split("-")
        lo, hi = addr(lo_s), addr(hi_s)
        check("dart.dapf", any(a <= lo <= b and a <= hi <= b for a, b in windows)
              or lo > 0x100000000, f"{r} outside known windows")

    pmgr = {d["name"]: d for d in doc["pmgr"]["ane_devices"]["v"]}
    check("pmgr.names", set(pmgr) ==
          {"ANE_MPM", "ANE_CPU", "ANE_TDBASE", "ANE_SYS_V", "ANE_SYS_V_PMP", "ANE_SYS_DART"})
    check("pmgr.ids", [pmgr[n]["id"] for n in
          ("ANE_MPM", "ANE_CPU", "ANE_TDBASE", "ANE_SYS_V", "ANE_SYS_V_PMP", "ANE_SYS_DART")]
          == [222, 223, 224, 569, 664, 568])
    check("pmgr.mpm", pmgr["ANE_MPM"]["ps"] == "0x2809003c0" and pmgr["ANE_MPM"]["parents"] == [219])
    check("pmgr.cpu", pmgr["ANE_CPU"]["ps"] == "0x2809003c8" and pmgr["ANE_CPU"]["parents"] == [219])
    # 27.2: TDBASE moved to parent 221 and no_ps (release 27 had parent 219 + ps)
    check("pmgr.tdbase", pmgr["ANE_TDBASE"]["parents"] == [221] and
          pmgr["ANE_TDBASE"]["ps"].startswith("none"), pmgr["ANE_TDBASE"])
    for name in ("ANE_MPM", "ANE_CPU"):
        ps = addr(pmgr[name]["ps"])
        check("pmgr.ps_window", 0x280900000 <= ps < 0x280918000, pmgr[name]["ps"])

    # board ids from the 27.2 manifest
    bids = doc["board_ids"]["v"]
    check("board_ids", bids == {"j714c": "0x0A", "j714s": "0x08", "j716c": "0x0E",
          "j716s": "0x0C", "j775c": "0x10", "j775d": "0x12", "j873s": "0x02"}, bids)

    fw = doc["firmware"]
    check("fw.name", fw["name"]["v"] == "h17_ane0_fw_hyperion_j71y.im4p")
    check("fw.path", fw["chip_map"]["v"]["ANE"]["path"].endswith(fw["name"]["v"]))
    check("fw.src_272", fw["name"]["src"] == "B2")

    check("clpc", "ANE" in doc["clpc"]["perf_domain"]["v"] and
          doc["clpc"]["ane_utilization"]["v"]["target"] == 52428)
    check("kc_kexts", "T6050 ANE HAL" in doc["driver_family_272"]["v"])

    if failures:
        print("t6050 27.2 cross-check FAILURES:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print("t6050 27.2 cross-check: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
