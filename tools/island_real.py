#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Run Parakeet island inputs reconstructed by the CPU MIL reference on T6021."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from island_ref import ISLANDS, alloc_for_ch, in_arg_order, read_fp16_surface

ISLAND_CAPTURE = {
    "island-a-kt": ("a-kt", {5: "pos_kT", 6: "q_v"}, "attention_scores_1"),
    "island-a-attn-p1": ("a-p1", {5: "k_headsT", 6: "q_scaled"}, "matmul_0"),
    "island-c-pv": ("c-pv", {5: "v_heads", 6: "probs"}, "attn_output_1"),
    "island-b-select-runtime": (
        "b-select", {5: "matrix_bd_5", 6: "ninf_rt", 7: "cond"}, "attention_mask_9"
    ),
}
LAYERS = (0, 11, 23)
ANE_RUN = "/var/tmp/inst/tools/ane-run"
REMOTE_ROOT = "/var/tmp/islands-run/real-parakeet"


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def capture_record(records, layer, capture_island, name):
    matches = [r for r in records if r["layer"] == layer and r["island"] == capture_island]
    if len(matches) != 1:
        fail(f"manifest has {len(matches)} records for layer {layer} island {capture_island}")
    tensors = [t for t in matches[0]["tensors"] if t["name"] == name]
    if len(tensors) != 1:
        fail(f"manifest has {len(tensors)} tensors named {name} in {capture_island}/layer{layer}")
    return tensors[0]


def load_tensor(root, records, layer, capture_island, name, desc):
    record = capture_record(records, layer, capture_island, name)
    path = (root / record["path"]).resolve()
    if root.resolve() not in path.parents:
        fail(f"tensor path escapes capture root: {record['path']}")
    expected = (desc["N"], desc["C"], desc["H"], desc["W"])
    expected_dtype = "bool" if desc["dtype"] == "bool" else "float16"
    if record["shape"] != list(expected) or record["dtype"] != expected_dtype:
        fail(f"{path}: manifest shape/dtype {record['shape']}/{record['dtype']} != {expected}/{expected_dtype}")
    arr = np.load(path, allow_pickle=False)
    if arr.shape != expected:
        fail(f"{path}: shape {arr.shape} != {expected}")
    if arr.dtype != np.dtype(expected_dtype):
        fail(f"{path}: dtype {arr.dtype} != {expected_dtype}")
    return arr


def pack_surface(path, arr, desc):
    """Pack valid NCHW lanes into the proven aligned channel rows."""
    if desc["dtype"] == "fp16":
        surface = np.zeros(alloc_for_ch(desc) // 2, dtype=np.float16)
        row_stride = desc["row_bytes"] // 2
    else:
        surface = np.zeros(alloc_for_ch(desc), dtype=np.uint8)
        row_stride = desc["row_bytes"]
    n, c, h, w = arr.shape
    for ni in range(n):
        for ci in range(c):
            for hi in range(h):
                start = ((ni * c + ci) * h + hi) * row_stride
                surface[start:start + w] = arr[ni, ci, hi]
    surface.tofile(path)


def magnitude(arr):
    values = np.abs(arr.astype(np.float64, copy=False))
    return {"min": float(np.min(values)), "max": float(np.max(values)), "mean": float(np.mean(values))}


def reference_bmm(inputs):
    x = inputs[6].astype(np.float64)
    w = inputs[5].astype(np.float64)
    exact = np.einsum("bcmk,bckn->bcmn", x, w)
    sum_abs_terms = np.einsum("bcmk,bckn->bcmn", np.abs(x), np.abs(w))
    return exact, sum_abs_terms


def compare_to_capture(actual, expected):
    same = np.isfinite(actual) & np.isfinite(expected)
    equal_inf = np.isinf(actual) & np.isinf(expected) & (np.signbit(actual) == np.signbit(expected))
    delta = np.zeros(actual.shape, dtype=np.float64)
    delta[same] = actual[same].astype(np.float64) - expected[same].astype(np.float64)
    delta[~same & ~equal_inf] = np.inf
    den = np.linalg.norm(expected[same].astype(np.float64))
    rel = float(np.linalg.norm(delta[same]) / den) if den else (0.0 if not np.any(delta[same]) else float("inf"))
    max_abs = float(np.max(np.abs(delta[same]))) if np.any(same) else 0.0
    return {"rel_l2_finite_lanes": rel, "max_abs_finite_lanes": max_abs,
            "finite_comparison_lanes": int(np.count_nonzero(same)),
            "matching_signed_infinity_lanes": int(np.count_nonzero(equal_inf)),
            "mismatched_nonfinite_lanes": int(np.count_nonzero(~same & ~equal_inf))}


def bmm_metrics(actual, exact, sum_abs_terms):
    error = np.abs(actual.astype(np.float64) - exact)
    denominator = (2.0 ** -11) * sum_abs_terms
    ratio = np.divide(error, denominator, out=np.where(error == 0.0, 0.0, np.inf), where=denominator != 0.0)
    per_lane = ratio <= 1.0
    per_head = np.max(ratio, axis=(0, 2, 3))
    return {
        "exact_product": "PASS" if bool(np.all(per_lane)) else "FAIL",
        "lane_count": int(per_lane.size), "lanes_in_bound": int(np.count_nonzero(per_lane)),
        "lanes_out_of_bound": int(np.count_nonzero(~per_lane)),
        "worst_error_over_2^-11_sum_abs_terms": float(np.max(ratio)),
        "per_head_worst_ratio": [float(x) for x in per_head],
    }


def finite_counts(arr):
    return {"nan": int(np.count_nonzero(np.isnan(arr))), "inf": int(np.count_nonzero(np.isinf(arr)))}


def run_one(root, manifest, layer, island_name, host):
    island = ISLANDS[island_name]
    bundle, input_names, output_name = ISLAND_CAPTURE[island_name]
    inputs = {
        ch: load_tensor(root, manifest["captures"], layer, bundle, name, island["channels"][ch])
        for ch, name in input_names.items()
    }
    expected = load_tensor(root, manifest["captures"], layer, bundle, output_name, island["channels"][4])
    run_key = f"layer{layer}-{island_name}"
    remote_dir = f"{REMOTE_ROOT}/{run_key}"
    subprocess.run(["ssh", host, f"mkdir -p {shlex.quote(remote_dir)}"], check=True)
    input_paths = {}
    for ch, arr in inputs.items():
        local = Path("/tmp") / f"island-real-{run_key}-ch{ch}.bin"
        pack_surface(local, arr, island["channels"][ch])
        remote = f"{remote_dir}/in-ch{ch}.bin"
        subprocess.run(["scp", str(local), f"{host}:{remote}"], check=True)
        input_paths[ch] = remote
    remote_out = f"{remote_dir}/out-ch4.bin"
    anec = f"/var/tmp/inst/fixtures/h14-anec/{island_name}/program-0.anec"
    command = ["flock", "/var/tmp/ane-run.lock", "timeout", "60", ANE_RUN, "--anec", anec]
    for slot, ch in in_arg_order(island):
        command += ["--in", f"{slot}={input_paths[ch]}"]
    command += ["--out", f"0={remote_out}", "--time"]
    remote_command = " ".join(shlex.quote(part) for part in command)
    print(f"RUN {run_key}: ssh {host} {remote_command}", flush=True)
    result = subprocess.run(["ssh", host, remote_command], check=True, text=True, capture_output=True, timeout=75)
    print(result.stdout, end="", flush=True)
    local_out = Path("/tmp") / f"island-real-{run_key}-out.bin"
    subprocess.run(["scp", f"{host}:{remote_out}", str(local_out)], check=True)
    if local_out.stat().st_size != alloc_for_ch(island["channels"][4]):
        fail(f"{run_key}: device output size {local_out.stat().st_size} != {alloc_for_ch(island['channels'][4])}")
    actual = read_fp16_surface(local_out, island["channels"][4]).astype(np.float64)
    valid_expected = expected.astype(np.float64)
    metrics = {
        "layer": layer, "island": island_name, "kind": island["kind"],
        "capture_output": compare_to_capture(actual, valid_expected),
        "nan_inf": finite_counts(actual),
        "inputs_abs": {island["channels"][ch]["role"]: magnitude(arr) for ch, arr in inputs.items()},
        "output_abs": magnitude(actual),
    }
    if island["kind"] == "bmm":
        exact, sum_abs_terms = reference_bmm(inputs)
        metrics["exact_product"] = bmm_metrics(actual, exact, sum_abs_terms)
    else:
        cond = inputs[7].astype(bool)
        selected = np.where(cond, inputs[6], inputs[5])
        expected_bits = expected.view(np.uint16)
        actual_bits = actual.astype(np.float16).view(np.uint16)
        selected_bits = selected.astype(np.float16).view(np.uint16)
        metrics["select"] = {
            "bit_equal_to_capture": bool(np.array_equal(actual_bits, expected_bits)),
            "bit_equal_to_selected_branch": bool(np.array_equal(actual_bits, selected_bits)),
            "lanes": int(actual_bits.size),
            "matching_capture_lanes": int(np.count_nonzero(actual_bits == expected_bits)),
        }
        metrics["exact_product"] = "n/a"
    metrics["verdict"] = "PASS" if (
        metrics["nan_inf"] == {"nan": 0, "inf": 0}
        and (metrics["exact_product"]["exact_product"] == "PASS" if island["kind"] == "bmm" else
             metrics["select"]["bit_equal_to_capture"] and metrics["select"]["bit_equal_to_selected_branch"])
    ) else "FAIL"
    print(json.dumps(metrics, indent=2, sort_keys=True), flush=True)
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture-dir", type=Path, default=Path("/var/tmp/parakeet-island-captures"))
    parser.add_argument("--host", required=True, help="SSH alias of the M2 host")
    parser.add_argument("--layers", nargs="+", type=int, choices=LAYERS, default=list(LAYERS))
    parser.add_argument("--islands", nargs="+", choices=tuple(ISLAND_CAPTURE), default=list(ISLAND_CAPTURE))
    parser.add_argument("--result", type=Path)
    args = parser.parse_args()
    root = args.capture_dir.resolve()
    manifest = json.loads((root / "manifest.json").read_text())
    if manifest.get("classification") != "RECONSTRUCTED_REFERENCE_NOT_PRODUCT_PATH_CAPTURE":
        fail(f"unexpected manifest classification: {manifest.get('classification')}")
    if len(manifest.get("captures", [])) != len(LAYERS) * len(ISLAND_CAPTURE):
        fail("manifest does not contain the expected 12 layer/island capture records")
    results = [run_one(root, manifest, layer, island, args.host) for layer in args.layers for island in args.islands]
    if args.result:
        args.result.write_text(json.dumps({"classification": manifest["classification"], "results": results}, indent=2) + "\n")
    if any(row["verdict"] != "PASS" for row in results):
        fail("one or more real-input island runs failed")


if __name__ == "__main__":
    main()
