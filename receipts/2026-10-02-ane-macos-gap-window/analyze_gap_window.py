#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Host-side analysis for the T6021 ANE gap window (runnable standalone on synthetic data).

Takes the macOS capture directory (the bundle's out/) plus the Linux E1 results JSON and prints:
  - the per-probe ratio table (Linux/macOS minmin corrected + raw, medmed; P6, P6', P7, encoder),
  - the E1 decision per the GapRank H1 rule on P6' (P6 when P6' is absent): ratio >= 2.0 supports
    the low-clock hypothesis, <= 1.15 while the activation-bound arms carry the ratio falsifies it,
  - fabric-ps / dcs-ps (DESIRED[3:0]) and the DSID word per ANERegDump dump (idle, mid-loop attempts),
    read by the per-range status in each index.json; perfStats per probe; golden compares.

Inputs:
  --macos-capture DIR   bundle out/ (timings/<probe>/blocks.tsv + block-*.json, 03-/04- regdump)
  --linux-e1 FILE       Linux E1 results JSON (raw ms; settle_ms is subtracted from the Linux min):
                        {"settle_ms": 1.05, "p6prime": {"blocks": [[min_ms, median_ms], ...]},
                         "p6": {...}, "p7": {...}, "pk": {...}}   (absent probe = "pending")
  --linux-ps FILE       optional Linux words JSON, printed as given:
                        {"idle": {"fabric_ps": "0x555", "dcs_ps": "0x999", "dsid": "0x80"}, ...}
  --e1 / --e1-prime DIR E1 probe trees for the P6/P7 and P6' golden compares
  --linux-out P=NPY     Linux output of probe P (e.g. p6prime=y63.npy) for a bitwise cross-OS compare
  --synthetic           generate a self-consistent fake capture + fake Linux result and analyze
                        that (no hardware needed; exercises every code path)
macOS numbers are upper bounds when captured under load; the report says so (the stamps carry load).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

PROBES = ("p6", "p6prime", "p7", "pk")
NAMES = {"p6": "P6 compute-bound (64x conv1x1, L2-resident weights; Linux output invalid, P6Fix)",
         "p6prime": "P6' = P6 program with input x0.8 (the E1 decision probe)",
         "p7": "P7 activation stream (2x32 MiB fp16 add)",
         "pk": "Parakeet whole encoder (reference: Linux 254.3 / macOS 89.3 ms)"}


def block_stats(path: Path):
    """min of block mins, median of block medians from a blocks.tsv written by the bundle."""
    mins, meds = [], []
    for line in path.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) >= 4 and parts[1] and parts[2]:
            mins.append(float(parts[1]))
            meds.append(float(parts[2]))
    if not mins:
        return None
    mins.sort()
    meds.sort()
    return {"minmin": mins[0], "medmed": meds[len(meds) // 2], "blocks": len(mins)}


def macos_stats(cap: Path):
    out = {}
    for probe in PROBES:
        tsv = cap / "timings" / probe / "blocks.tsv"
        out[probe] = block_stats(tsv) if tsv.exists() else None
    return out


def regdump_words(regdump_dir: Path):
    """fabric-ps / dcs-ps / dsid words of one aneregdump output dir, by the per-range status in
    index.json (a gated range is written as an empty .bin)."""
    idx = regdump_dir / "index.json"
    if not idx.exists():
        return None
    j = json.loads(idx.read_text())
    status = {r["name"]: r["status"] for r in j.get("ranges", [])}
    words = {"islands_up": j.get("islands_up"), "ps": j.get("ps")}
    for name in ("fabric-ps", "dcs-ps", "dsid"):
        f = regdump_dir / f"{name}.bin"
        if status.get(name) == "ok" and f.exists() and f.stat().st_size >= 4:
            words[name] = "0x%08x" % struct.unpack("<I", f.read_bytes()[:4])[0]
        else:
            words[name] = status.get(name, "absent")
    return words


def phase_dumps(cap: Path, tag: str):
    """[(label, words-or-reason)] for 03-regdump-idle (one dump) or 04-regdump-load (attempts a1..a8)."""
    base = cap / tag
    if (base / "SKIPPED").exists():
        return [("phase", {"skipped": (base / "SKIPPED").read_text().strip()})]
    dirs = [base] if (base / "regdump").is_dir() else sorted(base.glob("a[0-9]*"), key=lambda p: int(p.name[1:]))
    out = []
    for d in dirs:
        if (d / "SKIPPED").exists():
            out.append((d.name, {"skipped": (d / "SKIPPED").read_text().strip()}))
        else:
            out.append((d.name, regdump_words(d / "regdump") or {"skipped": "no index.json"}))
    return out


def dsid_decode(word: str):
    """SLC data-set id from the 0x285c2046c word; bits[17:10] per AneDsidRe2."""
    try:
        return (int(word, 16) >> 10) & 0xff
    except (ValueError, TypeError):
        return None


def desired(word: str):
    """DESIRED[3:0] of a ps word like 0x00000777 (format per the T6001 sysps reads)."""
    try:
        return int(word, 16) & 0xF
    except (ValueError, TypeError):
        return None


def fmt_side(stats, os_name):
    if stats is None:
        return f"{os_name}: pending"
    return (f"{os_name}: minmin {stats['minmin']:.3f} ms, medmed {stats['medmed']:.3f} ms "
            f"({stats['blocks']} blocks)")


def decision(r6, r7, rpk, name="P6"):
    if r6 is None:
        return f"{name} missing - no E1 decision possible"
    lines = []
    if r6 >= 2.0:
        lines.append(f"{name} ratio {r6:.2f} >= 2.0: SUPPORTS H1 (ANE operating point left low under "
                     "Linux; the compute-bound probe carries the ratio)")
    elif r6 <= 1.15:
        if (r7 is not None and r7 >= 2.0) or (rpk is not None and rpk >= 2.0):
            lines.append(f"{name} ratio {r6:.2f} <= 1.15 while activation-bound arms carry the ratio: "
                         "FALSIFIES H1 (clock is not the cause; memory-side H3/H12 class)")
        else:
            lines.append(f"{name} ratio {r6:.2f} <= 1.15 but nothing else carries the ratio: "
                         "H1 unsupported, cause likely driver/firmware-path shared cost")
    else:
        lines.append(f"{name} ratio {r6:.2f} in the unresolved band (1.15, 2.0): mixed; report both")
    if r7 is not None:
        if r7 > r6 * 1.5:
            lines.append(f"P7 {r7:.2f} >> {name} {r6:.2f}: the gap concentrates in the DMA/activation "
                         "path (H3 DCS/fabric QoS or H12 non-cacheable BO mappings)")
        elif r6 > r7 * 1.5:
            lines.append(f"{name} {r6:.2f} >> P7 {r7:.2f}: the gap concentrates in compute, consistent "
                         "with a low NE clock rather than memory side")
        else:
            lines.append(f"P7 {r7:.2f} ~ {name} {r6:.2f}: uniform ratio, one shared cause (clock-class)")
    if rpk is not None:
        lines.append(f"encoder ratio {rpk:.2f} vs the whole-encoder reference 2.83 "
                     "(NativeVsCross/NativeMacRun)")
    return "\n  ".join(lines)


def print_dumps(label, dumps):
    if not dumps:
        print(f"  macOS {label}: no capture")
    for name, w in dumps:
        if "skipped" in w:
            print(f"  macOS {label} {name}: SKIPPED ({w['skipped']})")
            continue
        fab, dcs, ds = w["fabric-ps"], w["dcs-ps"], w["dsid"]
        dsid = f"{ds} dsid={dsid_decode(ds)}" if ds.startswith("0x") else f"{ds} (not read)"
        print(f"  macOS {label} {name}: islands_up={w['islands_up']} fabric-ps {fab} "
              f"(DESIRED={desired(fab)}) dcs-ps {dcs} (DESIRED={desired(dcs)}) dsid-word {dsid}"
              f"\n    ps {w['ps']}")


def perfstats(cap: Path):
    """perf_stats_last of every block JSON: blocks that carried the dict, blocks where it had content."""
    for probe in PROBES:
        carried, filled, last = 0, 0, None
        for f in sorted((cap / "timings" / probe).glob("block-*.json")):
            try:
                ps = json.loads(f.read_text()).get("perf_stats_last")
            except json.JSONDecodeError:
                continue
            if ps is not None:
                carried += 1
                if ps.strip("{} \n"):
                    filled, last = filled + 1, ps
        print(f"  {probe}: perfStats dict accepted on {carried} block(s), non-empty on {filled}"
              + (f"; last: {' '.join(str(last).split())[:600]}" if last else ""))


def macos_load(cap: Path, probe: str):
    """1-min load average range over the block stamps of one probe."""
    tsv = cap / "timings" / probe / "blocks.tsv"
    loads = [float(p.split("load=")[1].split()[0]) for p in
             (tsv.read_text().splitlines() if tsv.exists() else []) if "load=" in p]
    return (min(loads), max(loads)) if loads else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--macos-capture", type=Path)
    ap.add_argument("--linux-e1", type=Path)
    ap.add_argument("--linux-ps", type=Path)
    ap.add_argument("--e1", type=Path, help="E1 probe tree for golden output compares")
    ap.add_argument("--e1-prime", type=Path, help="P6' probe tree (p6prime/) for its golden compare")
    ap.add_argument("--linux-out", action="append", default=[], metavar="PROBE=NPY",
                    help="Linux output of the same probe (fp16 .npy) for a bitwise cross-OS compare")
    ap.add_argument("--synthetic", action="store_true")
    a = ap.parse_args()

    if a.synthetic:
        cap = Path("/var/tmp/gapwin-synth-capture")
        if cap.exists():
            import shutil
            shutil.rmtree(cap)
        import numpy as np
        rng = np.random.default_rng(7)
        # macOS ms, Linux/macOS ratio - shaped like the real record (pk: 89.3 / 2.84).
        shape = {"p6": (2.4, 2.2), "p6prime": (2.4, 2.2), "p7": (7.0, 2.9), "pk": (89.3, 2.84)}
        (cap / "timings").mkdir(parents=True)
        for probe, (mac_base, ratio) in shape.items():
            d = cap / "timings" / probe
            d.mkdir(parents=True)
            with open(d / "blocks.tsv", "w") as f:
                for b in range(20):
                    mac = mac_base * float(rng.uniform(0.99, 1.01))
                    f.write(f"block-{b:02d}.json\t{mac:.3f}\t{mac * 1.01:.3f}\t20\t"
                            f"ts=synth load=2.1 1.9 1.8 uptime_s=900\n")
        for rd, iu, dsid in ((cap / "03-regdump-idle/regdump", 0, None),
                             (cap / "04-regdump-load/a1/regdump", 0, None),
                             (cap / "04-regdump-load/a2/regdump", 1, 0x2480)):
            rd.mkdir(parents=True)
            (rd / "fabric-ps.bin").write_bytes(struct.pack("<I", 0x555))
            (rd / "dcs-ps.bin").write_bytes(struct.pack("<I", 0x999))
            (rd / "dsid.bin").write_bytes(struct.pack("<I", dsid) if dsid else b"")
            ranges = [{"name": n, "status": "ok"} for n in ("fabric-ps", "dcs-ps")]
            ranges.append({"name": "dsid", "status": "ok" if dsid else "gated"})
            (rd / "index.json").write_text(json.dumps({"islands_up": iu, "ps": ["0x300"] * 8,
                                                       "ranges": ranges}))
        lin = {}
        for p in PROBES:
            mac_base, ratio = shape[p]
            lin[p] = {"minmin": mac_base * ratio, "medmed": mac_base * ratio * 1.01, "blocks": 3}
        settle = 0.0
        lps = {"idle": {"fabric_ps": "0x00000555", "dcs_ps": "0x00000999"},
               "load": {"fabric_ps": "0x00000555", "dcs_ps": "0x00000999"}}
    else:
        if not a.macos_capture or not a.linux_e1:
            ap.error("need --macos-capture and --linux-e1 (or --synthetic)")
        cap = a.macos_capture
        lin_raw = json.loads(a.linux_e1.read_text())
        settle = float(lin_raw.get("settle_ms", 0.0))
        lin = {p: ({"minmin": min(b[0] for b in lin_raw[p]["blocks"]),
                    "medmed": sorted(b[1] for b in lin_raw[p]["blocks"])[len(lin_raw[p]["blocks"]) // 2],
                    "blocks": len(lin_raw[p]["blocks"])} if lin_raw.get(p, {}).get("blocks") else None)
               for p in PROBES}
        lps = json.loads(a.linux_ps.read_text()) if a.linux_ps else None

    mac = macos_stats(cap)

    print(f"== E1 ratio table (Linux / macOS; corrected = Linux minus {settle} ms call settle) ==")
    ratios = {}
    for p in PROBES:
        line = "\n    ratio: pending"
        if mac[p] and lin[p]:
            raw = lin[p]["minmin"] / mac[p]["minmin"]
            ratios[p] = (lin[p]["minmin"] - settle) / mac[p]["minmin"]
            line = (f"\n    ratio minmin corrected {ratios[p]:.3f} raw {raw:.3f}; medmed raw "
                    f"{lin[p]['medmed'] / mac[p]['medmed']:.3f}")
        ld = macos_load(cap, p)
        print(f"  {NAMES[p]}\n    {fmt_side(lin[p], 'Linux')}\n    {fmt_side(mac[p], 'macOS')}"
              + (f" load1 {ld[0]:.2f}-{ld[1]:.2f}" if ld else "") + line)
    d6 = "p6prime" if "p6prime" in ratios else "p6"
    print(f"== E1 decision (GapRank H1 rule, on {d6}) ==")
    print("  " + decision(ratios.get(d6), ratios.get("p7"), ratios.get("pk"), "P6'" if d6 == "p6prime" else "P6"))
    print("== fabric-ps / dcs-ps (DESIRED[3:0]) and DSID word 0x285c2046c (bits[17:10]) per OS ==")
    print_dumps("idle", phase_dumps(cap, "03-regdump-idle"))
    print_dumps("load", phase_dumps(cap, "04-regdump-load"))
    for tag, src in ((lps or {}).items()):
        print(f"  Linux {tag}: " + " ".join(f"{k} {v}" + (f" (DESIRED={desired(v)})" if k != "dsid" else
                                                         f" dsid={dsid_decode(v)}") for k, v in src.items()))
    if not lps:
        print("  Linux: n/a (no --linux-ps)")
    print("== perfStats (perf_stats_last per block) ==")
    perfstats(cap)
    print("== caveats ==")
    print("  macOS numbers carry the load stamps next to each block; load lengthens times, so a")
    print("  macOS min is an upper bound and every ratio a lower bound (NativeMacRun precedent).")
    if a.e1:
        golden_check(cap, a.e1, a.e1_prime, dict(s.split("=", 1) for s in a.linux_out))
    return 0


def golden_check(cap: Path, e1: Path, e1_prime: Path = None, linux_out: dict = None):
    """macOS outputs vs the E1 CPU goldens (P6/P7 rel L2; the encoder vs the bit-exact fca96f13)."""
    import hashlib
    import numpy as np
    print("== golden compares (macOS last-block outputs) ==")
    checks = [("p6", "y63.last.bin", e1 / "p6/in/golden.npy"),
              ("p7", "y.last.bin", e1 / "p7/in/golden.npy")]
    if e1_prime:
        checks.insert(1, ("p6prime", "y63.last.bin", e1_prime / "p6prime/in/golden.npy"))
    for probe, out_name, golden in checks:
        out = cap / "timings" / probe / out_name
        if not out.exists():
            print(f"  {probe}: no output captured")
            continue
        want = np.ascontiguousarray(np.load(golden), "<f2").tobytes()
        got = out.read_bytes()[:len(want)]
        eq = got == want
        if eq:
            print(f"  {probe}: bit-exact vs CPU golden")
            continue
        a = np.frombuffer(got, "<f2").astype(np.float64)
        b = np.frombuffer(want, "<f2").astype(np.float64)
        n = min(a.size, b.size)
        rel = float(np.linalg.norm(a[:n] - b[:n]) / max(np.linalg.norm(b[:n]), 1e-30))
        bad = int((~np.isfinite(a[:n])).sum())
        print(f"  {probe}: NOT bit-exact, rel L2 vs CPU golden {rel:.3e}, non-finite lanes {bad}/{n}")
        if probe in (linux_out or {}):
            lin = np.ascontiguousarray(np.load(linux_out[probe]), "<f2").tobytes()
            diff = int((np.frombuffer(got[:len(lin)], "<u2") != np.frombuffer(lin, "<u2")).sum())
            print(f"    vs the Linux output {linux_out[probe]}: "
                  + ("bitwise identical" if got[:len(lin)] == lin else f"{diff} fp16 lanes differ"))
    hid = cap / "timings" / "pk" / "hidden.last.bin"
    if hid.exists():
        sha = hashlib.sha256(hid.read_bytes()[:375 * 640 * 2]).hexdigest()
        print(f"  pk: encoder_hidden fp16 sha {sha[:8]} "
              f"{'== fca96f13 (bit-exact, golden)' if sha.startswith('fca96f13') else '!= fca96f13'}")


if __name__ == "__main__":
    raise SystemExit(main())
