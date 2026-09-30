#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Encoder with attention islands on ANE, rest on CPU — four-way comparison.

Runs four variants of the 24-layer Parakeet TDT 0.6B v3 encoder on the pinned
LibriSpeech fixture (1089-134686-0000), then decodes a transcript with the
BNNS-equivalent NumPy decoder + joint and the pinned tokenizer:

  1. baseline        every op on CPU (NumPy MIL reference).
  2. islands-on-m2   A kt, A p1, C pv, B select on the M2 ANE every layer;
                     all other ops on CPU.
  3. cpv-only-on-m2  only C pv on the M2 ANE per layer; rest CPU.
  4. cpv-noise-cpu   CPU C pv with an emulated accumulator-noise quantiser
                     (fp32-RN chain, 2^-16 output grid for |y| < 2^-10,
                     FTZ below 2^-17) — first-order approximation of the
                     measured M2 C pv behaviour, not bit-exact.

Per variant the report carries: final encoder output (rel L2 / max abs vs
baseline and vs the golden capture), the per-layer divergence curve (rel L2
of the layer-normed residual vs baseline), the decoded token ids, and the
transcript with equality / WER against the reference transcript.

This is not "Parakeet on M2". Every op outside the four island sites still
runs on the host CPU: the subsampling convs, all LayerNorms, all feed-forward
linears and GLU gates, the depthwise convolutions, every softmax, every
residual add, the o_proj matmuls, and the whole decoder / joint / tokenizer.

Usage::

    python3 tools/parakeet_encoder_islands.py \
        --mil-source /var/tmp/parakeet-src \
        --capture ~/.cache/mlx-omarchy/parakeet-reference/captures/.../ane \
        --host <M2-SSH-alias> --out /var/tmp/parakeet-encoder-islands/run1

Any device failure, NaN/Inf device lane, or non-finite encoder output aborts
the run and is named in the report.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
from island_ref import ISLANDS, alloc_for_ch, in_arg_order, read_fp16_surface  # noqa: E402

MIL_NUMPY_PATH = "/home/joshuawarren/src/mlx-omarchy/overlay/tools/coreml/mil_numpy.py"

N_LAYERS = 24
BLANK_ID = 8192
VOCAB_SIZE = 8193
HIDDEN = 640

# MIL output-tensor name per (island, layer). Verified against
# /var/tmp/parakeet-src/model.mil (24 occurrences of each family).
LAYER_OUTPUT = {
    "a-kt": lambda L: f"attention_scores_{6 * L + 1}_cast_fp16",
    "a-p1": lambda L: f"matmul_{L}_cast_fp16",
    "c-pv": lambda L: f"attn_output_{6 * L + 1}_cast_fp16",
    "b-select": lambda L: f"attention_mask_{2 * L + 9}_cast_fp16",
}
OUTPUT_TO_ISLAND = {
    LAYER_OUTPUT[island](L): (island, L)
    for island in LAYER_OUTPUT
    for L in range(N_LAYERS)
}
# Layer 23's select drops the numeric suffix in the emitted MIL
# (attention_mask_cast_fp16, matrix_bd_cast_fp16).
OUTPUT_TO_ISLAND["attention_mask_cast_fp16"] = ("b-select", 23)

# Device channel layout per island (tools/island_ref.py ISLANDS keys), with
# the proven channel -> role binding from receipts 2026-09-30-t6021-island-*.
ISLAND_DEVICE = {
    "a-kt": "island-a-kt",
    "a-p1": "island-a-attn-p1",
    "c-pv": "island-c-pv",
    "b-select": "island-b-select-runtime",
}
# island -> {ch: MIL kwarg}. ch5 carries the second MIL input (w / b), ch6
# the first (x / a), ch7 cond (select only); ch4 is the output.
ISLAND_CHANNELS = {
    "a-kt": {6: "x", 5: "y"},
    "a-p1": {6: "x", 5: "y"},
    "c-pv": {6: "x", 5: "y"},
    "b-select": {6: "a", 5: "b", 7: "cond"},
}


def load_mil_module():
    spec = importlib.util.spec_from_file_location("mil_numpy_e2e", MIL_NUMPY_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# ---------------------------------------------------------------------------
# Device dispatch.
# ---------------------------------------------------------------------------

class AneDispatcher:
    """Pack island operands, run ane-run on the M2, unpack the fp16 surface."""

    def __init__(self, host: str, local_scratch: Path, anec_root: str):
        self.host = host
        self.local = local_scratch
        self.remote_root = "/var/tmp/islands-run/e2e"
        self.anec_root = anec_root
        self.local.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["ssh", host, f"mkdir -p {shlex.quote(self.remote_root)}"],
            check=True, capture_output=True,
        )
        self.calls: list[dict] = []

    def run(self, layer: int, island: str, operands: dict[int, np.ndarray]) -> np.ndarray:
        cfg = ISLANDS[ISLAND_DEVICE[island]]
        out_desc = cfg["channels"][4]
        run_key = f"L{layer:02d}-{island}"
        remote_dir = f"{self.remote_root}/{run_key}"
        subprocess.run(
            ["ssh", self.host, f"mkdir -p {shlex.quote(remote_dir)}"],
            check=True, capture_output=True,
        )

        remote_in: dict[int, str] = {}
        for ch, arr in operands.items():
            desc = cfg["channels"][ch]
            want = (desc["N"], desc["C"], desc["H"], desc["W"])
            arr = np.asarray(arr)
            if arr.ndim == 0:
                # MIL scalar constant (e.g. the select's -inf a branch):
                # broadcast to the full surface.
                arr = np.full(want, arr, dtype=arr.dtype)
            elif arr.shape != want:
                arr = np.broadcast_to(arr, want)
            local = self.local / f"{run_key}-ch{ch}.bin"
            if desc["dtype"] == "fp16":
                self._pack_fp16(local, arr, desc)
            else:
                self._pack_bool(local, arr, desc)
            remote = f"{remote_dir}/ch{ch}.bin"
            subprocess.run(
                ["scp", str(local), f"{self.host}:{remote}"],
                check=True, capture_output=True,
            )
            remote_in[ch] = remote

        cmd = ["flock", "/var/tmp/ane-run.lock", "timeout", "60",
               "/var/tmp/inst/tools/ane-run", "--anec",
               f"{self.anec_root}/{ISLAND_DEVICE[island]}/program-0.anec"]
        for slot, ch in in_arg_order(cfg):
            cmd += ["--in", f"{slot}={remote_in[ch]}"]
        remote_out = f"{remote_dir}/out.bin"
        cmd += ["--out", f"0={remote_out}", "--time"]
        result = subprocess.run(
            ["ssh", self.host, " ".join(shlex.quote(p) for p in cmd)],
            check=True, text=True, capture_output=True, timeout=75,
        )
        local_out = self.local / f"{run_key}-out.bin"
        subprocess.run(
            ["scp", f"{self.host}:{remote_out}", str(local_out)],
            check=True, capture_output=True,
        )
        want = alloc_for_ch(out_desc)
        if local_out.stat().st_size != want:
            raise RuntimeError(f"{run_key}: output size {local_out.stat().st_size} != {want}")
        out = read_fp16_surface(local_out, out_desc)
        if not np.isfinite(out.astype(np.float32)).all():
            raise RuntimeError(f"{run_key}: non-finite device lanes")
        self.calls.append({
            "layer": layer, "island": island,
            "exec_ms": self._exec_ms(result.stdout),
        })
        return out

    @staticmethod
    def _pack_fp16(path: Path, arr: np.ndarray, desc: dict) -> None:
        surface = np.zeros(alloc_for_ch(desc) // 2, dtype=np.float16)
        row = desc["row_bytes"] // 2
        n, c, h, w = arr.shape
        flat = np.ascontiguousarray(arr, dtype=np.float16).reshape(n, c, h, w)
        for ni in range(n):
            for ci in range(c):
                for hi in range(h):
                    base = ((ni * c + ci) * h + hi) * row
                    surface[base:base + w] = flat[ni, ci, hi]
        surface.tofile(path)

    @staticmethod
    def _pack_bool(path: Path, arr: np.ndarray, desc: dict) -> None:
        surface = np.zeros(alloc_for_ch(desc), dtype=np.uint8)
        row = desc["row_bytes"]
        n, c, h, w = arr.shape
        flat = np.ascontiguousarray(arr, dtype=np.uint8).reshape(n, c, h, w)
        for ni in range(n):
            for ci in range(c):
                for hi in range(h):
                    base = ((ni * c + ci) * h + hi) * row
                    surface[base:base + w] = flat[ni, ci, hi]
        surface.tofile(path)

    @staticmethod
    def _exec_ms(stdout: str) -> float | None:
        for line in stdout.splitlines():
            # ane-run --time prints:
            #   exec ms over N calls: min A p10 B ... median ... max Z
            if line.startswith("exec ms over"):
                parts = line.split()
                if "min" in parts:
                    try:
                        return float(parts[parts.index("min") + 1])
                    except (ValueError, IndexError):
                        return None
        return None


# ---------------------------------------------------------------------------
# CPU island references and the emulated C pv noise.
# ---------------------------------------------------------------------------

def cpu_matmul(x: np.ndarray, y: np.ndarray, transpose_x: bool, transpose_y: bool) -> np.ndarray:
    a = np.asarray(x).astype(np.float32)
    b = np.asarray(y).astype(np.float32)
    if transpose_x:
        a = np.swapaxes(a, -1, -2)
    if transpose_y:
        b = np.swapaxes(b, -1, -2)
    return a @ b


def cpu_c_pv_noisy(x: np.ndarray, y: np.ndarray, transpose_x: bool, transpose_y: bool) -> np.ndarray:
    """First-order emulation of the measured M2 C pv accumulator behaviour.

    AccumProbe (receipts/2026-09-30-t6021-accumulator) measured: fp32-class
    accumulation, |y| < 2^-17 forced to zero, 2^-17 <= |y| < 2^-10 quantised
    onto a 2^-16 grid, |y| >= 2^-10 plain fp16 rounding. Applied here on the
    fp32 dot product. This is NOT bit-exact: the real accumulator has chunked
    internal rounding no tested model reproduces.
    """
    full = cpu_matmul(x, y, transpose_x, transpose_y)
    mag = np.abs(full)
    out = np.zeros_like(full)
    small = (mag >= 2.0 ** -17) & (mag < 2.0 ** -10)
    out[small] = np.round(full[small] / 2.0 ** -16) * 2.0 ** -16
    big = mag >= 2.0 ** -10
    out[big] = full[big].astype(np.float16).astype(np.float32)
    return out


# ---------------------------------------------------------------------------
# Island-routed MIL program.
# ---------------------------------------------------------------------------

class IslandProgram:
    """mil_numpy.Program with the four island sites routed to a callback.

    Also snapshots the residual stream after every encoder layer: the output
    of the layer_norm whose gamma constant is `encoder_layers_{L}_norm_out_*`.
    """

    def __init__(self, mil_source: Path, router):
        mil = load_mil_module()
        self._mil = mil
        self.program = mil.Program(mil_source / "model.mil", mil_source / "model-root")
        self.router = router
        self.counts = {k: 0 for k in LAYER_OUTPUT}
        self.layer_outputs: dict[int, np.ndarray] = {}
        self._original_apply = self.program.apply
        self.program.apply = self._apply

    def _apply(self, stmt):
        out_name = stmt.names[0] if stmt.names else ""
        hit = OUTPUT_TO_ISLAND.get(out_name)
        if hit is not None:
            island, layer = hit
            operands = {}
            for ch, kw in ISLAND_CHANNELS[island].items():
                value = self.program.arg(stmt, kw)
                if value is None:
                    raise RuntimeError(f"{out_name}: missing operand {kw}")
                operands[ch] = np.asarray(value)
            if island != "b-select":
                # Materialize the MIL matmul's transposes so every router
                # sees plain x[B,M,K] @ y[B,K,N] semantics (A p1 declares
                # transpose_y = true).
                if self.program.scalar(stmt, "transpose_x", False):
                    operands[6] = np.swapaxes(operands[6], -1, -2)
                if self.program.scalar(stmt, "transpose_y", False):
                    operands[5] = np.swapaxes(operands[5], -1, -2)
            self.counts[island] += 1
            routed = self.router(layer, island, operands)
            return np.asarray(routed, dtype=self._mil.DTYPES[stmt.dtypes[0]])
        # Per-layer residual snapshot: the layer_norm whose gamma is the
        # layer's norm_out weight.
        if stmt.op == "layer_norm":
            gamma = stmt.kwargs.get("gamma", "")
            for L in range(N_LAYERS):
                if gamma.startswith(f"encoder_layers_{L}_norm_out_weight"):
                    result = self._original_apply(stmt)
                    self.layer_outputs[L] = np.asarray(result, dtype=np.float16)
                    return result
        return self._original_apply(stmt)

    def run(self, inputs: dict) -> dict[str, np.ndarray]:
        return self.program.run(
            inputs=inputs, wanted={"encoder_hidden"}, stop_after="encoder_mask"
        )


def make_router(variant: str, dispatcher: AneDispatcher | None):
    if variant == "baseline":
        def router(layer, island, operands):
            if island == "b-select":
                return np.where(
                    operands[7].astype(bool),
                    operands[6].astype(np.float16),
                    operands[5].astype(np.float16),
                )
            return cpu_matmul(operands[6], operands[5], False, False)
        return router
    if variant == "islands-on-m2":
        return lambda layer, island, operands: dispatcher.run(layer, island, operands)
    if variant == "cpv-only-on-m2":
        def router(layer, island, operands):
            if island == "c-pv":
                return dispatcher.run(layer, island, operands)
            if island == "b-select":
                return np.where(
                    operands[7].astype(bool),
                    operands[6].astype(np.float16),
                    operands[5].astype(np.float16),
                )
            return cpu_matmul(operands[6], operands[5], False, False)
        return router
    if variant == "cpv-noise-cpu":
        def router(layer, island, operands):
            if island == "c-pv":
                return cpu_c_pv_noisy(operands[6], operands[5], False, False)
            if island == "b-select":
                return np.where(
                    operands[7].astype(bool),
                    operands[6].astype(np.float16),
                    operands[5].astype(np.float16),
                )
            return cpu_matmul(operands[6], operands[5], False, False)
        return router
    raise ValueError(f"unknown variant {variant}")


# ---------------------------------------------------------------------------
# Decoder / joint / tokenizer (numpy mirrors of the pinned contracts).
# ---------------------------------------------------------------------------

class GreedyTdt:
    def __init__(self, decoder_pkg: Path, joint_pkg: Path, tokenizer_json: Path, lut_path: Path):
        sys.path.insert(0, "/home/joshuawarren/src/mlx-omarchy/overlay/tools")
        from coreml.pinned_component import load_pinned_component

        decoder = load_pinned_component(decoder_pkg, "decoder")
        joint = load_pinned_component(joint_pkg, "joint")
        self.embedding = decoder.constant("embedding_weight_to_fp16")
        self.layers = []
        for ih, hh, bias in (
            ("concat_1_to_fp16", "concat_2_to_fp16", "concat_0_to_fp16"),
            ("concat_4_to_fp16", "concat_5_to_fp16", "concat_3_to_fp16"),
        ):
            weight_t64 = np.ascontiguousarray(
                np.concatenate(
                    [decoder.constant(ih), decoder.constant(hh)], axis=1
                ).T,
                dtype=np.float64,
            )
            self.layers.append((weight_t64, decoder.constant(bias).astype(np.float16).ravel()))
        self.projector = decoder.constant("projector_weight_to_fp16")
        self.projector_bias = decoder.constant("projector_bias_to_fp16")
        self.head_w = joint.constant("head_weight_to_fp16")
        self.head_b = joint.constant("head_bias_to_fp16")
        if lut_path.exists():
            with np.load(lut_path) as z:
                self.sigma_lut = z["sigma_lut"]
                self.tanh_lut = z["tanh_lut"]
            self.luts = True
        else:
            self.luts = False

    def _gemv(self, a: np.ndarray, weight_t64: np.ndarray) -> np.ndarray:
        """BNNS fp16 GEMV: fp16 FMA chains over k-blocks of 128."""
        k_len, n_len = weight_t64.shape
        block = 128
        a64 = a.astype(np.float64)
        out = None
        for kb in range(0, k_len, block):
            acc = np.zeros(n_len, dtype=np.float16)
            for k in range(kb, min(kb + block, k_len)):
                acc = (acc.astype(np.float64) + a64[k] * weight_t64[k]).astype(np.float16)
            if out is None:
                out = acc
            else:
                out = (out.astype(np.float64) + acc.astype(np.float64)).astype(np.float16)
        return out

    def _lstm(self, x: np.ndarray, hidden: np.ndarray, cell: np.ndarray,
              weight_t64: np.ndarray, bias: np.ndarray):
        lanes = bias.size // 4
        a = np.concatenate([x.astype(np.float16).ravel(),
                            hidden.astype(np.float16).ravel()]).astype(np.float16)
        preact = (self._gemv(a, weight_t64).astype(np.float64)
                  + bias.astype(np.float64)).astype(np.float16)
        if self.luts:
            sig = self.sigma_lut
            tan = self.tanh_lut
            g_i = sig[preact[:lanes].view(np.uint16)]
            g_f = sig[preact[lanes:2 * lanes].view(np.uint16)]
            g_o = sig[preact[2 * lanes:3 * lanes].view(np.uint16)]
            g_g = tan[preact[3 * lanes:].view(np.uint16)]
        else:
            def sigmoid16(v):
                v64 = v.astype(np.float64)
                return (1.0 / (1.0 + np.exp(-v64))).astype(np.float16)
            def tanh16(v):
                return np.tanh(v.astype(np.float64)).astype(np.float16)
            g_i = sigmoid16(preact[:lanes])
            g_f = sigmoid16(preact[lanes:2 * lanes])
            g_o = sigmoid16(preact[2 * lanes:3 * lanes])
            g_g = tanh16(preact[3 * lanes:])
        c0 = cell.astype(np.float16).ravel()
        next_cell = (g_f.astype(np.float64) * c0.astype(np.float64)
                     + g_i.astype(np.float64) * g_g.astype(np.float64)).astype(np.float16)
        tanh_cell = (self.tanh_lut[next_cell.view(np.uint16)] if self.luts
                     else np.tanh(next_cell.astype(np.float64)).astype(np.float16))
        next_hidden = (g_o.astype(np.float64) * tanh_cell.astype(np.float64)).astype(np.float16)
        return next_hidden, next_cell

    def decode_step(self, token_id: int, hidden: np.ndarray, cell: np.ndarray):
        idx = token_id + VOCAB_SIZE if token_id < 0 else token_id
        x = self.embedding[idx].astype(np.float16).ravel().copy()
        states = []
        x_in = x
        for i, (weight_t64, bias) in enumerate(self.layers):
            nh, nc = self._lstm(x_in, hidden[i], cell[i], weight_t64, bias)
            states.append((nh, nc))
            x_in = nh
        decoder_hidden = (
            states[1][0].astype(np.float32) @ self.projector.astype(np.float32).T
            + self.projector_bias.astype(np.float32)
        ).astype(np.float32)
        next_hidden = np.stack([s[0].reshape(1, HIDDEN) for s in states]).astype(np.float32)
        next_cell = np.stack([s[1].reshape(1, HIDDEN) for s in states]).astype(np.float32)
        return decoder_hidden.reshape(1, HIDDEN), next_hidden, next_cell

    def joint(self, encoder_frame: np.ndarray, decoder_state: np.ndarray):
        h = np.maximum(
            encoder_frame.astype(np.float16) + decoder_state.astype(np.float16),
            np.float16(0),
        ).astype(np.float32)
        full = h @ self.head_w.astype(np.float32).T + self.head_b.astype(np.float32)
        return full[:, :VOCAB_SIZE], full[:, VOCAB_SIZE:]

    def decode(self, encoder_hidden: np.ndarray, n_frames: int) -> list[int]:
        """The canonical greedy TDT state machine (mlx-omarchy tdt_control.decode_tdt):
        durations (0,1,2,3,4), max_symbols_per_step 10, blank 8192. The decoder
        state is reused while the input token stays blank."""
        durations = (0, 1, 2, 3, 4)
        max_symbols = 10
        hidden = np.zeros((2, 1, HIDDEN), dtype=np.float32)
        cell = np.zeros((2, 1, HIDDEN), dtype=np.float32)
        input_token = BLANK_ID
        state_valid = False
        tokens: list[int] = []
        frame = 0
        while frame < n_frames:
            symbols = 0
            advanced = False
            while symbols < max_symbols:
                if not state_valid or input_token != BLANK_ID:
                    decoder_state, hidden, cell = self.decode_step(
                        input_token, hidden, cell
                    )
                    state_valid = True
                frame_vec = encoder_hidden[0, frame].astype(np.float32).reshape(1, HIDDEN)
                token_logits, duration_logits = self.joint(frame_vec, decoder_state)
                token_id = int(np.argmax(token_logits.reshape(-1)))
                duration = durations[int(np.argmax(duration_logits.reshape(-1)))]
                if token_id == BLANK_ID:
                    frame += max(duration, 1)
                    advanced = True
                    break
                tokens.append(token_id)
                input_token = token_id
                symbols += 1
                if duration > 0:
                    frame += duration
                    advanced = True
                    break
            if not advanced:
                frame += 1
        return tokens


def load_tokenizer(tokenizer_json: Path):
    """Load the pinned ParakeetTokenizer (HF BPE + Metaspace detokenizer)."""
    sys.path.insert(0, "/home/joshuawarren/src/mlx-omarchy/overlay/tools")
    from coreml.tokenizer import _parse_tokenizer

    root = json.loads(tokenizer_json.read_text())
    pieces, special_ids = _parse_tokenizer(root)
    return pieces, special_ids


def detokenize(token_ids: list[int], tokenizer) -> str:
    pieces, special_ids = tokenizer
    parts = []
    for tid in token_ids:
        if 0 <= tid < len(pieces) and tid not in special_ids:
            parts.append(pieces[tid].replace("▁", " "))
    text = "".join(parts)
    return text[1:] if text.startswith(" ") else text


def wer(reference: str, hypothesis: str) -> float:
    ref = reference.split()
    hyp = hypothesis.split()
    if not ref:
        return 0.0 if not hyp else 1.0
    d = np.zeros((len(ref) + 1, len(hyp) + 1), dtype=np.int32)
    d[:, 0] = np.arange(len(ref) + 1)
    d[0, :] = np.arange(len(hyp) + 1)
    for i in range(1, len(ref) + 1):
        for j in range(1, len(hyp) + 1):
            cost = 0 if ref[i - 1] == hyp[j - 1] else 1
            d[i, j] = min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost)
    return float(d[-1][-1]) / len(ref)


# ---------------------------------------------------------------------------
# Metrics + driver.
# ---------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def diff(a: np.ndarray, b: np.ndarray) -> dict:
    a = np.asarray(a, dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    delta = a - b
    den = float(np.linalg.norm(b))
    return {
        "max_abs": float(np.max(np.abs(delta))),
        "mean_abs": float(np.mean(np.abs(delta))),
        "rel_l2": float(np.linalg.norm(delta) / den) if den else 0.0,
        "nan": int(np.isnan(a).sum() + np.isnan(b).sum()),
        "inf": int(np.isinf(a).sum() + np.isinf(b).sum()),
        "bit_exact": bool(np.array_equal(a, b)),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mil-source", type=Path, default=Path("/var/tmp/parakeet-src"))
    ap.add_argument("--capture", type=Path, default=Path(
        "/home/joshuawarren/.cache/mlx-omarchy/parakeet-reference/captures/"
        "b650695c-75aec2a/20260912T154759Z-librispeech/ane"))
    ap.add_argument("--model-dir", type=Path, default=Path(
        "/home/joshuawarren/.cache/mlx-omarchy/parakeet-reference/mweinbach1/"
        "parakeet-tdt-0.6b-v3-coreml/b650695c2322ee5281dff48d7345b2f3a58ff018"))
    ap.add_argument("--host", required=True)
    ap.add_argument("--anec-root", default="/var/tmp/inst/fixtures/h14-anec")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--scratch", type=Path, default=Path("/tmp/parakeet-e2e"))
    ap.add_argument("--lut", type=Path, default=Path(
        "/home/joshuawarren/src/mlx-omarchy/overlay/tools/coreml/fused_lut_2026-09-15.npz"))
    ap.add_argument("--variants", nargs="+",
                    default=["baseline", "islands-on-m2", "cpv-only-on-m2", "cpv-noise-cpu"])
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    args.scratch.mkdir(parents=True, exist_ok=True)

    # M2 liveness + ANE fixture probe (one round trip).
    probe = subprocess.run(
        ["ssh", args.host,
         "uptime; ls /var/tmp/inst/tools/ane-run; "
         "ls /var/tmp/inst/fixtures/h14-anec | tr '\\n' ' '"],
        check=True, text=True, capture_output=True, timeout=20,
    )
    (args.out / "m2_probe.txt").write_text(probe.stdout)
    print(probe.stdout, flush=True)

    capture = args.capture
    features = np.load(capture / "encoder_input_features.npy").reshape(1, 3000, 128)
    mask_input = np.load(capture / "encoder_input_mask.npy").reshape(1, 3000)
    golden_hidden = np.load(capture / "encoder_hidden.npy").astype(np.float32)
    golden_trace = json.loads((capture / "token_ids.json").read_text())
    golden_tokens = list(golden_trace["token_ids"])
    golden_transcript = (capture / "transcript.txt").read_text().strip()
    encoder_mask = np.load(capture / "encoder_mask.npy")
    n_frames = int(np.asarray(encoder_mask).sum())

    tdt = GreedyTdt(args.model_dir / "decoder.mlpackage",
                    args.model_dir / "joint.mlpackage", None, args.lut)
    spm = load_tokenizer(args.model_dir / "tokenizer.json")

    # Control: decode the golden encoder_hidden with the NumPy decoder. If
    # this reproduces the golden tokens exactly, every transcript difference
    # below is attributable to the encoder variants, not the decoder.
    golden_replay = tdt.decode(golden_hidden, n_frames)

    runs: dict[str, dict] = {}
    for variant in args.variants:
        dispatcher = AneDispatcher(args.host, args.scratch, args.anec_root) \
            if variant.endswith("on-m2") else None
        program = IslandProgram(args.mil_source, make_router(variant, dispatcher))
        started = time.monotonic()
        keep = program.run({"input_features": features, "attention_mask": mask_input})
        elapsed = time.monotonic() - started
        hidden = keep["encoder_hidden"].astype(np.float32)
        if not np.isfinite(hidden).all():
            raise SystemExit(f"FAIL: {variant} produced non-finite encoder output")

        tokens = tdt.decode(hidden, n_frames)
        transcript = detokenize(tokens, spm)
        per_layer = [
            {"layer": L, **diff(program.layer_outputs[L], runs["baseline"]["layers"][L])}
            for L in sorted(program.layer_outputs)
            if "baseline" in runs and L in runs["baseline"]["layers"]
        ]
        runs[variant] = {
            "elapsed_s": round(elapsed, 3),
            "hidden": hidden,
            "layers": dict(program.layer_outputs),
            "counts": dict(program.counts),
            "ane_calls": list(dispatcher.calls) if dispatcher else [],
            "tokens": tokens,
            "transcript": transcript,
            "per_layer": per_layer,
        }
        print(f"[{variant}] {elapsed:.1f}s  islands={program.counts}  "
              f"tokens={len(tokens)}", flush=True)

    baseline = runs["baseline"]

    # Reference decoder tokens for cross-check (the golden transcript should
    # round-trip through the tokenizer).
    report = {
        "schema": "omarchy-ane.parakeet-encoder-islands.v1",
        "date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": args.host,
        "what_this_is": ("encoder with attention islands on ANE, rest on CPU "
                         "(subsampling convs, LayerNorms, feed-forward linears, GLU gates, "
                         "depthwise convolutions, softmax, residual adds, o_proj, decoder, "
                         "joint, tokenizer all on CPU)"),
        "model": {"repo": "mweinbach1/parakeet-tdt-0.6b-v3-coreml",
                  "revision": "b650695c2322ee5281dff48d7345b2f3a58ff018",
                  "mil_sha256": sha256_file(args.mil_source / "model.mil")},
        "fixture": {"name": "LibriSpeech test-clean 1089-134686-0000",
                    "frames": n_frames,
                    "encoder_hidden_sha256": sha256_file(capture / "encoder_hidden.npy")},
        "variants": {},
        "golden": {"tokens": len(golden_tokens), "transcript": golden_transcript},
        "golden_replay": {
            "tokens": golden_replay,
            "token_count": len(golden_replay),
            "tokens_equal_golden": golden_replay == golden_tokens,
            "transcript": detokenize(golden_replay, spm),
        },
    }
    for variant, run in runs.items():
        entry = {
            "elapsed_s": run["elapsed_s"],
            "island_counts": run["counts"],
            "ane_calls": run["ane_calls"],
            "hidden_vs_baseline": diff(run["hidden"], baseline["hidden"]) if variant != "baseline" else None,
            "hidden_vs_golden": diff(run["hidden"], golden_hidden),
            "tokens": run["tokens"],
            "token_count": len(run["tokens"]),
            "tokens_equal_golden": run["tokens"] == golden_tokens,
            "transcript": run["transcript"],
            "transcript_equal_golden": run["transcript"] == golden_transcript,
            "wer_vs_golden_transcript": wer(golden_transcript, run["transcript"]),
            "per_layer_vs_baseline": run["per_layer"],
        }
        report["variants"][variant] = entry
        np.save(args.out / f"hidden_{variant}.npy", run["hidden"])

    (args.out / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")

    # Flat plain-text report.
    lines = []
    lines.append("parakeet-encoder-islands — encoder with attention islands on ANE, rest on CPU")
    lines.append(f"date_utc={report['date_utc']}  host={args.host}")
    lines.append(f"model=mweinbach1/parakeet-tdt-0.6b-v3-coreml@b650695c  "
                 f"mil_sha256={report['model']['mil_sha256'][:16]}...")
    lines.append(f"fixture=LibriSpeech test-clean 1089-134686-0000  frames={n_frames}")
    lines.append("")
    lines.append("Final encoder output (1,375,640) fp32:")
    for variant, run in runs.items():
        vsb = report["variants"][variant]["hidden_vs_baseline"]
        vsg = report["variants"][variant]["hidden_vs_golden"]
        vsb_text = (f"rel_l2={vsb['rel_l2']:.6e} max_abs={vsb['max_abs']:.6e}"
                    if vsb else "baseline")
        lines.append(f"  {variant:16s} vs baseline: {vsb_text}")
        lines.append(f"  {'':16s} vs golden  : rel_l2={vsg['rel_l2']:.6e} "
                     f"max_abs={vsg['max_abs']:.6e}  bit_exact={vsg['bit_exact']}")
    lines.append("")
    lines.append("Per-layer divergence vs baseline (rel_l2 of the layer-normed residual):")
    header = "  layer  " + "".join(f"{v:>18s}" for v in args.variants)
    lines.append(header)
    for L in range(N_LAYERS):
        cells = [f"  {L:5d}  "]
        for v in args.variants:
            pl = next((p for p in runs[v]["per_layer"] if p["layer"] == L), None)
            if pl is None:
                cells.append(f"{'base':>18s}")
            else:
                cells.append(f"{pl['rel_l2']:>18.6e}")
        lines.append("".join(cells))
    lines.append("")
    lines.append("Transcripts:")
    lines.append(f"  golden       ({len(golden_tokens)} tokens): {golden_transcript}")
    replay = report["golden_replay"]
    lines.append(f"  golden-replay ({replay['token_count']} tokens, "
                 f"numpy decoder on golden encoder_hidden, equal={replay['tokens_equal_golden']}): "
                 f"{replay['transcript']}")
    for variant, run in runs.items():
        mark = "MATCH" if run["tokens"] == golden_tokens else "DIFFER"
        lines.append(f"  {variant:16s} ({len(run['tokens'])} tokens, {mark}): {run['transcript']}")
    lines.append("")
    lines.append("WER vs golden transcript:")
    for variant, run in runs.items():
        lines.append(f"  {variant:16s} wer={wer(golden_transcript, run['transcript']):.4f}  "
                     f"tokens_equal={run['tokens'] == golden_tokens}")
    text = "\n".join(lines) + "\n"
    (args.out / "report.txt").write_text(text)
    print(text)


if __name__ == "__main__":
    main()
