#!/usr/bin/env python3
"""Host checks for tools/qwen_precision.py: the T6021 output-grid model
against the accumulator probe facts, and the pre-registered gate verdicts on
synthetic runs. No device."""

import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from qwen_precision import gate, poisson_binomial, t6021_grid


def test_t6021_grid_matches_the_probe_3_rules():
    # receipts/2026-09-30-t6021-accumulator, probe 3: |v| < 2^-17 -> 0; below 2^-6 the
    # nearest multiple of 2^-16, ties away from zero; from 2^-6 up the fp16 value.
    v = np.array([7.569790e-06, 7.688999e-06, 1.5 * 2 ** -16, -2.5 * 2 ** -16, 2 ** -6 + 2 ** -16, 0.1])
    got = t6021_grid(v)
    assert got[0] == 0.0 and got[1] == 2 ** -16
    assert got[2] == 2 * 2 ** -16 and got[3] == -3 * 2 ** -16
    assert got[4] == np.float16(v[4]) and got[5] == np.float16(0.1)


def test_poisson_binomial_matches_the_binomial_for_equal_p():
    from math import comb
    got = poisson_binomial([0.3] * 10)
    assert np.allclose(got, [comb(10, k) * 0.3 ** k * 0.7 ** (10 - k) for k in range(11)])


def write_run(tmp, name, prompts, gens, logits):
    js = {"prompts": [{"id": p, "prompt_token_ids": [1, 2], "runs": [{"generated_ids": g}]}
                      for p, g in zip(prompts, gens)]}
    (tmp / f"{name}.json").write_text(json.dumps(js))
    np.savez(tmp / f"{name}.npz", **{f"prompt_{n:03d}": lg for n, lg in enumerate(logits)})
    return tmp / f"{name}.json", tmp / f"{name}.npz"


def greedy(rng, n=4, vocab=6, flip_at=None):
    """Logits with top1-top2 margin 1.0, ids 0/1; flip_at swaps them at one position by a 0.1 margin."""
    lg = rng.normal(0, 0.1, (n, vocab)).astype(np.float32)
    lg[:, 0], lg[:, 1] = 10.0, 9.0
    if flip_at is not None:
        lg[flip_at, 0], lg[flip_at, 1] = 9.95, 10.05
    return lg


def run_gate(tmp, capsys, ref, run, linux_logits):
    prompts = [f"p{i:03d}" for i in range(1, 11)]
    a = write_run(tmp, "ref", prompts, [list(np.argmax(lg, 1).tolist()) for lg in ref], ref)
    m = write_run(tmp, "run", prompts, [list(np.argmax(lg, 1).tolist()) for lg in run], run)
    with (tmp / "linux.jsonl").open("w") as f:
        for p, lg in zip(prompts, linux_logits):
            ids = np.argsort(lg, 1)[:, ::-1]
            for k in range(len(lg)):
                f.write(json.dumps({"type": "step", "prompt": p, "gen_index": k, "token_out": int(ids[k, 0]),
                                    "top1": float(lg[k, ids[k, 0]]), "top2_id": int(ids[k, 1]),
                                    "top2": float(lg[k, ids[k, 1]])}) + "\n")
            f.write(json.dumps({"type": "prompt", "prompt": p,
                                "generated_ids": ",".join(map(str, ids[:, 0]))}) + "\n")
    args = SimpleNamespace(ref_json=a[0], ref_logits=a[1], run_json=m[0], run_logits=m[1],
                           linux=tmp / "linux.jsonl", hwx_same="yes")
    assert gate(args) == 0
    return json.loads(capsys.readouterr().out)


def test_gate_verdicts(tmp_path, capsys):
    rng = np.random.default_rng(0)
    ref = [greedy(rng) for _ in range(10)]
    linux = [greedy(rng, flip_at=2) if i < 7 else ref[i] for i in range(10)]
    # Native run equal to the Linux run: H_driver, G1 passes, G2 passes (near-tie flips only).
    out = run_gate(tmp_path, capsys, ref, linux, linux)
    assert out["verdict"] == "H_driver" and out["G1_linux_vs_run"] and out["G2_run_vs_ref"]
    assert out["n_run_ref"] == 3 and out["n_run_linux"] == 10
    # Native run equal to the reference while Linux differs on 7 prompts: H_compile.
    out = run_gate(tmp_path, capsys, ref, ref, linux)
    assert out["verdict"] == "H_compile" and not out["G1_linux_vs_run"]
    # Native run that flips other positions than Linux on 7 prompts: H_noise.
    other = [greedy(rng, flip_at=3) if i < 7 else ref[i] for i in range(10)]
    out = run_gate(tmp_path, capsys, ref, other, linux)
    assert out["verdict"] == "H_noise"
    # A flip at a reference gap of 3.0 (above the 1.5 bound) fails G2.
    far = [lg.copy() for lg in ref]
    far[0][1, 1] = 12.0
    big = [lg.copy() for lg in ref]
    big[0][1, 0] = 12.0
    out = run_gate(tmp_path, capsys, big, far, linux)
    assert not out["G2_run_vs_ref"]
