#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Evaluate a staged Qwen MIL program in float64 and measure the M1 band.

run_mil() evaluates the ops the 38 staged programs use, with the fp16
constants from weights.bin (BLOBFILE data starts 64 bytes after the recorded
offset) and no intermediate rounding. The command line feeds each program
the M1 inputs of the per-step dump and prints, per output port, the M1
output's distance from that float64 result: the M1's own error band, which
bounds the M2-vs-M1 pass threshold per output class.
"""

import argparse
import json
import re
from pathlib import Path

import numpy as np

UNARY = {
    "silu": lambda x: x / (1 + np.exp(-x)),
    "sigmoid": lambda x: 1 / (1 + np.exp(-x)),
    "softplus": lambda x: np.logaddexp(0, x),
    "relu": lambda x: np.maximum(x, 0),
    "exp": np.exp,
}
BINARY = {"add": np.add, "sub": np.subtract, "mul": np.multiply, "real_div": np.divide,
          "maximum": np.maximum, "minimum": np.minimum}
LINE = re.compile(r"^\s*\S.*?\s(\w+)\s*=\s*(\w+)\((.*)\)\[(.*)\];\s*$")


def split_top(text):
    """Split on commas outside brackets."""
    parts, depth, start = [], 0, 0
    for i, ch in enumerate(text):
        if ch in "([<":
            depth += 1
        elif ch in ")]>":
            depth -= 1
        elif ch == "," and depth == 0:
            parts.append(text[start:i])
            start = i + 1
    parts.append(text[start:])
    return [p.strip() for p in parts if p.strip()]


def literal(text, env, weights):
    if re.fullmatch(r"\w+", text):
        return env[text]
    if text.startswith("("):
        return [literal(t, env, weights) for t in split_top(text[1:-1])]
    m = re.fullmatch(r"(bool|fp16|fp32|int32|uint64|string)\((.*)\)", text)
    if m:
        kind, inner = m.groups()
        if kind == "bool":
            return inner == "true"
        if kind.startswith("fp"):
            return float.fromhex(inner) if "0x" in inner else float(inner)
        return int(inner) if kind != "string" else inner.strip('"')
    m = re.fullmatch(r"tensor<(\w+),\s*\[([^\]]*)\]>\((.*)\)", text)
    if not m:
        raise ValueError(f"unknown MIL literal {text[:80]}")
    dtype, dims, inner = m.groups()
    shape = [int(d) for d in dims.split(",") if d.strip()]
    blob = re.search(r"offset\s*=\s*uint64\((\d+)\)", inner)
    if blob:
        if dtype != "fp16":
            raise ValueError(f"BLOBFILE of {dtype}")
        count = int(np.prod(shape))
        return np.frombuffer(weights, np.float16, count, int(blob.group(1)) + 64).astype(np.float64).reshape(shape)
    return np.array(json.loads(inner), dtype=np.float64 if dtype.startswith("fp") else np.int64).reshape(shape)


def run_op(op, a):
    if op in UNARY:
        return UNARY[op](a["x"])
    if op in BINARY:
        return BINARY[op](a["x"], a["y"])
    if op == "reshape":
        return a["x"].reshape([int(d) for d in a["shape"]])
    if op == "transpose":
        return np.transpose(a["x"], [int(d) for d in a["perm"]])
    if op == "matmul":
        x, y = a["x"], a["y"]
        return np.matmul(x.swapaxes(-1, -2) if a["transpose_x"] else x,
                         y.swapaxes(-1, -2) if a["transpose_y"] else y)
    if op in ("reduce_sum", "reduce_l2_norm"):
        axes = tuple(int(d) for d in a["axes"])
        if op == "reduce_sum":
            return a["x"].sum(axis=axes, keepdims=a["keep_dims"])
        return np.sqrt((a["x"] ** 2).sum(axis=axes, keepdims=a["keep_dims"]))
    if op == "slice_by_size":
        x = a["x"]
        return x[tuple(slice(int(b), x.shape[i] if int(s) == -1 else int(b) + int(s))
                       for i, (b, s) in enumerate(zip(a["begin"], a["size"])))]
    if op == "concat":
        if a["interleave"]:
            raise ValueError("concat interleave")
        return np.concatenate(a["values"], axis=int(a["axis"]))
    if op == "softmax":
        x = a["x"] - a["x"].max(axis=int(a["axis"]), keepdims=True)
        return np.exp(x) / np.exp(x).sum(axis=int(a["axis"]), keepdims=True)
    raise ValueError(f"unsupported MIL op {op}")


def parse_mil(mil_path):
    """One MIL program as (input shapes, float64 constants, ops, returned
    names), for run_mil. weights.bin is memory-mapped."""
    mil_path = Path(mil_path)
    weights = np.memmap(mil_path.parent / "weights.bin", np.uint8, "r")
    text = mil_path.read_text()
    sig = re.search(r"func main<\w+>\((.*?)\)\s*\{", text, re.S).group(1)
    shapes = {}
    for decl in split_top(sig):
        m = re.fullmatch(r"tensor<\w+,\s*\[([^\]]*)\]>\s+(\w+)", decl)
        shapes[m.group(2)] = [int(d) for d in m.group(1).split(",")]
    consts, ops = {}, []
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        name, op, args, attrs = m.groups()
        if op == "const":
            consts[name] = literal(re.search(r"\bval\s*=\s*(.*)$", attrs).group(1), consts, weights)
        else:
            ops.append((name, op, {k.strip(): v.strip() for k, v in (p.split("=", 1) for p in split_top(args))}))
    returns = re.search(r"\}\s*->\s*\(([^)]*)\)", text).group(1)
    return shapes, consts, ops, [n.strip() for n in returns.split(",")]


def run_mil(program, inputs, post=None):
    """program: parse_mil() output. inputs: name -> array (any float dtype).
    Returns name -> float64 array for every returned tensor. post(op, value,
    const_args), if given, replaces each op result (a rounding model);
    const_args holds the names of the op's arguments that are constants."""
    shapes, consts, ops, returns = program
    env = dict(consts)
    env.update({n: np.asarray(inputs[n], dtype=np.float64).reshape(s) for n, s in shapes.items()})
    for name, op, raw in ops:
        out = run_op(op, {k: literal(v, env, None) for k, v in raw.items()})
        env[name] = post(op, out, {k for k, v in raw.items() if v in consts}) if post else out
    return {n: env[n] for n in returns}


def main(argv=None):
    from qwen_m2_conform import compare, m1_io, parse_list

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dump", default="/var/tmp/qwen38-step-goldens")
    ap.add_argument("--mil-dir", required=True, help="dir with prog_NNN/model.mil and weights.bin")
    ap.add_argument("--steps", default="0,1,2,11,12,13")
    ap.add_argument("--progs", default="0-37")
    args = ap.parse_args(argv)
    dump = Path(args.dump)
    index = json.loads((dump / "index.json").read_text())
    executions = {(e["step"], e["program"]): e for e in index["executions"]}
    for prog in parse_list(args.progs):
        program = parse_mil(Path(args.mil_dir) / f"prog_{prog:03d}" / "model.mil")
        for step in parse_list(args.steps):
            execution = executions[step, prog]
            ins = {n: a for n, (a, _) in m1_io(dump, execution, "inputs").items()}
            ref = run_mil(program, ins)
            for name, (m1, entry) in m1_io(dump, execution, "outputs").items():
                exact = ref[name].ravel()
                c = compare(m1, exact)
                print(json.dumps({"prog": prog, "step": step, "port": name, "kind": entry["kind"],
                                  "lane": entry["lane"], "m1_vs_fp64_rel_l2": c["rel_l2"],
                                  "m1_vs_fp64_max_abs": c["max_abs"],
                                  "fp64_rms": float(np.sqrt(np.mean(exact ** 2)))}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
