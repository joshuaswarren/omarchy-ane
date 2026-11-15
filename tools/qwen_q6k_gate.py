#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host-only gate and paired timing of the Q6_K output head against the float32 head, on the REAL hidden vectors of the staged Qwen decode. No ANE, no GPU.

usage: qwen_q6k_gate.py GGUF AB_DIR HIDDEN_DIR [--gguf-py DIR] [--pair-steps 16] [--idle-ms 146] [--out FILE] [--no-timing]

Inputs: the GGUF; AB_DIR of a sealed identity run (identity-pNNN/A/logits.f32 and summary.json with each prompt's A_ids); HIDDEN_DIR with hidden_pNNN.npy (32 x 2048 fp32 per
prompt) recovered from those sealed logits (the hidden vector is the least-squares solution of logits = head @ hidden, checked against the sealed logits).

1. NUMERICS GATE. For every prompt and step: logits_q6k = Q6KHead.logits(hidden). Required: the argmax equals the sealed token id on all steps (and the 16 anchor ids on p001);
   the largest absolute logit difference to the sealed fp32 logits is below 0.1 of the smallest sealed top1-top2 margin over all steps. Reported: per-prompt max abs difference,
   relative L2, the smallest sealed margin, the number of steps.
2. PAIRED TIMING (unless --no-timing). One process, pinned to the performance cores like the launcher (pcore_mask, OPENBLAS_NUM_THREADS=4 when it pins): arm A the float32
   head (dequantized, 2.03 GB, `head @ hidden` on OpenBLAS), arm B the Q6_K head (8 threads). Arms A B A B, each --pair-steps steps on the p001 hidden vectors, --idle-ms of sleep
   between steps (the ANE phase of a decode step leaves the host idle for about 146 ms, so the head runs against a cold cache as in the decode). Each step times the head
   matvec plus top2 (the host work of the decode's head step except the logits file write, which is the same in both arms). Raw step times of every arm are kept.
   Gate: the median B/A ratio is at most 0.50 in BOTH pairs (A1,B1) and (A2,B2); the A/A spread |median A1 - median A2| is reported next to the difference.
Exit 0 only when every gate that ran holds."""
import argparse
import hashlib
import json
import os
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pcore_mask  # noqa: E402
from qwen_anchor import ANCHOR_TOKENS  # noqa: E402

PINNED = pcore_mask.pin_self()
if PINNED is not None:
    os.environ.setdefault("OPENBLAS_NUM_THREADS", "4")

import numpy as np  # noqa: E402

MARGIN_FRACTION = 0.1
RATIO_BAR = 0.50


def top2(logits):
    first = int(np.argmax(logits))
    rest = logits.copy()
    rest[first] = -np.inf
    second = int(np.argmax(rest))
    return first, float(logits[first]), second, float(logits[second])


def numerics(head_q6k, ab, hidden_dir, rows):
    summary = json.loads((ab / "summary.json").read_text())
    ids_by_prompt = {e["prompt"]: e["A_ids"] for e in summary["identity_prompts"]}
    report, bad, worst_all, margin_all = {}, [], 0.0, float("inf")
    if "p001" not in ids_by_prompt:
        return {"prompts": {}, "max_abs_diff_all": None, "min_sealed_margin_all": None, "margin_fraction": MARGIN_FRACTION}, ["the sealed run has no p001 (the 16 anchor ids and the margin gate need it)"]
    for pid in sorted(ids_by_prompt):
        hidden = np.load(hidden_dir / f"hidden_{pid}.npy")
        sealed = np.fromfile(ab / f"identity-{pid}" / "A" / "logits.f32", dtype=np.float32).reshape(-1, rows)
        if hidden.shape[0] != sealed.shape[0] or hidden.shape[0] != len(ids_by_prompt[pid]):
            bad.append(f"{pid}: {hidden.shape[0]} hidden, {sealed.shape[0]} sealed, {len(ids_by_prompt[pid])} ids")
            continue
        worst, rel, margin, ids_ok = 0.0, 0.0, float("inf"), True
        for s in range(hidden.shape[0]):
            got = head_q6k.logits(hidden[s])
            if not np.isfinite(got).all():
                bad.append(f"{pid} step {s}: non-finite logits")
                ids_ok = False
            ref = sealed[s]
            worst = max(worst, float(np.abs(got - ref).max()))
            rel = max(rel, float(np.linalg.norm(got - ref) / np.linalg.norm(ref)))
            a, v1, _, v2 = top2(ref)
            margin = min(margin, v1 - v2)
            ids_ok &= int(np.argmax(got)) == int(a) == int(ids_by_prompt[pid][s])
        report[pid] = {"steps": int(hidden.shape[0]), "max_abs_diff": worst, "max_rel_l2": rel, "min_sealed_margin": margin, "ids_equal_sealed": bool(ids_ok)}
        worst_all, margin_all = max(worst_all, worst), min(margin_all, margin)
        if not ids_ok:
            bad.append(f"{pid}: argmax differs from the sealed ids")
    if "p001" not in report:
        bad.append("p001 was not compared (missing or mismatched hidden vectors or logits)")
    elif ids_by_prompt["p001"][:16] != ANCHOR_TOKENS:
        bad.append("p001 sealed ids do not start with the 16 anchor ids")
    if margin_all == float("inf") or worst_all >= MARGIN_FRACTION * margin_all:
        bad.append(f"max abs logit difference {worst_all:.3g} is not below {MARGIN_FRACTION} x the smallest sealed margin {margin_all:.3g}")
    return {"prompts": report, "max_abs_diff_all": worst_all, "min_sealed_margin_all": margin_all, "margin_fraction": MARGIN_FRACTION}, bad


def timed_arm(fn, hidden, steps, idle_s):
    out = []
    for s in range(steps):
        time.sleep(idle_s)
        h = hidden[s % hidden.shape[0]]
        t = time.perf_counter()
        logits = fn(h)
        top2(logits)
        out.append((time.perf_counter() - t) * 1e3)
    return out


def pair_timing(head32, head_q6k, hidden, steps, idle_ms):
    arms = []
    for label in "ABAB":
        fn = (lambda h: head32 @ h) if label == "A" else head_q6k.logits
        fn(hidden[0])
        arms.append({"arm": label, "steps_ms": timed_arm(fn, hidden, steps, idle_ms / 1e3)})
    for a in arms:
        a["median_ms"], a["mean_ms"], a["min_ms"], a["max_ms"] = statistics.median(a["steps_ms"]), statistics.mean(a["steps_ms"]), min(a["steps_ms"]), max(a["steps_ms"])
    pairs = [arms[1]["median_ms"] / arms[0]["median_ms"], arms[3]["median_ms"] / arms[2]["median_ms"]]
    bad = [f"pair {i + 1}: B/A median ratio {r:.3f} is above {RATIO_BAR}" for i, r in enumerate(pairs) if not r <= RATIO_BAR]
    return {"arms": arms, "B_over_A_median": pairs, "AA_spread_ms": abs(arms[0]["median_ms"] - arms[2]["median_ms"]), "bar": RATIO_BAR}, bad


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("gguf")
    ap.add_argument("ab")
    ap.add_argument("hidden")
    ap.add_argument("--gguf-py", default="/var/tmp/qwen-decode/gguf-py")
    ap.add_argument("--pair-steps", type=int, default=16)
    ap.add_argument("--idle-ms", type=float, default=146.0)
    ap.add_argument("--no-timing", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    sys.path.insert(0, a.gguf_py)
    from gguf import GGMLQuantizationType, GGUFReader
    from gguf.quants import dequantize
    from q6k_head import Q6KHead

    reader = GGUFReader(a.gguf, "r")
    tensor = next(t for t in reader.tensors if t.name == "token_embd.weight")
    if tensor.tensor_type != GGMLQuantizationType.Q6_K:
        print(f"token_embd.weight is {tensor.tensor_type.name}, not Q6_K")
        return 2
    k, rows = int(tensor.shape[0]), int(tensor.shape[1])
    threads = len(pcore_mask.p_core_cpus() or []) or min(8, os.cpu_count() or 1)
    head_q6k = Q6KHead(np.asarray(tensor.data).reshape(rows, -1), rows, k, threads=threads)
    result = {"gguf_sha256": hashlib.sha256(Path(a.gguf).read_bytes()).hexdigest(), "pinned": PINNED, "q6k_threads": threads,
              "openblas_threads_env": os.environ.get("OPENBLAS_NUM_THREADS"), "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    result["numerics"], bad = numerics(head_q6k, Path(a.ab), Path(a.hidden), rows)
    if not a.no_timing:
        head32 = dequantize(np.asarray(tensor.data), tensor.tensor_type).astype(np.float32, copy=False)
        hidden = np.load(Path(a.hidden) / "hidden_p001.npy")
        result["timing"], tbad = pair_timing(head32, head_q6k, hidden, a.pair_steps, a.idle_ms)
        bad += tbad
    result["failures"] = bad
    text = json.dumps(result, indent=1)
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
