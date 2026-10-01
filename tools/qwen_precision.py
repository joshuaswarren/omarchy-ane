#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Offline precision analysis of the staged Qwen3.8-2B decode (CPU only).

chain      the 38 MIL programs (mil_eval, float64) chained over each run's
           reference tokens up to its generated index; a run is
           prompt:last_index:rounding (rounding none = float64). All runs
           step together, so each program is parsed once per step. Saves h
           after program 37 at each generated position (<out>/<prompt>-<round>.npz)
           and, for the dump's prompt, prints every output port's rel L2 of
           the M1 (--dump) and M2 (--m2-dump) arrays from the chain.
decompose  per program and output port at the dump steps, with f the float64
           MIL on a chip's own inputs: M2out - M1out = local + propagated,
           local = (M2out - f(M2in)) - (M1out - f(M1in)),
           propagated = f(M2in) - f(M1in); norms relative to |M1out|. local is
           not zero for a program that is bit-exact across chips when its
           device error depends on the input.
gaps       at each first divergence of a decode run, the logit gap g =
           logit(reference token) - logit(run token) in the reference, the run
           and the fp64 chain, and the margin errors on the agreeing prefix.
gate       the pre-registered decision rule (H_driver / H_compile / H_noise,
           gates G1 and G2) for a native run in the chunk_00 layout against the
           reference and the Linux run.
noise      the flip model behind the gate thresholds: how many prompts a run
           with the measured margin noise is expected to keep equal to the
           reference.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

from mil_eval import parse_mil, run_mil
from qwen_m2_decode import ctx_vals, rope_params, rope_tables, top2

f16 = np.float16
GRID, GRID_TOP = 2.0 ** -16, 2.0 ** -6


def fp16(v):
    return np.asarray(v).astype(f16).astype(np.float64)


def t6021_grid(v):
    """T6021 bmm output (receipts/2026-09-30-t6021-accumulator, probe 3):
    below 2^-6 the nearest multiple of 2^-16, ties away from zero; else fp16."""
    q = np.sign(v) * np.floor(np.abs(v) / GRID + 0.5) * GRID
    return np.where(np.abs(v) < GRID_TOP, q, fp16(v))


ROUND = {
    "none": None,
    "fp16": lambda op, v, consts: fp16(v),
    "fp16-grid-bmm": lambda op, v, consts: t6021_grid(v) if op == "matmul" and "y" not in consts else fp16(v),
    "fp16-grid-matmul": lambda op, v, consts: t6021_grid(v) if op == "matmul" else fp16(v),
}


def rel(a, b):
    a, b = np.asarray(a, np.float64).ravel(), np.asarray(b, np.float64).ravel()
    return float(np.linalg.norm(a - b) / np.linalg.norm(b))


def load_dump(root):
    index = json.loads((Path(root) / "index.json").read_text())
    return {(e["step"], e["program"]): e for e in index["executions"]}


def read(root, entry):
    return np.fromfile(Path(root) / entry["file"], dtype=f16).astype(np.float64)


def chain(args):
    from gguf import GGUFReader
    from gguf.quants import dequantize
    progs = json.loads(Path(args.manifest).read_text())["programs"]
    prompts = {p["id"]: p for p in json.loads(Path(args.ref).read_text())["prompts"]}
    reader = GGUFReader(args.gguf)
    emb = next(t for t in reader.tensors if t.name == "token_embd.weight")
    cos, sin = rope_tables(args.max_len, *rope_params(reader))
    dump = load_dump(args.dump) if args.dump else {}
    dump_prompt = json.loads((Path(args.dump) / "index.json").read_text())["prompt"] if args.dump else None
    runs = []
    for spec in args.runs:
        pid, last, rounding = spec.split(":")
        p = prompts[pid]
        first = len(p["prompt_token_ids"]) - 1
        runs.append({"name": f"{pid}-{rounding}", "pid": pid, "post": ROUND[rounding], "first": first,
                     "last": first + int(last), "ids": p["prompt_token_ids"] + p["generated_ids"], "hs": [],
                     "states": [{s["in_port"]: np.zeros(s["in_shape"]) for s in pr["states"]} for pr in progs]})
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    for step in range(max(r["last"] for r in runs) + 1):
        live = [r for r in runs if step <= r["last"]]
        vals = {k: v.astype(np.float64) for k, v in ctx_vals(step, args.max_len, cos, sin).items()}
        for r in live:
            r["hidden"] = fp16(dequantize(emb.data[r["ids"][step]:r["ids"][step] + 1], emb.tensor_type))
            r["lanes"] = {}
        for i, pr in enumerate(progs):
            program = parse_mil(Path(args.mil_dir) / f"prog_{i:03d}" / "model.mil")
            for r in live:
                lanes, states = r["lanes"], r["states"][i]
                if pr["group_start"]:
                    lanes["x"] = r["hidden"]
                ins = {s["port"]: states[s["port"]] if s["kind"] == "state_in" else
                       vals[s["lane"]] if s["kind"] == "ctx" else lanes[s["lane"]] for s in pr["srcs"]}
                out = run_mil(program, ins, r["post"])
                for d in pr["dsts"]:
                    lanes[d["lane"]] = out[d["port"]]
                for s in pr["states"]:
                    states[s["in_port"]] = out[s["out_port"]].reshape(s["in_shape"])
                if pr["group_end"]:
                    r["hidden"] = lanes["h"]
                if r["pid"] != dump_prompt or (step, i) not in dump:
                    continue
                if i == 0:
                    x = next(p for p in dump[step, 0]["inputs"] if p["lane"] == "x")
                    assert np.array_equal(ins[x["port"]].ravel(), read(args.dump, x)), "embedding row"
                for p in dump[step, i]["outputs"]:
                    m1, m2, exact = read(args.dump, p), read(args.m2_dump, p), out[p["port"]].ravel()
                    print(json.dumps({"run": r["name"], "step": step, "prog": i, "port": p["port"],
                                      "kind": p["kind"], "lane": p["lane"], "m1_vs_chain": rel(m1, exact),
                                      "m2_vs_chain": rel(m2, exact), "m2_vs_m1": rel(m2, m1)}), flush=True)
        for r in live:
            if step >= r["first"]:
                r["hs"].append(r["hidden"].ravel())
            if step == r["last"]:
                np.savez(out_dir / f"{r['name']}.npz", h=np.array(r["hs"]), first_position=r["first"])
        print(f"step {step} runs {len(live)} {time.monotonic() - start:.0f}s", file=sys.stderr, flush=True)
    return 0


def decompose(args):
    progs = json.loads(Path(args.manifest).read_text())["programs"]
    m1x = load_dump(args.dump)
    for i, pr in enumerate(progs):
        program = parse_mil(Path(args.mil_dir) / f"prog_{i:03d}" / "model.mil")
        for step in sorted(s for s, j in m1x if j == i):
            e = m1x[step, i]
            f1 = run_mil(program, {p["port"]: read(args.dump, p) for p in e["inputs"]})
            f2 = run_mil(program, {p["port"]: read(args.m2_dump, p) for p in e["inputs"]})
            d_in = max((rel(read(args.m2_dump, p), read(args.dump, p)) for p in e["inputs"]
                        if p["kind"] != "ctx" and np.linalg.norm(read(args.dump, p))), default=0.0)
            for p in e["outputs"]:
                m1, m2 = read(args.dump, p), read(args.m2_dump, p)
                a, b = f1[p["port"]].ravel(), f2[p["port"]].ravel()
                n = np.linalg.norm(m1)
                print(json.dumps({"step": step, "prog": i, "port": p["port"], "kind": p["kind"],
                                  "lane": p["lane"], "group_end": pr["group_end"], "in_m2_vs_m1": d_in,
                                  "m2_vs_m1": float(np.linalg.norm(m2 - m1) / n),
                                  "local": float(np.linalg.norm((m2 - b) - (m1 - a)) / n),
                                  "propagated": float(np.linalg.norm(b - a) / n),
                                  "band_m1": rel(m1, a), "band_m2": rel(m2, b)}), flush=True)
    return 0


def gaps(args):
    """One JSON record per prompt of the decode run (qwen_m2_decode results.jsonl)."""
    from qwen_m2_decode import load_head
    _, head, _ = load_head(args.gguf)
    ref = {p["id"]: p for p in json.loads(Path(args.ref).read_text())["prompts"]}
    ref_logits = np.load(args.ref_logits)
    recs = [json.loads(line) for line in Path(args.results).read_text().splitlines()]
    order = [p["id"] for p in json.loads(Path(args.ref).read_text())["prompts"]]
    for pr in (r for r in recs if r["type"] == "prompt" and not r["match"]):
        pid, k = pr["prompt"], pr["first_divergence"]
        rl = ref_logits[f"prompt_{order.index(pid):03d}"]
        want = ref[pid]["generated_ids"]
        steps = {r["gen_index"]: r for r in recs if r["type"] == "step" and r["prompt"] == pid and "gen_index" in r}
        run_tok, ref_tok = steps[k]["token_out"], want[k]
        out = {"prompt": pid, "gen": k, "run_token": run_tok, "ref_token": ref_tok,
               "g_ref": float(rl[k][ref_tok] - rl[k][run_tok]),
               "g_run": float(steps[k]["top2"] - steps[k]["top1"]) if steps[k]["top2_id"] == ref_tok else None,
               "ref_top2": list(top2(rl[k])[::2])}
        path = Path(args.chains) / f"{pid}-none.npz"
        if path.exists():
            z = np.load(path)
            lg = z["h"].astype(np.float32) @ head.T
            out["g_fp64"] = float(lg[k][ref_tok] - lg[k][run_tok])
            out["fp64_top2"] = list(top2(lg[k])[::2])
            out["fp64_prefix_agree"] = int(sum(int(np.argmax(lg[j])) == want[j] for j in range(k)))
            ref_m = [top2(rl[j]) for j in range(k)]
            out["fp64_margin_err"] = [float(abs((lg[j][a] - lg[j][b]) - (va - vb))) for j, (a, va, b, vb) in
                                      enumerate(ref_m)]
            out["run_margin_err"] = [float(abs(steps[j]["margin"] - steps[j]["ref_margin"])) for j in range(k)]
            out["fp64_logit_max_abs"] = [float(np.abs(lg[j] - rl[j]).max()) for j in range(k + 1)]
        print(json.dumps(out), flush=True)
    return 0


def poisson_binomial(ps):
    dist = np.zeros(len(ps) + 1)
    dist[0] = 1.0
    for p in ps:
        dist[1:] = dist[1:] * (1 - p) + dist[:-1] * p
        dist[0] *= 1 - p
    return dist


def noise(args):
    """Flip model of a decode run vs the reference. delta = run margin - reference margin on every position
    with the reference prefix (at a divergence: run gap of the reference token - reference margin); a reference
    token with margin m flips when delta < -m. Per model (empirical delta, symmetrized |delta|, Laplace with
    scale median|delta|/ln 2): expected number of prompts equal to the reference, the count distribution, and
    the expected flips at reference margins above X."""
    recs = [json.loads(line) for line in Path(args.results).read_text().splitlines()]
    delta = np.array([(r["margin"] if r["token_out"] == r["ref_token"] else r["top2"] - r["top1"]) - r["ref_margin"]
                      for r in recs if r["type"] == "step" and "ref_margin" in r
                      and (r["token_out"] == r["ref_token"] or r["top2_id"] == r["ref_token"])])
    margins = {pid: np.array([margin(row) for row in lg]) for pid, (_, _, lg) in
               chunk_runs(args.ref_json, args.ref_logits).items()}
    observed = sum(r["match"] for r in recs if r["type"] == "prompt")
    scale = float(np.median(np.abs(delta)) / np.log(2))
    models = {"empirical": lambda m: float(np.mean(delta < -m)),
              "symmetrized": lambda m: float(np.mean(np.abs(delta) > m)) / 2,
              "laplace": lambda m: 0.5 * float(np.exp(-m / scale))}
    out = {"n_delta": len(delta), "delta_median": float(np.median(delta)), "laplace_scale": scale,
           "abs_delta_pct": {q: float(np.percentile(np.abs(delta), q)) for q in (50, 90, 95, 99, 100)},
           "observed_matches": observed}
    for name, flip in models.items():
        p_match = {pid: float(np.prod([1 - flip(m) for m in ms])) for pid, ms in margins.items()}
        dist = poisson_binomial(list(p_match.values()))
        out[name] = {"p_match": p_match, "expected_matches": float(sum(p_match.values())),
                     "p_at_most_observed": float(dist[:observed + 1].sum()),
                     f"p_at_least_{H_COMPILE_N}": float(dist[H_COMPILE_N:].sum()), "p_all": float(dist[-1]),
                     "expected_flips_above": {x: float(sum(flip(m) for ms in margins.values() for m in ms if m > x))
                                              for x in (0.84, 1.2, G2_GAP)}}
    print(json.dumps(out, indent=1))
    return 0


# Pre-registered 2026-10-01T11:55Z (receipts/2026-10-01-t6021-qwen-decode/precision.md), before any native run.
E_DRIVER, TIE, H_COMPILE_N, G2_GAP, G2_REL, G2_MARGIN = 1e-3, 2e-3, 8, 1.5, 0.10, 0.25


def chunk_runs(json_path, npz_path):
    """prompt id -> (prompt ids, generated ids, logits [n, vocab]) from the chunk_00.json/.npz layout."""
    z = np.load(npz_path)
    return {p["id"]: (p["prompt_token_ids"], p["runs"][0]["generated_ids"], z[f"prompt_{n:03d}"])
            for n, p in enumerate(json.loads(Path(json_path).read_text())["prompts"])}


def linux_run(path):
    """prompt id -> (generated ids, {gen index: (top1 id, top1, top2 id, top2)}) from qwen_m2_decode results."""
    recs = [json.loads(line) for line in Path(path).read_text().splitlines()]
    top = {}
    for r in recs:
        if r["type"] == "step" and "gen_index" in r:
            top.setdefault(r["prompt"], {})[r["gen_index"]] = (r["token_out"], r["top1"], r["top2_id"], r["top2"])
    return {r["prompt"]: ([int(t) for t in r["generated_ids"].split(",")], top[r["prompt"]])
            for r in recs if r["type"] == "prompt"}


def same_prefix(a, b):
    return next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))


def margin(logits):
    _, v1, _, v2 = top2(logits)
    return v1 - v2


def gate(args):
    """Decision rule for a native run M against the reference A and the Linux run L."""
    A, M, L = chunk_runs(args.ref_json, args.ref_logits), chunk_runs(args.run_json, args.run_logits), \
        linux_run(args.linux)
    invalid = [pid for pid, (ids, gen, lg) in M.items()
               if pid not in A or ids != A[pid][0] or len(gen) != len(A[pid][1]) or len(lg) != len(gen)
               or not np.isfinite(lg).all() or [int(np.argmax(row)) for row in lg] != gen]
    invalid += [pid for pid in A if pid not in M]
    out = {"invalid": invalid}
    if not invalid:
        n = len(next(iter(A.values()))[1])
        out["n_run_ref"] = sum(M[p][1] == A[p][1] for p in A)
        out["n_run_linux"] = sum(M[p][1] == L[p][0] for p in A)
        out["n_linux_ref"] = sum(L[p][0] == A[p][1] for p in A)
        d_l, flips, deltas, rels, g2 = 0.0, [], [], [], []
        for pid in A:
            gen, lg = M[pid][1], M[pid][2]
            k = same_prefix(gen, L[pid][0])
            for j in range(min(k, n - 1) + 1):
                t1, v1, t2, v2 = L[pid][1][j]
                d_l = max(d_l, abs(float(lg[j][t1]) - v1), abs(float(lg[j][t2]) - v2))
            if k < n:
                t1, v1, _, v2 = L[pid][1][k]
                flips.append({"prompt": pid, "gen": k, "linux_margin": v1 - v2})
            ref, rl = A[pid][1], A[pid][2]
            k = same_prefix(gen, ref)
            for j in range(min(k, n - 1) + 1):
                rels.append(rel(lg[j], rl[j]))
                if j < k:
                    deltas.append(abs(margin(lg[j]) - margin(rl[j])))
            if k < n:
                g2.append(top2(rl[k])[2] == gen[k] and top2(lg[k])[2] == ref[k]
                          and float(rl[k][ref[k]] - rl[k][gen[k]]) < G2_GAP)
        out.update({"dL": d_l, "linux_divergences": flips, "rel_l2_vs_ref_median": float(np.median(rels)),
                    "margin_err_vs_ref_median": float(np.median(deltas)) if deltas else 0.0,
                    "margin_err_vs_ref_p90": float(np.percentile(deltas, 90)) if deltas else 0.0})
        if d_l <= E_DRIVER and all(f["linux_margin"] <= TIE for f in flips):
            out["verdict"] = "H_driver"
        elif out["n_run_linux"] == len(A):
            out["verdict"] = "H_driver tokens only (dL above 1e-3)"
        elif out["n_run_ref"] >= H_COMPILE_N:
            out["verdict"] = "H_compile"
        else:
            out["verdict"] = "H_noise"
        out["G1_linux_vs_run"] = out["verdict"] == "H_driver"
        out["G2_run_vs_ref"] = all(g2) and out["rel_l2_vs_ref_median"] <= G2_REL \
            and out["margin_err_vs_ref_median"] <= G2_MARGIN and out["n_run_ref"] >= 1
    out["hwx_same"] = args.hwx_same
    print(json.dumps(out, indent=1))
    return 0 if not invalid else 2


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    common.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    common.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json")
    common.add_argument("--dump", help="M1 per-step dump (index.json)")
    common.add_argument("--m2-dump", help="M2 mirror dump (same layout)")
    mil = argparse.ArgumentParser(add_help=False)
    mil.add_argument("--mil-dir", required=True, help="dir with prog_NNN/model.mil and weights.bin")
    c = sub.add_parser("chain", parents=[common, mil])
    c.add_argument("runs", nargs="+", help=f"prompt:last_generated_index:rounding, rounding in {sorted(ROUND)}")
    c.add_argument("--max-len", type=int, default=50)
    c.add_argument("--out", required=True, help="output directory")
    sub.add_parser("decompose", parents=[common, mil])
    g = sub.add_parser("gaps", parents=[common])
    g.add_argument("--results", required=True, help="qwen_m2_decode results.jsonl")
    g.add_argument("--ref-logits", required=True)
    g.add_argument("--chains", required=True, help="chain output directory (<prompt>-none.npz)")
    t = sub.add_parser("gate", help="pre-registered decision rule for a native run")
    t.add_argument("--ref-json", required=True, help="reference chunk_00.json")
    t.add_argument("--ref-logits", required=True, help="reference chunk_00.npz")
    t.add_argument("--run-json", required=True, help="native run, chunk_00.json layout")
    t.add_argument("--run-logits", required=True, help="native run, chunk_00.npz layout")
    t.add_argument("--linux", required=True, help="Linux run results.jsonl (qwen_m2_decode)")
    t.add_argument("--hwx-same", choices=("yes", "no", "unknown"), default="unknown",
                   help="38/38 executed HWX equal the Linux run's after path normalization")
    nz = sub.add_parser("noise", help="flip model of a decode run vs the reference")
    nz.add_argument("--results", required=True, help="qwen_m2_decode results.jsonl (run with --ref-logits)")
    nz.add_argument("--ref-json", required=True, help="reference chunk_00.json")
    nz.add_argument("--ref-logits", required=True, help="reference chunk_00.npz")
    args = ap.parse_args(argv)
    return {"chain": chain, "decompose": decompose, "gaps": gaps, "gate": gate, "noise": noise}[args.cmd](args)


if __name__ == "__main__":
    raise SystemExit(main())
