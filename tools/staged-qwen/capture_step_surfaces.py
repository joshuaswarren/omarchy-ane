#!/usr/bin/env python3
"""Capture decode steps 0-1 of prompt p001 on the macOS e5rt path, at both ends.

- dense/: every program's port values as ANEForge hands them to e5rt (set_input)
  and gets them back (read_output), first occurrence = step 0
- surfaces/: the IOSurfaces the ANE runtime actually hands the engine for the first
  two steps' evaluations (ane_request_capture.m; eval k = program k % n of step
  k // n), raw bytes + surface properties + the model's LiveInput/LiveOutput
  attributes. Step 1 exists for the resident states: they never cross read_output,
  and step 0 feeds them zeros, so their placement is checked step-0 out -> step-1 in.

check_step_surfaces.py then packs dense/ with io_layout.json geometry and
byte-compares against surfaces/.

  ANEFORGE_PATH=~/src/ane-af-split-wt python3 capture_step_surfaces.py \
    --gguf Qwen3.8-2B-Q4_K_M.gguf --ref chunk_00.json \
    --hook libane_request_capture.dylib --out step0
"""
import argparse, ctypes, json, os, sys

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--gguf", required=True)
ap.add_argument("--ref", required=True)
ap.add_argument("--hook", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--max-len", type=int, default=50)
a = ap.parse_args()

hook = ctypes.CDLL(os.path.abspath(a.hook))   # constructor swizzles the ANE client
hook.anecap_arm.argtypes = [ctypes.c_char_p, ctypes.c_int]
hook.anecap_count.restype = ctypes.c_int

sys.path.insert(0, os.environ.get("ANEFORGE_PATH", os.path.expanduser("~/src/ane-af-split-wt")))
from aneforge.qwen35 import load_gguf

m = load_gguf(a.gguf, resid_scale=1.0)   # contract macos_ane
m.ane_lm_head = False
d = m._decoder(int(a.max_len))
n = len(d["chunks"])
dense = os.path.join(a.out, "dense")
os.makedirs(dense, exist_ok=True)
seen = set()


def keep(ci, kind, port, arr):
    key = (ci, kind, port)
    if key not in seen:
        seen.add(key)
        np.save(os.path.join(dense, f"prog_{ci:03d}_{kind}_{port}.npy"), np.array(arr, copy=True))


for ci, c in enumerate(d["chunks"]):
    pr = c["net"].prog
    o_si, o_ro = pr.set_input, pr.read_output

    def si(port, arr, ci=ci, o_si=o_si):
        keep(ci, "in", port, np.ascontiguousarray(arr))
        return o_si(port, arr)

    def ro(port, ci=ci, o_ro=o_ro):
        v = o_ro(port)
        keep(ci, "out", port, v)
        return v

    pr.set_input, pr.read_output = si, ro

ids = json.load(open(a.ref))["prompts"][0]["prompt_token_ids"]
hook.anecap_arm(os.path.join(a.out, "surfaces").encode(), 2 * n)
m.generate(ids, max_new_tokens=1, max_len=int(a.max_len), temperature=0.0,
           top_p=1.0, top_k=0, batched_prefill=False)
print(f"captured {hook.anecap_count()} ANE evaluations (armed {2 * n}), "
      f"{len(seen)} dense port values -> {a.out}")
assert hook.anecap_count() == 2 * n, "prompt shorter than two steps"
