#!/usr/bin/env python3
"""H13 decode of many prompts at a longer context (Q78): the pinned h13_qwen_decode.py machinery (unchanged) with a loop that keeps every step's wall time and exec time, the logits sha256 per prompt, and writes
records after every prompt. Free-running (the device's own greedy ids) or teacher-forced. Logits are saved per prompt as fp32 (steps x 248320).

usage: h13_qwen_decode_long_q6k.py --pkg DIR --lib LIB --manifest JSON --ref PROMPTS.json --out OUT [--prompts N --tokens T --forced-ids JSON --fp32-head]
Q6_K head by default (Q163: 13-inch step 0.1200 to 0.0941 s, logits within 7.6e-6 of the fp32 head); --fp32-head restores the old head (2.03 GB read per token).
"""
import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE / "tools"))
sys.path.insert(0, str(HERE / "research"))
import h13_qwen_decode as base
import numpy as np
import qwen_m2_decode as dec


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pkg", required=True)
    ap.add_argument("--lib", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--ref", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--prompts", type=int, default=50)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--tokens", type=int, default=128)
    ap.add_argument("--forced-ids")
    ap.add_argument("--fp32-head", action="store_true", help="read the head as an fp32 copy (the old default) instead of Q6_K")
    a = ap.parse_args(argv)
    manifest = json.loads(Path(a.manifest).read_text())
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    prompts = json.loads(Path(a.ref).read_text())["prompts"][a.start:a.start + a.prompts]
    forced = json.loads(Path(a.forced_ids).read_text()) if a.forced_ids else {}
    t0 = time.time()
    decoder = base.H13Decoder(a, manifest, a.pkg, a.lib)
    print(f"loaded {len(decoder.prepared)} programs in {time.time() - t0:.1f} s, max_len {decoder.max_len}", flush=True)
    embed, head, (dh, rotary, rope_base) = dec.load_head(a.gguf, q6k=not a.fp32_head)
    cos, sin = dec.rope_tables(decoder.max_len, dh, rotary, rope_base)
    recs = []
    try:
        for prompt in prompts:
            r = base.decode_prompt(decoder, prompt, a.tokens, cos, sin, embed, head, forced.get(prompt["id"]))
            raw = r["logits"].astype(np.float32)
            raw.tofile(out / f"logits-{prompt['id']}.f32")
            rec = {"prompt": prompt["id"], "prompt_len": len(prompt["prompt_token_ids"]), "tokens": r["tokens"], "walls_s": [round(w, 5) for w in r["walls"]],
                   "exec_ms": [round(e, 3) for e in r["exec_ms"]], "finite": bool(np.isfinite(raw).all()), "logits_sha256": hashlib.sha256(raw.tobytes()).hexdigest()}
            recs.append(rec)
            (out / "records.json").write_text(json.dumps({"records": recs}) + "\n")
            w = sorted(r["walls"])
            print(f"{prompt['id']} len {rec['prompt_len']} steps {len(w)} step_p50 {w[len(w) // 2]:.4f} finite {rec['finite']}", flush=True)
    finally:
        decoder.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
