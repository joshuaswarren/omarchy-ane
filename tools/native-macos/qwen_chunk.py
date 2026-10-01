#!/usr/bin/env python3
"""Staged Qwen3.8-2B greedy decode on this Mac's own ANE, saved in the chunk_00.json/.npz layout.

ANEForge (lane/deltanet-split-decode) emits the 38 programs from the GGUF and compiles them through
e5rt on this host, so the ANE compiler of this macOS build makes the executed programs. The host
side is the reference runner's: resid_scale 1.0, host fp32 lm_head, token-by-token prefill, greedy.

  ANEFORGE_PATH=aneforge-src ANEFORGE_CACHE_DIR=afcache python3 qwen_chunk.py --gguf Qwen3.8-2B-Q4_K_M.gguf \
    --ref chunk_00.json --keys qwen38-program-keys.txt --out out/chunk [--max-len 50 | --max-len 0] [--forced]

Writes <out>.json (prompts[i].runs[0].generated_ids, timings, metadata) and <out>.npz (prompt_NNN float32
[new_tokens, vocab], the logits of each greedy position). --max-len 50 runs the 38 programs of the staged
manifest for every prompt (the Linux run's programs). --max-len 0 rebuilds the decoder per prompt with
max_len = prompt length + new tokens, which is what the reference runner's generate() call did. --forced also
writes <out>-forced.npz: the logits with the reference ids forced as the generated tokens.
"""
import argparse, datetime, glob, hashlib, json, os, platform, sys, time

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("--gguf", required=True)
ap.add_argument("--ref", required=True, help="reference chunk_00.json")
ap.add_argument("--keys", required=True, help="lines 'prog_NNN <ANEForge cache key>' of the max_len 50 programs")
ap.add_argument("--out", required=True, help="output path prefix")
ap.add_argument("--max-len", type=int, default=50, help="0 = per prompt, prompt length + new tokens")
ap.add_argument("--new-tokens", type=int, default=32)
ap.add_argument("--warmup", type=int, default=1, help="untimed generate() runs of the first prompt")
ap.add_argument("--prompts", type=int, default=0, help="first N prompts only (0 = all)")
ap.add_argument("--forced", action="store_true")
ap.add_argument("--env", help="host facts file to embed (run.sh env.txt)")
a = ap.parse_args()

sys.path.insert(0, os.environ["ANEFORGE_PATH"])
from aneforge.qwen35 import load_gguf
from aneforge._compile import cache_root


def utc():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def sha256(*paths):
    h = hashlib.sha256()
    for path in paths:
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 24), b""):
                h.update(block)
    return h.hexdigest()


started = utc()
ref = json.load(open(a.ref))
prompts = ref["prompts"][: a.prompts or None]
assert sha256(a.gguf) == ref["model_sha256"], "GGUF differs from the reference model"
max_len = lambda p: a.max_len or len(p["prompt_token_ids"]) + a.new_tokens

m = load_gguf(a.gguf, n_layers=24, resid_scale=1.0)
m.ane_lm_head = False
t0 = time.perf_counter()
m.warmup(max_len(prompts[0]))
compile_seconds = time.perf_counter() - t0

programs = []
if a.max_len == 50:   # the emitted dirs are content-addressed: name == sha256(model.mil + weights.bin)[:24]
    for line in open(a.keys).read().split("\n"):
        if not line.strip():
            continue
        prog, key = line.split()
        d = cache_root() / key
        assert sha256(d / "model.mil", d / "weights.bin")[:24] == key, f"{prog}: emitted program differs"
        bundles = sorted(glob.glob(str(d / "cache" / "*" / "*" / "*" / "*.bundle" / "*.bundle")))
        programs.append({"prog": prog, "key": key, "e5_bundles": [os.path.relpath(b, d) for b in bundles],
                         "anehash": [open(h).read().strip() for b in bundles
                                     for h in glob.glob(b + "/main/main_ane/model.anehash")]})
    assert len(programs) == len(m._dec["chunks"]) == 38, "program count differs from the staged manifest"

capture, forced_ids = [], None
original = m._decode_logits


def decode_logits(hidden, use_ane, greedy):
    logits = np.asarray(original(hidden, use_ane, greedy), dtype=np.float32).copy()
    capture.append(logits)
    if forced_ids is None:
        return logits
    steered = logits.copy()
    steered[forced_ids[len(capture) - 1]] = np.inf   # greedy argmax takes the forced id
    return steered


m._decode_logits = decode_logits


def run(p, forced=None):
    global forced_ids
    capture.clear()
    forced_ids = forced
    t = time.perf_counter()
    gen = m.generate(p["prompt_token_ids"], max_new_tokens=a.new_tokens, max_len=max_len(p), temperature=0.0,
                     top_p=1.0, top_k=0, batched_prefill=False)
    return [int(g) for g in gen], np.stack(capture), time.perf_counter() - t


for _ in range(a.warmup):
    run(prompts[0])
records, logits, forced = [], {}, {}
for n, p in enumerate(prompts):
    gen, lg, seconds = run(p)
    ref_ids = p["runs"][0]["generated_ids"][: a.new_tokens]
    records.append({"id": p["id"], "category": p.get("category"), "prompt": p.get("prompt"),
                    "prompt_token_ids": p["prompt_token_ids"], "max_len": max_len(p),
                    "runs": [{"generated_ids": gen, "elapsed_seconds": seconds, "reference_equal": gen == ref_ids}]})
    logits[f"prompt_{n:03d}"] = lg
    print(f"{p['id']} M={max_len(p)} {seconds:.2f}s equal_ref={gen == ref_ids} {gen}", flush=True)
    if a.forced:
        gen_f, lg_f, _ = run(p, ref_ids)
        assert gen_f == ref_ids, "forcing failed"
        forced[f"prompt_{n:03d}"] = lg_f
m._decode_logits = original
m.release()

np.savez_compressed(a.out + ".npz", **logits)
if a.forced:
    np.savez_compressed(a.out + "-forced.npz", **forced)
meta = {
    "runner_sha256": sha256(os.path.abspath(__file__)), "argv": sys.argv, "started_utc": started, "ended_utc": utc(),
    "compile_path": "native: ANEForge E5RT.compile -> e5rt -> ANE compiler service of this macOS (no target option)",
    "compile_seconds_first_decoder": compile_seconds, "warmup_runs": a.warmup, "max_len_mode": a.max_len or "per prompt",
    "aneforge_path": os.environ["ANEFORGE_PATH"], "aneforge_cache_dir": str(cache_root()),
    "python": sys.version, "numpy": np.__version__, "platform": platform.platform(),
    "host": open(a.env).read() if a.env else None, "programs": programs,
    "model_sha256": ref["model_sha256"], "reference_json": a.ref, "reference_json_sha256": sha256(a.ref),
    "head": "float32 dequantized GGUF token_embd @ float32(h after program 37) (ANEForge host lm_head)",
}
json.dump({"metadata": meta, "max_new_tokens": a.new_tokens, "prompt_count": len(records), "prompts": records},
          open(a.out + ".json", "w"), indent=1)
print(f"done: {sum(r['runs'][0]['reference_equal'] for r in records)}/{len(records)} prompts equal the reference; "
      f"compile {compile_seconds:.1f}s; {a.out}.json")
