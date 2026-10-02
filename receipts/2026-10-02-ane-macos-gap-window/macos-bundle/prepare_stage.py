#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Build the gap-window macOS staging dir from the E1 probe tree and the Parakeet encoder.

Verifies every file against pinned hashes (E1 manifests, the NativeMacRun encoder fixtures)
before staging; converts the fp16 fixtures to raw .bin surfaces; emits stage_manifest.sha256
(the manifest macOS re-checks before running) and a plan of what still needs fetching.

Usage:
  python3 prepare_stage.py --e1 /var/tmp/e1-probes --parakeet-mil <dir> --fixtures <dir> \
      --out /var/tmp/gapwin-stage [--regdump-src ~/src/omarchy-ane/tools/macos-regdump]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

# Pinned identities from the notebook record (NativeMacRun / NativeVsCross / AneClockRe).
PK_MIL_SHA = "4e3d2e8d"      # parakeet encoder model.mil (668,363 B)
PK_WEIGHT_SHA = "295dccd4"   # parakeet encoder weights/weight.bin (444,016,768 B)
FEATURES_SHA = "38dce85bf2cfab3fda6e0034fbe7534fc175047c21e00729b6599bfdeeeffa0c"
MASK_SHA = "e50598dd6ea415de61485666659fa648d026dc3351b50ae3a49137bee69687cb"
P6_X_BYTES = 512 * 16 * 32 * 2        # 524,288
P7_BYTES = 1024 * 128 * 128 * 2       # 33,554,432


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_manifest_probe(probe_dir: Path) -> None:
    m = json.loads((probe_dir / "manifest.json").read_text())
    for rel, want in m["sha256"].items():
        if not (probe_dir / rel).is_file():
            continue  # manifest also carries value hashes like golden_fp16_sha256
        got = sha256(probe_dir / rel)
        if got != want:
            sys.exit(f"FATAL: {probe_dir / rel} sha256 {got[:12]} != manifest {want[:12]}")


def tofile(npy_path: Path, out_bin: Path) -> None:
    import numpy as np
    a = np.load(npy_path)
    assert a.dtype == np.float16, f"{npy_path}: expected fp16, got {a.dtype}"
    a.astype("<f2").tofile(out_bin)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--e1", type=Path, required=True, help="E1 probe tree (p6/, p7/)")
    ap.add_argument("--parakeet-mil", type=Path, default=None,
                    help="the pinned encoder MIL (4e3d2e8d; ane-linux-experiments 2026-09-22 capture)")
    ap.add_argument("--parakeet-weights", type=Path, default=None,
                    help="the pinned encoder weights (295dccd4; encoder-v10/weights/weight.bin)")
    ap.add_argument("--fixtures", type=Path, default=None,
                    help="dir with input_features.npy + attention_mask.npy (Linux fp16 fixtures); "
                         "omit to stage without the encoder input bins (a PENDING notice prints)")
    ap.add_argument("--allow-missing-fixtures", action="store_true", dest="allow_missing",
                    help="with --fixtures: mismatch prints PENDING and stages the rest instead of failing")
    ap.add_argument("--inmem-src", type=Path, required=True,
                    help="path to the gap-window ane_inmem_run.m (tools/native-macos on the branch)")
    ap.add_argument("--regdump-src", type=Path, default=None,
                    help="tools/macos-regdump source to build the kext on-box")
    ap.add_argument("--inmem-bin", type=Path, default=None,
                    help="prebuilt ane_inmem_run (staged if given; else built on-box)")
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    out = a.out
    if out.exists():
        sys.exit(f"FATAL: {out} exists; remove it first (staging is one-shot, hashes pinned)")

    # 1. E1 probes against their own manifests, then copy with raw .bin fixtures.
    check_manifest_probe(a.e1 / "p6")
    check_manifest_probe(a.e1 / "p7")
    shutil.copytree(a.e1 / "p6", out / "p6")
    shutil.copytree(a.e1 / "p7", out / "p7")
    tofile(out / "p6/in/x.npy", out / "p6/in/p6-x.bin")
    tofile(out / "p7/in/x.npy", out / "p7/in/p7-x.bin")
    tofile(out / "p7/in/z.npy", out / "p7/in/p7-z.bin")
    for p in ("p6/in/p6-x.bin", "p7/in/p7-x.bin", "p7/in/p7-z.bin"):
        assert (out / p).stat().st_size in (P6_X_BYTES, P7_BYTES), f"{p} size"

    # 2. Parakeet encoder against the pinned NativeMacRun/ParakeetFull hashes.
    if not (a.parakeet_mil and a.parakeet_weights):
        print("PARAKEET ENCODER PENDING: pass --parakeet-mil/--parakeet-weights "
              "(pinned 4e3d2e8d / 295dccd4); staged WITHOUT the encoder arm.")
    else:
        mil, wt = a.parakeet_mil, a.parakeet_weights
        if sha256(mil)[:8] != PK_MIL_SHA:
            sys.exit(f"FATAL: {mil} sha {sha256(mil)[:8]} != pinned {PK_MIL_SHA}")
        if sha256(wt)[:8] != PK_WEIGHT_SHA:
            sys.exit(f"FATAL: {wt} sha {sha256(wt)[:8]} != pinned {PK_WEIGHT_SHA}")
        (out / "parakeet/weights").mkdir(parents=True)
        shutil.copy2(mil, out / "parakeet/model.mil")
        shutil.copy2(wt, out / "parakeet/weights/weight.bin")

    # 3. Encoder fixtures (Linux M2 /var/tmp/pk-enc/in) against the pinned hashes.
    pending = not (a.fixtures and (a.fixtures / "input_features.npy").exists())
    if pending:
        print("PARAKEET FIXTURES PENDING: stage input_features.npy + attention_mask.npy from the "
              "Linux /var/tmp/pk-enc/in before the window, or re-run with --fixtures.\n"
              f"  pinned: input_features {FEATURES_SHA}\n  pinned: attention_mask {MASK_SHA}\n"
              "  staged WITHOUT the encoder input bins (the encoder arm will skip on the Mac).")
    else:
        feats, mask = a.fixtures / "input_features.npy", a.fixtures / "attention_mask.npy"
        if sha256(feats) != FEATURES_SHA or sha256(mask) != MASK_SHA:
            msg = "FATAL: encoder fixtures do not match the pinned Linux fixture hashes"
            if a.allow_missing:
                print(msg + " - continuing WITHOUT them (--allow-missing-fixtures)")
                pending = True
            else:
                sys.exit(msg)
        if not pending:
            tofile(feats, out / "parakeet/in-in-features.bin")
            tofile(mask, out / "parakeet/in-in-mask.bin")

    # 4. Tools and the runtime ranges file.
    (out / "src").mkdir(parents=True, exist_ok=True)
    shutil.copy2(a.inmem_src, out / "src/ane_inmem_run.m")
    (out / "src/macos-regdump").mkdir(parents=True)
    if a.regdump_src:
        for item in ("build.sh", "capture.sh", "aneregdump.c", "aneprobe.swift",
                     "ane_regdump_filter.h", "ranges.txt", "test_filter.c"):
            src = a.regdump_src / item
            if src.exists():
                shutil.copy2(src, out / "src/macos-regdump" / item)
        kext_src = a.regdump_src / "ANERegDump"
        if kext_src.is_dir():
            shutil.copytree(kext_src, out / "src/macos-regdump/ANERegDump")
    if a.inmem_bin:
        shutil.copy2(a.inmem_bin, out / "ane_inmem_run")
        os.chmod(out / "ane_inmem_run", 0o755)

    # 5. Manifest (checked again on macOS before anything runs; sha256sum lines are
    #    `hash  path`, the same format `shasum -a 256 -c` verifies).
    lines = subprocess.run(
        ["find", ".", "-type", "f", "!", "-name", "stage_manifest.sha256",
         "-exec", "sha256sum", "{}", "+"], capture_output=True, text=True, cwd=out,
        check=True).stdout.splitlines()
    lines.sort()
    (out / "stage_manifest.sha256").write_text("\n".join(lines) + "\n")
    shutil.copy2(Path(__file__), out / "prepare_stage.py")
    bundle = Path(__file__).resolve().parent
    for f in ("macos_window.sh", "loopload.sh", "ranges-gapwin.txt"):
        if (bundle / f).exists():
            shutil.copy2(bundle / f, out / f)
    n = len(lines)
    print(f"staged {n} files under {out}; total "
          f"{sum(p.stat().st_size for p in out.rglob('*') if p.is_file()) >> 20} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
