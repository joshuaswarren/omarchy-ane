#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Check the whole Parakeet encoder output of the M2 ANE against the golden.

The encoder runs on the M2 as one Apple-compiled H14 program (one call, every
op on the ANE). This tool compares its encoder_hidden with the golden capture
and with the CPU NumPy MIL reference, then decodes the transcript with the
greedy TDT decoder of tools/parakeet_encoder_islands.py. Device run (on the M2):

    python3 tools/qwen_prog_run.py --prog parakeet_encoder --anec-dir DIR \\
        --timeout 120 --repeat 20 \\
        --in attention_mask=mask.npy --in input_features=features.npy \\
        --out linear_217_cast_fp16=hidden.npy --out output_mask_f=out_mask.npy

DIR/parakeet_encoder holds program-0.anec (tools/hwx_h14_staged_to_anec.py)
and ports.json (tools/hwx_ports.py derive_program). Inputs are the capture's
encoder_input_features and encoder_input_mask cast to fp16.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from parakeet_encoder_islands import GreedyTdt, detokenize, diff, load_tokenizer, wer

CACHE = Path.home() / ".cache/mlx-omarchy/parakeet-reference"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--hidden", type=Path, required=True, help="device encoder_hidden .npy")
    ap.add_argument("--mask", type=Path, required=True, help="device output_mask .npy")
    ap.add_argument("--baseline", type=Path, required=True,
                    help="CPU NumPy MIL reference encoder_hidden .npy")
    ap.add_argument("--capture", type=Path, default=CACHE / (
        "captures/b650695c-75aec2a/20260912T154759Z-librispeech/ane"))
    ap.add_argument("--model-dir", type=Path, default=CACHE / (
        "mweinbach1/parakeet-tdt-0.6b-v3-coreml/b650695c2322ee5281dff48d7345b2f3a58ff018"))
    ap.add_argument("--lut", type=Path, default=Path.home() / (
        "src/mlx-omarchy/overlay/tools/coreml/fused_lut_2026-09-15.npz"))
    ap.add_argument("--out", type=Path, required=True, help="report.json")
    args = ap.parse_args()

    hidden = np.load(args.hidden).reshape(1, 375, 640)
    mask = np.load(args.mask).reshape(-1)
    if not np.isfinite(hidden).all():
        raise SystemExit("FAIL: non-finite encoder output")
    golden = np.load(args.capture / "encoder_hidden.npy").astype(np.float32)
    golden_mask = np.load(args.capture / "encoder_mask.npy").reshape(-1)
    golden_tokens = json.loads((args.capture / "token_ids.json").read_text())["token_ids"]
    golden_text = (args.capture / "transcript.txt").read_text().strip()

    tdt = GreedyTdt(args.model_dir / "decoder.mlpackage", args.model_dir / "joint.mlpackage",
                    None, args.lut)
    tokens = tdt.decode(hidden.astype(np.float32), int(golden_mask.sum()))
    text = detokenize(tokens, load_tokenizer(args.model_dir / "tokenizer.json"))
    report = {
        "hidden_vs_golden": diff(hidden, golden),
        "hidden_vs_cpu_reference": diff(hidden, np.load(args.baseline)),
        "mask_equal_golden": bool(np.array_equal(mask.astype(np.int64), golden_mask.astype(np.int64))),
        "token_count": len(tokens),
        "tokens_equal_golden": tokens == golden_tokens,
        "transcript": text,
        "transcript_equal_golden": text == golden_text,
        "wer_vs_golden_transcript": wer(golden_text, text),
        "tokens": tokens,
    }
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "tokens"}, indent=2))
    return 0 if report["tokens_equal_golden"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
