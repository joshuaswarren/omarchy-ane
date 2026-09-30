#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Reference / device comparison for the M2 ANE Parakeet island ANECs.

Six islands under test:
  island-c-pv, island-a-kt, island-a-attn-p1, island-b-select-runtime,
  island-b-select-constfill, rms-c2048-gamma

The ANEC header's nchw[] fields are zero (the encoder does not populate
them; ane_m2_open copies the raw header bytes into the anec struct). So
the check tool cannot derive shapes from the header alone. This script
bakes the per-island shape from the H14 compiler oracle templates
recorded at /home/joshuawarren/src/mil-hwx-h14-mint-wt/research/oracles/h14/
(plural: H14IslandTemplates.inc, H14RmsNormTemplates.inc), generates
seeded random inputs in the surface layout, runs ane-run (no --check),
reads the device output, and compares against numpy references using
the matvec-style tolerance band:
  matmul / rms: 2 ulp of fp16 OR 4 cond-units where cond-unit = 2^-11.
  select:      bit-exact equality.

The (a, b, cond) order at runtime -- in 0,1,2 -- follows the MIL
declaration order per the existing ane-run comment; this script does
NOT enforce it: it tests every plausible operand order / transpose and
reports which one matches the device.

Usage:
  island_ref.py --island NAME [--seed N] [--anec PATH] [--in-dir DIR]
                 [--out-dir DIR] [--gamma FILE]
"""

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

# --------------------------------------------------------------------------
# Per-island layout table (oracle truth).
# --------------------------------------------------------------------------
# For every island we know:
#   - which channels carry which logical tensor
#   - dtype (fp16 or bool)
#   - (N, C, H, W, plane_bytes, row_bytes) for each
#
# We never trust the ANEC header nchw[]: it is always zero.
# We do trust the header tiles[]: it sets the channel allocation in
# 0x4000 B units and must agree with plane * C * H within rounding.

ISLANDS = {
    # island-c-pv: gabmm_r3_m375_k375_n128_tx0_ty0_b8
    # MIL matmul(x, w) where x=[8,375,375], w=[8,375,128] -> product=[8,375,128].
    # x [B=8, M=375, K=375]; w [B=8, K=375, N=128]; product [B=8, M=375, N=128].
    # Tensor bindings list (binding 1 = inputs in MIL declaration order):
    #   binding 1, [1,8,375,375] 2304000 B  -> x
    #   binding 1, [1,8,375,128]  768000 B  -> w
    #   binding 2, [1,8,375,128]  768000 B  -> product (output)
    # Encoder template kH14BatchedTensors6 (the proven channel map):
    #   ch5 = [1,8,375,128] alloc 770048    -> w (second MIL input)
    #   ch6 = [1,8,375,375] alloc 2310144   -> x (first MIL input)
    #   ch4 = [1,8,375,128] alloc 770048    -> product
    # ANEC tiles: ch4=47, ch5=47, ch6=141.  ch5 alloc 770048 matches w
    # [1,8,375,128] logical, ch6 alloc 2310144 matches x [1,8,375,375].
    # NOTE: the kernel swaps operands -- ch5 is the SECOND MIL input,
    # ch6 is the FIRST. Reference: out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n].
    "island-c-pv": {
        "kind": "bmm",
        "channels": {
            5: dict(role="w", dtype="fp16", N=1, C=8, H=375, W=128,
                    plane_bytes=96000, row_bytes=256),
            6: dict(role="x", dtype="fp16", N=1, C=8, H=375, W=375,
                    plane_bytes=288000, row_bytes=768),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=128,
                    plane_bytes=96000, row_bytes=256),
        },
        # The MIL: matmul(x, w). ch5=w [B,K,N]=(1,8,375,128), ch6=x
        # [B,M,K]=(1,8,375,375). Note w's surface carries [B=1,K=375,N=128]
        # in MIL order (the [K, N] layout: H=375=K, W=128=N), so
        # out[b,m,n] = sum_k x[b,m,k] * w[b,k,n] reads w[b,k,n] =
        # w_surface[b, k, n] (no transpose needed).
    },

    # island-a-kt: gabmm_r3_m375_k128_n749_tx0_ty0_b8
    # MIL matmul(x, w): x=[8,375,128] (M=375, K=128), w=[8,128,749]
    # (K=128, N=749), product=[8,375,749].
    # Encoder kH14BatchedTensors4:
    #   ch5=[1,8,128,749] alloc 1572864  -> w (second MIL input)
    #   ch6=[1,8,375,128] alloc 770048   -> x (first MIL input)
    #   ch4=[1,8,375,749] alloc 4620288  -> product
    # ANEC tiles: ch4=282, ch5=96, ch6=47.  The "kt" name = K^T;
    # this is the Q @ K^T projection in attention (375 query rows
    # over 749 context keys).
    # Reference: out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n].
    "island-a-kt": {
        "kind": "bmm",
        "channels": {
            5: dict(role="w", dtype="fp16", N=1, C=8, H=128, W=749,
                    plane_bytes=196608, row_bytes=1536),
            6: dict(role="x", dtype="fp16", N=1, C=8, H=375, W=128,
                    plane_bytes=96000, row_bytes=256),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=749,
                    plane_bytes=576000, row_bytes=1536),
        },
    },

    # island-a-attn-p1: gabmm_r4heads_m375_k128_n375_tx0_ty0_b8
    # MIL matmul(x, w): x=[8,375,128] (M=375, K=128), w=[8,128,375]
    # (K=128, N=375), product=[8,375,375].  The "r4heads" prefix
    # signals 4 attention heads run side-by-side as 8 channels (one
    # output channel each of M=375, plus 4 more channels for the
    # second head's slice).
    # Encoder kH14BatchedTensors7:
    #   ch5=[1,8,128,375] alloc 786432  -> w (second MIL input)
    #   ch6=[1,8,375,128] alloc 770048  -> x (first MIL input)
    #   ch4=[1,8,375,375] alloc 2310144 -> product
    # ANEC tiles: ch4=141, ch5=48, ch6=47.
    # Reference: out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n].
    "island-a-attn-p1": {
        "kind": "bmm",
        "channels": {
            5: dict(role="w", dtype="fp16", N=1, C=8, H=128, W=375,
                    plane_bytes=98304, row_bytes=768),
            6: dict(role="x", dtype="fp16", N=1, C=8, H=375, W=128,
                    plane_bytes=96000, row_bytes=256),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=375,
                    plane_bytes=288000, row_bytes=768),
        },
    },

    # island-b-select-runtime: gasel_rrb_1x8x375x375
    # MIL select(a, b, cond) where a, b, cond are all runtime inputs.
    # ANEC tiles: ch4=141, ch5=141, ch6=141, ch7=71.
    # Reference: out[i] = cond[i] ? a[i] : b[i].
    "island-b-select-runtime": {
        "kind": "select",
        "channels": {
            5: dict(role="a", dtype="fp16", N=1, C=8, H=375, W=375,
                    plane_bytes=288000, row_bytes=768),
            6: dict(role="b", dtype="fp16", N=1, C=8, H=375, W=375,
                    plane_bytes=288000, row_bytes=768),
            7: dict(role="cond", dtype="bool", N=1, C=8, H=375, W=375,
                    plane_bytes=144000, row_bytes=384),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=375,
                    plane_bytes=288000, row_bytes=768),
        },
    },

    # island-b-select-constfill: gasel_ninf_1x8x375x375
    # MIL select(a = a_const, b = b_runtime, cond = cond_runtime)
    # where a = fp16 -inf constant (loaded from BLOBFILE weights.bin,
    # 2.25 MiB; the kernel section is 2256256 B).  Runtime inputs are
    # b (fp16 [1,8,375,375]) and cond (bool [1,8,375,375]).
    # ANEC tiles: ch4=141 (out), ch5=141 (fp16 input), ch6=71 (bool
    # input).  Per MANIFEST:
    #   ch5 = [1,8,375,375] fp16 alloc 2310144
    #   ch6 = [1,8,375,375] bool alloc 1163264
    # The MIL declaration order is (a_const, b, cond). After
    # stripping constants, the runtime inputs are (b, cond). Channel
    # order in the encoder: ch5 = first runtime input (b), ch6 =
    # second runtime input (cond). The output formula is
    # y = cond ? a : b = cond ? -inf : b.
    "island-b-select-constfill": {
        "kind": "select_constfill",
        "channels": {
            5: dict(role="b", dtype="fp16", N=1, C=8, H=375, W=375,
                    plane_bytes=288000, row_bytes=768),
            6: dict(role="cond", dtype="bool", N=1, C=8, H=375, W=375,
                    plane_bytes=144000, row_bytes=384),
            4: dict(role="out", dtype="fp16", N=1, C=8, H=375, W=375,
                    plane_bytes=288000, row_bytes=768),
        },
        "a_fill_value": -np.inf,  # the constant a = -inf
    },

    # rms-c2048-gamma: garms_chain_c2048_gamma
    # ANEC tiles: ch4=8, ch5=8 (alloc 131072 each).
    # x [1,2048,1,1] fp16. gamma from weights BLOBFILE (fp16 [2048]
    # at offset 64). y [1,2048,1,1] fp16.
    "rms-c2048-gamma": {
        "kind": "rms",
        "channels": {
            5: dict(role="x", dtype="fp16", N=1, C=2048, H=1, W=1,
                    plane_bytes=64, row_bytes=64),
            4: dict(role="out", dtype="fp16", N=1, C=2048, H=1, W=1,
                    plane_bytes=64, row_bytes=64),
        },
    },
}

ANE_RUN = "/var/tmp/inst/tools/ane-run"
ANEC_FIXTURE_ROOT = "/var/tmp/islands-fixtures"


# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------

def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def read_anec_tiles(path):
    """Read tiles[32] from the ANEC header."""
    with open(path, "rb") as f:
        hdr = f.read(0x1000)
    if len(hdr) < 0xa8 + 32 * 4:
        fail(f"{path} too short for header")
    return list(struct.unpack_from("<32I", hdr, 0x28))


def check_alloc_matches(anec_path, island):
    tiles = read_anec_tiles(anec_path)
    for ch, desc in island["channels"].items():
        want_alloc = ((desc["C"] * desc["H"] * desc["row_bytes"]) + 0x3fff) & ~0x3fff
        if tiles[ch] * 0x4000 < want_alloc:
            fail(f"{anec_path} ch{ch} alloc {tiles[ch]*0x4000} < "
                 f"layout-derived alloc {want_alloc} (role={desc['role']})")


def alloc_for_ch(desc):
    return ((desc["C"] * desc["H"] * desc["row_bytes"]) + 0x3fff) & ~0x3fff


def write_fp16_surface(path, desc, seed):
    """fp16 surface: valid lanes filled, padding zero."""
    rng = np.random.default_rng(seed)
    N, C, H, W = desc["N"], desc["C"], desc["H"], desc["W"]
    row_bytes = desc["row_bytes"]
    elem = N * C * H * W
    alloc = alloc_for_ch(desc)
    arr = np.zeros(alloc // 2, dtype=np.float16)
    flat = rng.uniform(-1.0, 1.0, elem).astype(np.float16)
    idx = 0
    for n in range(N):
        for c in range(C):
            for h in range(H):
                base = ((n * C + c) * H + h) * (row_bytes // 2)
                arr[base : base + W] = flat[idx : idx + W]
                idx += W
    arr.tofile(path)


def write_bool_surface(path, desc, seed):
    rng = np.random.default_rng(seed)
    N, C, H, W = desc["N"], desc["C"], desc["H"], desc["W"]
    row_bytes = desc["row_bytes"]
    elem = N * C * H * W
    alloc = alloc_for_ch(desc)
    arr = np.zeros(alloc, dtype=np.uint8)
    for n in range(N):
        for c in range(C):
            for h in range(H):
                base = ((n * C + c) * H + h) * row_bytes
                arr[base : base + W] = rng.integers(0, 2, W).astype(np.uint8)
    arr.tofile(path)


def gen_rms_gamma(path, length, seed):
    """64-byte BLOBFILE sub-header + fp16 [length] gamma."""
    rng = np.random.default_rng(seed)
    arr = np.zeros(64 // 2, dtype=np.float16)
    gamma = rng.uniform(-1.0, 1.0, length).astype(np.float16)
    np.concatenate([arr, gamma]).tofile(path)


def read_fp16_surface(path, desc):
    """Return a [N, C, H, W] array with valid lanes from the surface layout."""
    N, C, H, W = desc["N"], desc["C"], desc["H"], desc["W"]
    row_bytes = desc["row_bytes"]
    flat = np.fromfile(path, dtype=np.float16)
    out = np.zeros((N, C, H, W), dtype=np.float16)
    for n in range(N):
        for c in range(C):
            for h in range(H):
                base = ((n * C + c) * H + h) * (row_bytes // 2)
                out[n, c, h] = flat[base : base + W]
    return out


def read_bool_surface(path, desc):
    N, C, H, W = desc["N"], desc["C"], desc["H"], desc["W"]
    row_bytes = desc["row_bytes"]
    flat = np.fromfile(path, dtype=np.uint8)
    out = np.zeros((N, C, H, W), dtype=np.uint8)
    for n in range(N):
        for c in range(C):
            for h in range(H):
                base = ((n * C + c) * H + h) * row_bytes
                out[n, c, h] = flat[base : base + W]
    return out


# --------------------------------------------------------------------------
# Reference computations
# --------------------------------------------------------------------------

def bmm_reference(island, ch5_path, ch6_path, out_path):
    """Compute the matmul reference for the given island.

    ch5 carries the SECOND MIL input (w, [B,K,N] surface layout);
    ch6 carries the FIRST MIL input (x, [B,M,K] surface layout).
    Output: out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n].
    """
    cd_w = island["channels"][5]   # w [B,C,K,N]
    cd_x = island["channels"][6]   # x [B,C,M,K]
    cd_o = island["channels"][4]   # out [B,C,M,N]
    w = read_fp16_surface(ch5_path, cd_w).astype(np.float64)  # [B,C,K,N]
    x = read_fp16_surface(ch6_path, cd_x).astype(np.float64)  # [B,C,M,K]
    B, C, K, N = w.shape
    _, _, M, _ = x.shape

    # Reference formula:
    #   out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n]
    out = np.einsum("bcmk,bckn->bcmn", x, w)

    arr = np.zeros(alloc_for_ch(cd_o) // 2, dtype=np.float16)
    row_bytes_o = cd_o["row_bytes"]
    for n in range(B):
        for c in range(C):
            for h in range(M):
                base = ((n * C + c) * M + h) * (row_bytes_o // 2)
                arr[base : base + N] = out[n, c, h].astype(np.float16)
    arr.tofile(out_path)
    return arr, out


def rms_reference(island, x_path, gamma_path, out_path):
    cd_x = island["channels"][5]
    cd_o = island["channels"][4]
    C = cd_x["C"]
    x = read_fp16_surface(x_path, cd_x).astype(np.float64).reshape(-1)[:C]
    gamma = np.fromfile(gamma_path, dtype=np.float16)[64 // 2 : 64 // 2 + C].astype(np.float64)

    max_abs = np.max(np.abs(x))
    sum_sq = np.sum(x * x)
    mean_sq = sum_sq / C
    eps = 2 ** -17
    rscaled = np.sqrt(mean_sq + eps * max_abs * max_abs)
    y = x * gamma / rscaled
    arr = np.zeros(alloc_for_ch(cd_o) // 2, dtype=np.float16)
    arr[:C] = y.astype(np.float16)
    arr.tofile(out_path)
    return arr, x, gamma, rscaled


def select_reference(island, in_files, out_path):
    """Reference for select(a, b, cond) = cond ? a : b.

    For the runtime form all three tensors are read from ch 5, 6, 7.
    For the constfill form one of a, b, cond is constant in the kernel
    (filled with `island["a_fill_value"]` etc.) and not present in
    `in_files`; we substitute the constant.
    """
    # Resolve a, b, cond by MIL role, not channel id.
    cd_o = island["channels"][4]

    def get_fp16(role):
        for ch, desc in island["channels"].items():
            if ch != 4 and desc.get("role") == role:
                return read_fp16_surface(in_files[ch], desc)
        fill = island.get(f"{role}_fill_value", 0)
        # Shape: match the output shape (same N, C, H, W as output).
        return np.full((cd_o["N"], cd_o["C"], cd_o["H"], cd_o["W"]), fill,
                       dtype=np.float16)

    def get_bool(role):
        for ch, desc in island["channels"].items():
            if ch != 4 and desc.get("role") == role:
                return read_bool_surface(in_files[ch], desc)
        fill = island.get(f"{role}_fill_value", 1)
        return np.full((cd_o["N"], cd_o["C"], cd_o["H"], cd_o["W"]), fill,
                       dtype=np.uint8)

    a = get_fp16("a")
    b = get_fp16("b")
    cond = get_bool("cond")

    out = np.where(cond.astype(bool), a, b)
    arr = np.zeros(alloc_for_ch(cd_o) // 2, dtype=np.float16)
    row_bytes_o = cd_o["row_bytes"]
    N, C, H, W = out.shape
    for n in range(N):
        for c in range(C):
            for h in range(H):
                base = ((n * C + c) * H + h) * (row_bytes_o // 2)
                arr[base : base + W] = out[n, c, h]
    arr.tofile(out_path)
    return arr


# --------------------------------------------------------------------------
# Compare
# --------------------------------------------------------------------------

def fp16_ulp(v):
    a = abs(v)
    if a == 0:
        return 2 ** -24
    e = int(np.floor(np.log2(a)))
    if e < -13:
        return 2 ** -24
    return 2 ** (e - 11)


def compare_fp16(dev_path, ref_path, valid_lanes=None):
    """Return (max_ulp, max_nerr, exact_count, total, in_band_count)."""
    dev = np.fromfile(dev_path, dtype=np.float16).astype(np.float64)
    ref = np.fromfile(ref_path, dtype=np.float16).astype(np.float64)
    n = len(dev)
    if valid_lanes is None:
        valid = np.ones(n, dtype=bool)
    else:
        valid = valid_lanes
    diff = np.abs(dev - ref)
    ulps = np.where(valid, diff / np.maximum(2 ** -24, 2 ** (np.floor(np.log2(np.abs(ref) + 1e-30)) - 11)), 0)
    nerr = np.where(valid & ((np.abs(dev) + np.abs(ref)) > 0),
                    diff / (2 ** -11 * (np.abs(dev) + np.abs(ref))),
                    0)
    exact = int(((diff == 0) & valid).sum())
    in_band = int(((ulps <= 2.0) | (nerr <= 4.0)).sum() & valid.sum())
    return (float(ulps.max()), float(nerr.max()), exact, int(valid.sum()), in_band)


def compare_select(dev_path, ref_path, valid_lanes=None):
    dev = np.fromfile(dev_path, dtype=np.uint16)
    ref = np.fromfile(ref_path, dtype=np.uint16)
    if valid_lanes is None:
        valid = np.ones(len(dev), dtype=bool)
    else:
        valid = valid_lanes
    eq = ((dev == ref) & valid).sum()
    return int(eq), int(valid.sum()), int((~valid).sum())


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------

def resolve_anec(island_name, arg_anec):
    if arg_anec:
        return arg_anec
    candidates = [
        f"{ANEC_FIXTURE_ROOT}/{island_name}.anec",
        f"/var/tmp/inst/fixtures/h14-anec/{island_name}/program-0.anec",
    ]
    for c in candidates:
        if Path(c).exists():
            return c
    fail(f"no ANEC found for {island_name}; tried {candidates}")


def in_arg_order(island):
    """Map runtime input channel -> the --in index ane-run accepts.

    Channels are assigned in ascending channel-id order; the first
    runtime input goes to slot 0, the second to slot 1, etc. Output
    (ch 4) is never returned.
    """
    args = []
    slot = 0
    for ch in sorted(island["channels"].keys()):
        if ch == 4:
            continue
        args.append((slot, ch))
        slot += 1
    return args


def run_device(anec, in_args, out_path):
    cmd = [ANE_RUN, "--anec", anec]
    for slot, path in in_args:
        cmd += ["--in", f"{slot}={path}"]
    cmd += ["--out", f"0={out_path}"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        fail(f"ane-run failed (rc={r.returncode}): {r.stderr}")
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--island", required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--anec", default=None)
    ap.add_argument("--in-dir", default="/var/tmp/islands-run/in")
    ap.add_argument("--out-dir", default="/var/tmp/islands-run/out")
    ap.add_argument("--gamma", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    island = ISLANDS.get(args.island)
    if not island:
        fail(f"unknown island: {args.island}; known: {sorted(ISLANDS.keys())}")

    anec = resolve_anec(args.island, args.anec)
    check_alloc_matches(anec, island)

    os.makedirs(args.in_dir, exist_ok=True)
    os.makedirs(args.out_dir, exist_ok=True)

    base_seed = args.seed * 1000
    in_files = {}
    ane_args = []
    for slot, ch in in_arg_order(island):
        desc = island["channels"][ch]
        if desc["dtype"] == "fp16":
            p = os.path.join(args.in_dir, f"in-{args.island}-ch{ch}-s{args.seed}.fp16")
            write_fp16_surface(p, desc, seed=base_seed + ch)
        elif desc["dtype"] == "bool":
            p = os.path.join(args.in_dir, f"in-{args.island}-ch{ch}-s{args.seed}.bin")
            write_bool_surface(p, desc, seed=base_seed + ch)
        else:
            fail(f"unsupported dtype {desc['dtype']} for ch{ch}")
        in_files[ch] = p
        ane_args.append((slot, p))

    out_dev = os.path.join(args.out_dir, f"out-{args.island}-s{args.seed}.fp16")
    run_device(anec, ane_args, out_dev)

    if island["kind"] == "bmm":
        out_ref = os.path.join(args.out_dir, f"ref-{args.island}-s{args.seed}.fp16")
        try:
            ref_arr, ref_full = bmm_reference(island, in_files[5], in_files[6], out_ref)
        except RuntimeError as e:
            print(f"{args.island} s{args.seed} ref-build-error: {e}")
            return
        # valid lanes in the output: each (n, c, m) row first W elements.
        cd = island["channels"][4]
        N, C, H, W = cd["N"], cd["C"], cd["H"], cd["W"]
        valid = np.zeros(alloc_for_ch(cd) // 2, dtype=bool)
        rb = cd["row_bytes"]
        for n in range(N):
            for c in range(C):
                for h in range(H):
                    base = ((n * C + c) * H + h) * (rb // 2)
                    valid[base : base + W] = True
        max_ulp, max_nerr, exact, total, in_band = compare_fp16(out_dev, out_ref, valid)
        pad_zero = (np.fromfile(out_dev, dtype=np.float16)[~valid] == 0).all()
        verdict = "PASS" if (exact == total or in_band == total) and pad_zero else "FAIL"
        print(f"{args.island} s{args.seed} bmm: max_ulp={max_ulp:.4f} "
              f"max_nerr={max_nerr:.3f} exact={exact}/{total} "
              f"in_band={in_band}/{total} pad_zero={pad_zero} -> {verdict}")
    elif island["kind"] in ("select", "select_constfill"):
        out_ref = os.path.join(args.out_dir, f"ref-{args.island}-s{args.seed}.fp16")
        ref_arr = select_reference(island, in_files, out_ref)
        cd = island["channels"][4]
        N, C, H, W = cd["N"], cd["C"], cd["H"], cd["W"]
        valid = np.zeros(alloc_for_ch(cd) // 2, dtype=bool)
        rb = cd["row_bytes"]
        for n in range(N):
            for c in range(C):
                for h in range(H):
                    base = ((n * C + c) * H + h) * (rb // 2)
                    valid[base : base + W] = True
        dev = np.fromfile(out_dev, dtype=np.uint16)
        ref = np.fromfile(out_ref, dtype=np.uint16)
        eq = int(((dev == ref) & valid).sum())
        pad_zero = bool((dev[~valid] == 0).all())
        total = int(valid.sum())
        verdict = "PASS" if eq == total and pad_zero else "FAIL"
        print(f"{args.island} s{args.seed} select: exact={eq}/{total} "
              f"pad_zero={pad_zero} -> {verdict}")
    elif island["kind"] == "rms":
        gamma_path = args.gamma or os.path.join(args.in_dir, f"gamma-{args.island}-s{args.seed}.bin")
        if not Path(gamma_path).exists():
            gen_rms_gamma(gamma_path, island["channels"][5]["C"], base_seed + 99)
        out_ref = os.path.join(args.out_dir, f"ref-{args.island}-s{args.seed}.fp16")
        ref_arr, x, gamma, rscaled = rms_reference(island, in_files[5], gamma_path, out_ref)
        cd = island["channels"][4]
        C = cd["C"]
        valid = np.zeros(alloc_for_ch(cd) // 2, dtype=bool)
        valid[:C] = True
        max_ulp, max_nerr, exact, total, in_band = compare_fp16(out_dev, out_ref, valid)
        pad_zero = (np.fromfile(out_dev, dtype=np.float16)[~valid] == 0).all()
        verdict = "PASS" if (exact == total or in_band == total) and pad_zero else "FAIL"
        print(f"{args.island} s{args.seed} rms: max_ulp={max_ulp:.4f} "
              f"max_nerr={max_nerr:.3f} exact={exact}/{total} "
              f"in_band={in_band}/{total} pad_zero={pad_zero} -> {verdict}")


if __name__ == "__main__":
    main()