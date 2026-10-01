#!/usr/bin/env python3
"""Offline check of a dump_step_ports.py dump (no ANE needed).

1. every file's size and sha256 match index.json;
2. goldens.json (manifest port order): its lane/ctx/output hashes are step 11 of p001
   (position 11, the last prompt token, which yields the first generated id); its
   state-input hashes are the reset-time zero fill, so they are checked against step 0;
3. chain closure: each lane input equals its producer's output in the same step
   (residual x follows the last x/h output; program 0's x is the host embedding),
   oh/inv/mask equal the host tables for the step's position, step-0 state inputs are
   zeros, and state inputs at step S+1 equal state outputs at step S;
4. optional --prog20 file equals program 20's output at the goldens step.

  check_step_dump.py --dump step-goldens --manifest manifest.json \
    --goldens goldens.json [--goldens-step 11] [--prog20 golden-t15.f16]
"""
import argparse, hashlib, json, os, sys

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--dump", required=True)
ap.add_argument("--manifest", required=True)
ap.add_argument("--goldens", required=True)
ap.add_argument("--goldens-step", type=int, default=11)
ap.add_argument("--prog20")
a = ap.parse_args()

index = json.load(open(os.path.join(a.dump, "index.json")))
programs = json.load(open(a.manifest))["programs"]
goldens = json.load(open(a.goldens))
M = index["max_len"]
problems = []
counts = dict.fromkeys(("files", "goldens", "lanes", "ctx", "state_zero", "state_chain"), 0)

by = {}   # (step, program) -> {"in": {port: entry}, "out": {port: entry}}
for r in index["executions"]:
    slot = by.setdefault((r["step"], r["program"]), {"in": {}, "out": {}})
    for direction, entries in (("in", r["inputs"]), ("out", r["outputs"])):
        for e in entries:
            raw = open(os.path.join(a.dump, e["file"]), "rb").read()
            if len(raw) != e["bytes"] or len(raw) != 2 * int(np.prod(e["shape"])) \
                    or hashlib.sha256(raw).hexdigest() != e["sha256"]:
                problems.append(f"{e['file']}: size/sha256 differs from index")
            counts["files"] += 1
            slot[direction][e["port"]] = e

G = a.goldens_step
for ci, prog in enumerate(programs):
    got, zero = by.get((G, ci)), by.get((0, ci))
    if got is None or zero is None:
        problems.append(f"prog {ci}: step {G} or step 0 not dumped")
        continue
    want = goldens[str(ci)]
    for s, h in zip(prog["srcs"], want["srcs"]):
        step = 0 if s["kind"] == "state_in" else G
        counts["goldens"] += 1
        if by[(step, ci)]["in"][s["port"]]["sha256"] != h:
            problems.append(f"step {step} prog {ci} in {s['port']} ({s['lane']}): != goldens")
    for d, h in zip(prog["dsts"], want["dsts"]):
        counts["goldens"] += 1
        if got["out"][d["port"]]["sha256"] != h:
            problems.append(f"step {G} prog {ci} out {d['port']} ({d['lane']}): != goldens")

for step in index["steps"]:
    oh = np.zeros((1, M, 1), np.float16); oh[0, step, 0] = 1.0
    inv = np.ones((1, M, 1), np.float16); inv[0, step, 0] = 0.0
    mask = np.full((1, 1, M), -1e4, np.float16); mask[..., :step + 1] = 0.0
    table = {k: hashlib.sha256(v.tobytes()).hexdigest() for k, v in
             (("oh", oh), ("inv", inv), ("mask", mask))}
    producer, residual = {}, None
    for ci, prog in enumerate(programs):
        got = by[(step, ci)]
        for s in prog["srcs"]:
            e = got["in"][s["port"]]
            if s["kind"] == "ctx" and s["lane"] in table:
                counts["ctx"] += 1
                if e["sha256"] != table[s["lane"]]:
                    problems.append(f"step {step} prog {ci} ctx {s['lane']} != host table")
            elif s["kind"] == "lane":
                if s["lane"] == "x" and residual is None:
                    residual = e   # host embedding row, carried until an x/h output
                    continue
                src = residual if s["lane"] == "x" else producer.get(s["lane"])
                counts["lanes"] += 1
                if src is None or src["sha256"] != e["sha256"]:
                    problems.append(f"step {step} prog {ci} lane {s['lane']} != producer")
            elif s["kind"] == "state_in" and step == 0:
                counts["state_zero"] += 1
                if e["sha256"] != hashlib.sha256(bytes(e["bytes"])).hexdigest():
                    problems.append(f"step 0 prog {ci} {s['port']}: state input not zeros")
        for d in prog["dsts"]:
            producer[d["lane"]] = got["out"][d["port"]]
            if d["lane"] in ("x", "h"):
                residual = got["out"][d["port"]]
        if step + 1 in index["steps"]:
            nxt = by[(step + 1, ci)]
            for st in prog["states"]:
                counts["state_chain"] += 1
                if nxt["in"][st["in_port"]]["sha256"] != got["out"][st["out_port"]]["sha256"]:
                    problems.append(f"prog {ci} {st['in_port']}: step {step + 1} in != step {step} out")

if a.prog20:
    counts["prog20"] = 1
    raw = open(a.prog20, "rb").read()
    if hashlib.sha256(raw).hexdigest() != by[(G, 20)]["out"][programs[20]["dsts"][0]["port"]]["sha256"]:
        problems.append(f"prog 20 step {G} output != --prog20 file")

for p in problems:
    print("FAIL", p)
print(" ".join(f"{k}={v}" for k, v in counts.items()), f"problems={len(problems)}")
print("generated", index["generated_ids"], "reference", index["reference_generated_ids"])
if index["generated_ids"] != index["reference_generated_ids"]:
    problems.append("generated ids != chunk_00 reference")
    print("FAIL generated ids != chunk_00 reference")
sys.exit(1 if problems else 0)
