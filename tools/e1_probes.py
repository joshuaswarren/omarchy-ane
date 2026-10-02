#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Write the E1 clock probes: MIL, weights, seeded inputs and fp16 goldens.

P6 (compute-bound, legacy): LAYERS stacked 1x1 convs [1,C,H,W] -> [1,C,H,W] that all
share one orthogonal fp16 C x C weight, so the weight is 512 KiB, each activation is
512 KiB and the program does LAYERS * C * C * H * W MACs.
P6' (compute-bound, current): same shape class (64 stacked 1x1 convs 512->512 on
[1,512,16,32], one shared 512 KiB weight, 8.59e9 MAC, 64 tasks), input scaled by
INPUT_SCALE so the CPU fp16 reference max abs stays <= ~4 and the largest
intermediate (across all 64 layers) leaves a >= 100x margin against the fp16 ceiling
65504. Per-layer stats are written into the manifest.
P7 (activation stream): add of two [1,1024,128,128] fp16 tensors (32 MiB each).

  e1_probes.py OUT [--variant p6|p6prime|p7] [--check] [--only PROBE]

The goldens are CPU references: fp32 accumulate, fp16 rounding after every op, the
storage the ANE uses between layers. Every file is a pure function of SEED, so the
inputs and goldens can be rebuilt on any host. P6 and P6' are independent draws; P6
remains reproducible exactly.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np

SEED = 20261002
C, H, W, LAYERS = 512, 16, 32, 64
P7_SHAPE = (1, 1024, 128, 128)
HEADER = 64
WEIGHT_PATH = "@model_path/weights/weight.bin"
INPUT_SCALE = 0.8  # P6' input scale: leaves the CPU fp16 reference max abs <= ~4


def blob(payload: bytes) -> bytes:
    out = bytearray(128 + len(payload))
    struct.pack_into("<II", out, 0, 1, 2)
    struct.pack_into("<IQQ", out, HEADER, 0xDEADBEEF, len(payload), 128)
    out[128:] = payload
    return bytes(out)


def tensor(shape):
    return f"tensor<fp16, [{', '.join(map(str, shape))}]>"


def p6_mil() -> str:
    act, wt = tensor((1, C, H, W)), tensor((C, C, 1, 1))
    lines = [
        'string pt = const()[name = string("pt"), val = string("valid")];',
        'tensor<int32, [2]> st = const()[name = string("st"), val = tensor<int32, [2]>([1, 1])];',
        'tensor<int32, [4]> pd = const()[name = string("pd"), val = tensor<int32, [4]>([0, 0, 0, 0])];',
        'tensor<int32, [2]> dl = const()[name = string("dl"), val = tensor<int32, [2]>([1, 1])];',
        'int32 gp = const()[name = string("gp"), val = int32(1)];',
        f'{wt} w = const()[name = string("w"), val = {wt}(BLOBFILE(path = string("{WEIGHT_PATH}"), '
        f'offset = uint64({HEADER})))];',
    ]
    prev = "x"
    for i in range(LAYERS):
        lines.append(f'{act} y{i} = conv(dilations = dl, groups = gp, pad = pd, pad_type = pt, strides = st, '
                     f'weight = w, x = {prev})[name = string("y{i}")];')
        prev = f"y{i}"
    body = "\n".join(f"    {line}" for line in lines)
    return (f"program(1.3)\n[buildInfo = dict<string, string>({{}})]\n{{\n"
            f"  func main<ios18>({act} x) {{\n{body}\n  }} -> ({prev});\n}}\n")


def p7_mil() -> str:
    t = tensor(P7_SHAPE)
    return (f"program(1.3)\n[buildInfo = dict<string, string>({{}})]\n{{\n"
            f"  func main<ios18>({t} x, {t} z) {{\n"
            f'    {t} y = add(x = x, y = z)[name = string("y")];\n'
            f"  }} -> (y);\n}}\n")


def p6_data():
    rng = np.random.default_rng(SEED)
    q, r = np.linalg.qr(rng.standard_normal((C, C)))
    w = (q * np.sign(np.diag(r))).astype(np.float16)
    x = rng.standard_normal((1, C, H, W)).astype(np.float16)
    w32 = w.astype(np.float32)
    y = x.reshape(C, H * W)
    for _ in range(LAYERS):
        y = (w32 @ y.astype(np.float32)).astype(np.float16)
    return w, x, y.reshape(1, C, H, W)


def p6prime_data():
    """Same shape class and weight generation as P6, but with a scaled input so the
    CPU fp16 reference stays bounded. Per-layer stats are returned alongside the
    arrays so the manifest can record them.
    """
    rng = np.random.default_rng(SEED)
    q, r = np.linalg.qr(rng.standard_normal((C, C)))
    w = (q * np.sign(np.diag(r))).astype(np.float16)
    x = (INPUT_SCALE * rng.standard_normal((1, C, H, W))).astype(np.float16)
    w32 = w.astype(np.float32)
    y = x.astype(np.float32).reshape(C, H * W).copy()
    layer_abs = []
    for L in range(LAYERS):
        y = w32 @ y
        y_fp16 = y.astype(np.float16)
        layer_abs.append(float(np.abs(y_fp16).max()))
        y = y_fp16.astype(np.float32)
    golden = y_fp16.reshape(1, C, H, W)
    stats = {
        "input_scale": INPUT_SCALE,
        "weight_seed_digest": hashlib.sha256(w.tobytes()).hexdigest()[:16],
        "per_layer_absmax": layer_abs,
        "max_intermediate": max(layer_abs),
        "final_absmax": float(np.abs(golden).max()),
        "norm_ratio": float(np.linalg.norm(golden.astype(np.float32)) /
                            np.linalg.norm(x.astype(np.float32))),
        "fp16_margin_against_65504": 65504.0 / max(layer_abs),
    }
    return w, x, golden, stats


def p7_data():
    rng = np.random.default_rng(SEED + 7)
    a = rng.standard_normal(P7_SHAPE, dtype=np.float32).astype(np.float16)
    b = rng.standard_normal(P7_SHAPE, dtype=np.float32).astype(np.float16)
    return a, b, (a.astype(np.float64) + b).astype(np.float16)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_probe(d: Path, mil: str, weights: bytes, inputs: dict, golden: np.ndarray,
                info: dict) -> dict:
    (d / "weights").mkdir(parents=True, exist_ok=True)
    (d / "in").mkdir(exist_ok=True)
    (d / "model.mil").write_text(mil)
    (d / "weights" / "weight.bin").write_bytes(weights)
    (d / "weights.bin").write_bytes(weights)
    files = {"model.mil": sha(mil.encode()), "weights/weight.bin": sha(weights)}
    for name, arr in {**inputs, "golden": golden}.items():
        path = d / "in" / f"{name}.npy"
        np.save(path, arr)
        files[f"in/{name}.npy"] = sha(path.read_bytes())
    files["golden_fp16_sha256"] = sha(np.ascontiguousarray(golden, "<f2").tobytes())
    manifest = {**info, "seed": SEED, "sha256": files}
    (d / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--variant", choices=("p6", "p6prime", "p7", "all"),
                    default="all",
                    help="which probe(s) to write; default 'all' keeps P6 reproducible")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--only", choices=("p6", "p6prime", "p7"))
    a = ap.parse_args()
    variants = ("p6", "p6prime", "p7") if a.variant == "all" else (a.variant,)
    if a.only:
        variants = (a.only,)

    if "p6" in variants:
        w, x, y = p6_data()
        macs = LAYERS * C * C * H * W
        m6 = write_probe(a.out / "p6", p6_mil(), blob(w.tobytes()), {"x": x}, y, {
            "probe": "P6", "layers": LAYERS, "shape": [1, C, H, W], "macs": macs,
            "weight_bytes": w.nbytes, "activation_bytes": x.nbytes})
        if a.check:
            assert macs >= 5e9 and w.nbytes <= 1e6 and x.nbytes <= 1e6, m6
            ratio = float(np.linalg.norm(y.astype(np.float32)) / np.linalg.norm(x.astype(np.float32)))
            assert 0.9 < ratio < 1.1 and np.isfinite(y).all(), ratio
            assert p6_mil().count("conv(") == LAYERS and p6_mil().count("BLOBFILE") == 1
            print(f"P6 ok macs={macs:.3e} norm_ratio={ratio:.4f}")
        print(json.dumps(m6))

    if "p6prime" in variants:
        w, x, y, stats = p6prime_data()
        macs = LAYERS * C * C * H * W
        mp = write_probe(a.out / "p6prime", p6_mil(), blob(w.tobytes()), {"x": x}, y, {
            "probe": "P6'", "variant_of": "P6", "layers": LAYERS,
            "shape": [1, C, H, W], "macs": macs,
            "weight_bytes": w.nbytes, "activation_bytes": x.nbytes,
            "layer_stats": stats})
        if a.check:
            assert macs >= 5e9 and w.nbytes <= 1e6 and x.nbytes <= 1e6, mp
            assert np.isfinite(y).all(), "p6prime golden has non-finite lanes"
            assert stats["final_absmax"] <= 4.0, stats["final_absmax"]
            assert stats["max_intermediate"] <= 5.0, stats["max_intermediate"]
            assert stats["fp16_margin_against_65504"] >= 100.0, stats
            assert 0.9 < stats["norm_ratio"] < 1.1, stats
            assert p6_mil().count("conv(") == LAYERS and p6_mil().count("BLOBFILE") == 1
            print(f"P6' ok macs={macs:.3e} norm_ratio={stats['norm_ratio']:.4f} "
                  f"max_intermediate={stats['max_intermediate']:.3f} "
                  f"final_absmax={stats['final_absmax']:.3f} "
                  f"margin={stats['fp16_margin_against_65504']:.0f}x")
        print(json.dumps(mp))

    if "p7" in variants:
        x, z, y = p7_data()
        m7 = write_probe(a.out / "p7", p7_mil(), bytes(HEADER), {"x": x, "z": z}, y, {
            "probe": "P7", "shape": list(P7_SHAPE), "tensor_bytes": x.nbytes})
        if a.check:
            assert x.nbytes == 32 << 20 and np.isfinite(y).all() and p7_mil().count("add(") == 1
            print("P7 ok")
        print(json.dumps(m7))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
