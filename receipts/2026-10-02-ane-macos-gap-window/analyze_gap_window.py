#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Host-side analysis for the T6021 ANE gap window (runnable standalone on synthetic data).

Takes the macOS capture directory (the bundle's out/) plus the Linux E1 results JSON and prints:
  - the per-probe ratio table (Linux/macOS minmin and medmed, P6 / P7 / Parakeet encoder),
  - the E1 decision per the GapRank H1 rule (P6 ratio >= 2.0 supports the low-clock hypothesis,
    <= 1.15 while the activation-bound arms carry the ratio falsifies it),
  - the fabric-ps / dcs-ps words (DESIRED[3:0]) idle vs load per OS.

Inputs:
  --macos-capture DIR   bundle out/ (timings/<probe>/blocks.tsv + block-*.json, 03-/04- regdump)
  --linux-e1 FILE       Linux E1 results JSON:
                        {"p6": {"blocks": [[min_ms, median_ms], ...]},
                         "p7": {...}, "pk": {...}}   (empty/absent = "pending")
  --linux-ps FILE       optional Linux ps-words JSON:
                        {"idle": {"fabric_ps": "0x777", "dcs_ps": "0x999"},
                         "load": {...}}           (absent = printed as n/a)
  --synthetic           generate a self-consistent fake capture + fake Linux result and analyze
                        that (no hardware needed; exercises every code path)

macOS numbers are upper bounds when captured under load; the report says so (the stamps carry load).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

PROBES = ("p6", "p7", "pk")
NAMES = {"p6": "P6 compute-bound (64x conv1x1, L2-resident weights)",
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


def ps_words(regdump_dir: Path):
    """fabric-ps / dcs-ps 4-byte words from an aneregdump output dir (index.json + name.bin)."""
    words = {}
    for name in ("fabric-ps", "dcs-ps"):
        f = regdump_dir / f"{name}.bin"
        if f.exists() and f.stat().st_size >= 4:
            words[name] = "0x%08x" % struct.unpack("<I", f.read_bytes()[:4])[0]
        elif f.exists():
            words[name] = f"short-read:{f.stat().st_size}B"
    idx = regdump_dir / "index.json"
    if idx.exists():
        try:
            j = json.loads(idx.read_text())
            words["islands_up"] = j.get("islands_up")
        except json.JSONDecodeError:
            pass
    return words or None


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


def decision(r6, r7, rpk):
    if r6 is None:
        return "P6 missing - no E1 decision possible"
    lines = []
    if r6 >= 2.0:
        lines.append(f"P6 ratio {r6:.2f} >= 2.0: SUPPORTS H1 (ANE operating point left low under "
                     "Linux; the compute-bound probe carries the ratio)")
    elif r6 <= 1.15:
        if (r7 is not None and r7 >= 2.0) or (rpk is not None and rpk >= 2.0):
            lines.append(f"P6 ratio {r6:.2f} <= 1.15 while activation-bound arms carry the ratio: "
                         "FALSIFIES H1 (clock is not the cause; memory-side H3/H12 class)")
        else:
            lines.append(f"P6 ratio {r6:.2f} <= 1.15 but nothing else carries the ratio: "
                         "H1 unsupported, cause likely driver/firmware-path shared cost")
    else:
        lines.append(f"P6 ratio {r6:.2f} in the unresolved band (1.15, 2.0): mixed; report both")
    if r7 is not None and r6 is not None:
        if r7 > r6 * 1.5:
            lines.append(f"P7 {r7:.2f} >> P6 {r6:.2f}: the gap concentrates in the DMA/activation "
                         "path (H3 DCS/fabric QoS or H12 non-cacheable BO mappings)")
        elif r6 > r7 * 1.5:
            lines.append(f"P6 {r6:.2f} >> P7 {r7:.2f}: the gap concentrates in compute, consistent "
                         "with a low NE clock rather than memory side")
        else:
            lines.append(f"P7 {r7:.2f} ~ P6 {r6:.2f}: uniform ratio, one shared cause (clock-class)")
    if rpk is not None:
        lines.append(f"encoder ratio {rpk:.2f} vs the whole-encoder reference 2.83 "
                     "(NativeVsCross/NativeMacRun)")
    return "\n  ".join(lines)


def print_ps(os_name, words_idle, words_load):
    if not words_idle and not words_load:
        print(f"  {os_name}: n/a (no capture)")
        return
    for tag, w in (("idle", words_idle), ("load", words_load)):
        if not w:
            print(f"  {os_name} {tag}: missing")
            continue
        if w.get("skipped"):
            print(f"  {os_name} {tag}: SKIPPED ({w['skipped']})")
            continue
        fab, dcs = w.get("fabric-ps"), w.get("dcs-ps")
        iu = w.get("islands_up")
        print(f"  {os_name} {tag}: fabric-ps {fab} (DESIRED={desired(fab)}) "
              f"dcs-ps {dcs} (DESIRED={desired(dcs)}) islands_up={iu}")


def load_capture(cap: Path):
    idle = ps_words(cap / "03-regdump-idle" / "regdump")
    load = ps_words(cap / "04-regdump-load" / "regdump")
    for tag in ("03-regdump-idle", "04-regdump-load"):
        skipped = cap / tag / "SKIPPED"
        if skipped.exists():
            note = {"skipped": skipped.read_text().strip()}
            if tag.endswith("idle"):
                idle = note
            else:
                load = note
    return idle, load


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--macos-capture", type=Path)
    ap.add_argument("--linux-e1", type=Path)
    ap.add_argument("--linux-ps", type=Path)
    ap.add_argument("--e1", type=Path, help="E1 probe tree for golden output compares")
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
        shape = {"p6": (2.4, 2.2), "p7": (7.0, 2.9), "pk": (89.3, 2.84)}
        (cap / "timings").mkdir(parents=True)
        for probe, (mac_base, ratio) in shape.items():
            d = cap / "timings" / probe
            d.mkdir(parents=True)
            with open(d / "blocks.tsv", "w") as f:
                for b in range(20):
                    mac = mac_base * float(rng.uniform(0.99, 1.01))
                    f.write(f"block-{b:02d}.json\t{mac:.3f}\t{mac * 1.01:.3f}\t20\t"
                            f"ts=synth load=2.1 uptime_s=900\n")
        for tag, fab, dcs in (("03-regdump-idle", 0xF, 0x9), ("04-regdump-load", 0xE, 0x8)):
            rd = cap / tag / "regdump"
            rd.mkdir(parents=True)
            (rd / "fabric-ps.bin").write_bytes(struct.pack("<I", fab))
            (rd / "dcs-ps.bin").write_bytes(struct.pack("<I", dcs))
            (rd / "index.json").write_text('{"islands_up": 15}')
        lin = {}
        for p in PROBES:
            mac_base, ratio = shape[p]
            lin[p] = {"minmin": mac_base * ratio, "medmed": mac_base * ratio * 1.01, "blocks": 3}
        lps = {"idle": {"fabric_ps": "0x00000777", "dcs_ps": "0x00000999"},
               "load": {"fabric_ps": "0x00000777", "dcs_ps": "0x00000999"}}
    else:
        if not a.macos_capture or not a.linux_e1:
            ap.error("need --macos-capture and --linux-e1 (or --synthetic)")
        cap = a.macos_capture
        lin_raw = json.loads(a.linux_e1.read_text())
        lin = {p: ({"minmin": min(b[0] for b in lin_raw[p]["blocks"]),
                    "medmed": sorted(b[1] for b in lin_raw[p]["blocks"])[len(lin_raw[p]["blocks"]) // 2],
                    "blocks": len(lin_raw[p]["blocks"])} if lin_raw.get(p, {}).get("blocks") else None)
               for p in PROBES}
        lps = json.loads(a.linux_ps.read_text()) if a.linux_ps else None

    mac = macos_stats(cap)
    idle, load = load_capture(cap)

    print("== E1 ratio table (Linux / macOS, minmin and medmed) ==")
    ratios = {}
    for p in PROBES:
        r = None
        if mac[p] and lin[p]:
            r = lin[p]["minmin"] / mac[p]["minmin"]
            ratios[p] = r
        print(f"  {NAMES[p]}\n    {fmt_side(lin[p], 'Linux')}\n    {fmt_side(mac[p], 'macOS')}"
              + (f"\n    ratio minmin {r:.3f}" if r else "\n    ratio: pending"))
    print("== E1 decision (GapRank H1 rule) ==")
    print("  " + decision(ratios.get("p6"), ratios.get("p7"), ratios.get("pk")))
    print("== fabric-ps / dcs-ps words (DESIRED[3:0]) idle vs load per OS ==")
    print_ps("macOS", idle, load)
    if lps:
        lw = {}
        for tag, src in (("idle", lps.get("idle")), ("load", lps.get("load"))):
            if src:
                lw = {"fabric-ps": src.get("fabric_ps"), "dcs-ps": src.get("dcs_ps")}
                print(f"  Linux {tag}: fabric-ps {lw['fabric-ps']} (DESIRED={desired(lw['fabric-ps'])}) "
                      f"dcs-ps {lw['dcs-ps']} (DESIRED={desired(lw['dcs-ps'])})")
            else:
                print(f"  Linux {tag}: missing")
    else:
        print("  Linux: n/a (no read route staged; a T6021 read-only sysps module does not exist yet)")
    print("== caveats ==")
    print("  macOS numbers carry the load stamps next to each block; load lengthens times, so a")
    print("  macOS min is an upper bound and every ratio a lower bound (NativeMacRun precedent).")
    if a.e1:
        golden_check(cap, a.e1)
    return 0


def golden_check(cap: Path, e1: Path):
    """macOS outputs vs the E1 CPU goldens (P6/P7 rel L2; the encoder vs the bit-exact fca96f13)."""
    import hashlib
    import numpy as np
    print("== golden compares (macOS last-block outputs) ==")
    checks = (("p6", "y63.last.bin", e1 / "p6/in/golden.npy"),
              ("p7", "y.last.bin", e1 / "p7/in/golden.npy"))
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
        print(f"  {probe}: NOT bit-exact, rel L2 vs CPU golden {rel:.3e} "
              "(ANE accumulation order may differ; band like the Linux leg's golden line)")
    hid = cap / "timings" / "pk" / "hidden.last.bin"
    if hid.exists():
        sha = hashlib.sha256(hid.read_bytes()[:375 * 640 * 2]).hexdigest()
        print(f"  pk: encoder_hidden fp16 sha {sha[:8]} "
              f"{'== fca96f13 (bit-exact, golden)' if sha.startswith('fca96f13') else '!= fca96f13'}")


if __name__ == "__main__":
    raise SystemExit(main())
