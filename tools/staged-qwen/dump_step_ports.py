#!/usr/bin/env python3
"""Dump every staged-program execution's exact fp16 ports for chosen decode steps of p001.

macOS e5rt path (ANEForge lane/deltanet-split-decode). Step S is the S-th step() of
generate(batched_prefill=False): position S, fed ids[S] while S < len(prompt), else the
previous greedy token. Per captured execution:

  <out>/step{S}/prog_{NNN}/in_{PORT}.f16   lane/ctx inputs as handed to set_input;
                                           resident state inputs read from the aliased
                                           state port just before execute
  <out>/step{S}/prog_{NNN}/out_{PORT}.f16  lane outputs as read_output returns them;
                                           state outputs read just after execute
  <out>/index.json                         order, step, position, token, program, port,
                                           kind, lane, shape, dtype, bytes, sha256

Raw little-endian fp16, dense logical layout (the bytes goldens.json hashes). The
wrappers only copy; the generate() numerics are untouched. A warmup generate runs
first, so step-0 state inputs reading back as zeros proves the state read-back.

  ANEFORGE_PATH=~/src/ane-af-split-wt python3 dump_step_ports.py --gguf Qwen3.8-2B-Q4_K_M.gguf \
    --manifest qwen38-staged-manifest/manifest.json --ref chunk_00.json \
    --steps 0,1,2,11,12,13 --new-tokens 3 --out step-goldens
"""
import argparse, hashlib, json, os, sys

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--gguf", required=True)
ap.add_argument("--manifest", required=True, help="staged_qwen_manifest.py manifest.json")
ap.add_argument("--ref", required=True, help="chunk_00.json (p001 prompt ids)")
ap.add_argument("--steps", required=True, help="comma-separated step indices")
ap.add_argument("--new-tokens", type=int, default=3)
ap.add_argument("--out", required=True)
a = ap.parse_args()

sys.path.insert(0, os.environ.get("ANEFORGE_PATH", os.path.expanduser("~/src/ane-af-split-wt")))
from aneforge.qwen35 import load_gguf

manifest = json.load(open(a.manifest))
ref = json.load(open(a.ref))
p001 = ref["prompts"][0]
ids = [int(t) for t in p001["prompt_token_ids"]]
M = int(manifest["max_len"])
assert M == int(ref["max_len"]), "manifest and reference max_len differ"
wanted = {int(s) for s in a.steps.split(",")}

m = load_gguf(a.gguf, resid_scale=1.0)   # contract macos_ane: resid_scale 1.0
m.ane_lm_head = False                    # contract: host fp32 lm_head
chunks = m._decoder(M)["chunks"]
assert len(chunks) == manifest["n_programs"], "live decoder and manifest disagree on program count"

# the live compile must expose exactly the manifest's ports, or the dump is mislabelled
for ci, (c, prog) in enumerate(zip(chunks, manifest["programs"])):
    pr = c["net"].prog
    ins = {s["port"] for s in prog["srcs"]}
    outs = {d["port"] for d in prog["dsts"]} | {s["out_port"] for s in prog["states"]}
    assert set(pr._inputs) == ins and set(pr._outputs) == outs, f"prog {ci}: port set drift"

warm = m.generate(ids, max_new_tokens=a.new_tokens, max_len=M, temperature=0.0,
                  top_p=1.0, top_k=0, batched_prefill=False)
print(f"warmup generate: {warm}", flush=True)

records = []
cur = {"step": -1, "exec": None}
tokens = list(ids)   # step S feeds tokens[S]; generated ids are appended after the run


def save(ci, direction, port, arr, kind, lane):
    data = np.ascontiguousarray(arr, dtype="<f2")
    rel = f"step{cur['step']}/prog_{ci:03d}/{direction}_{port}.f16"
    path = os.path.join(a.out, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    raw = data.tobytes()
    with open(path, "wb") as f:
        f.write(raw)
    cur["exec"][direction + "puts"].append({
        "port": port, "kind": kind, "lane": lane, "shape": list(data.shape),
        "dtype": "float16-le", "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(),
        "file": rel})


for ci, (c, prog) in enumerate(zip(chunks, manifest["programs"])):
    pr = c["net"].prog
    role = {s["port"]: (s["kind"], s["lane"]) for s in prog["srcs"]}
    role |= {d["port"]: (d["kind"], d["lane"]) for d in prog["dsts"]}
    states = [(s["in_port"], s["out_port"], f"state{k}") for k, s in enumerate(prog["states"])]
    o_si, o_ex, o_ro = pr.set_input, pr.execute, pr.read_output

    def si(port, arr, ci=ci, role=role, o_si=o_si):
        if ci == 0 and port == manifest["programs"][0]["srcs"][0]["port"]:
            cur["step"] += 1   # program 0's lane x is the first write of every step
        if cur["step"] in wanted:
            if cur["exec"] is None or cur["exec"]["program"] != ci:
                cur["exec"] = {"order": len(records), "step": cur["step"], "position": cur["step"],
                               "program": ci, "inputs": [], "outputs": []}
                records.append(cur["exec"])
            save(ci, "in", port, arr, *role[port])
        return o_si(port, arr)

    def ex(ci=ci, states=states, o_ex=o_ex, o_ro=o_ro):
        live = cur["step"] in wanted
        if live:
            for in_port, out_port, lane in states:   # resident: the aliased buffer is the input
                save(ci, "in", in_port, o_ro(out_port), "state_in", lane)
        o_ex()
        if live:
            for in_port, out_port, lane in states:
                save(ci, "out", out_port, o_ro(out_port), "state_out", lane)

    def ro(port, ci=ci, role=role, o_ro=o_ro):
        v = o_ro(port)
        if cur["step"] in wanted:
            save(ci, "out", port, v, *role[port])
        return v

    pr.set_input, pr.execute, pr.read_output = si, ex, ro

os.makedirs(a.out, exist_ok=True)
gen = m.generate(ids, max_new_tokens=a.new_tokens, max_len=M, temperature=0.0,
                 top_p=1.0, top_k=0, batched_prefill=False)
tokens += gen
for r in records:
    r["token"] = tokens[r["step"]]
    if r["step"] >= len(ids) - 1:
        r["next_token"] = tokens[r["step"] + 1]   # host fp32 lm_head argmax of this step's h
    r["inputs"].sort(key=lambda x: [s["port"] for s in manifest["programs"][r["program"]]["srcs"]].index(x["port"]))
index = {
    "prompt": p001["id"], "prompt_token_ids": ids, "max_len": M,
    "generated_ids": gen, "warmup_generated_ids": warm,
    "reference_generated_ids": p001["runs"][0]["generated_ids"][: a.new_tokens],
    "steps": sorted(wanted), "steps_run": cur["step"] + 1,
    "step_semantics": "step S = S-th generate() step at position S; token = id fed "
                      "(prompt ids, then greedy outputs); steps >= len(prompt)-1 produce generated_ids",
    "executions": records,
}
json.dump(index, open(os.path.join(a.out, "index.json"), "w"), indent=1)
print(f"generated {gen} (warmup {warm}); {len(records)} executions over steps "
      f"{sorted(wanted)} -> {a.out}/index.json")
assert gen == warm, "instrumented run diverged from the warmup run"
assert all(len({r['program'] for r in records if r['step'] == s}) == len(chunks) for s in wanted), \
    "a wanted step was not fully captured"
