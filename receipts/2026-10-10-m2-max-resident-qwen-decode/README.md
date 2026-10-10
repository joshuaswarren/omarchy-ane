# M2 Max (T6021): resident Qwen3.8-2B decode on module bd34a12, 2026-10-10

This is one run on one boot. The raw files are in this directory.

## Setup
- Chip: Apple M2 Max (T6021).
- Kernel: 7.1.12-2-12.6-sep-ARCH.
- Driver module: `ane_t6021` build `bd34a12`, srcversion C133FDFAB7C937204AE2186. The module file sha256 is f068aaf1a2f86b28c8ffe53237347945752b6245660466e6fc1480834e3528f5.
- Allocator: `bo_total_bytes` was 0 at the start, after a graceful reboot.
- Nothing else ran on the ANE or the GPU during the run.
- `ane-run` has sha256 a3e9ce81fe83587aeebb95b8733ce6bcac00e5343fecd30a0a16f87409b0e66f.
- `ane-session` has sha256 af1828e893fe2938e244b8c5e5f891ce25481660a17a48b8f02c07b88fe34b3c.
- Both came from the signed commit bd34a1261db417e79e46595ed8b77954b41f4eb4. They were built with gcc 16.1.1.
- Decoder: `tools/qwen_m2_decode.py` and `tools/qwen_prog_run.py` from the resident-decoder branch (git blob ids f27b3f9e45 and 3f60fd6182). They are the files in PR 144.
- Program set: 38 programs for Qwen3.8-2B, compiled for the H14 engine.

## Command
The decoder was started with these arguments (`run1-results.jsonl`, first record). The paths are on the lab machine.

```
--manifest /var/tmp/qwen-decode/manifest.json --anec-dir /var/tmp/qwen-real-anec-h14 --gguf /var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf --ref /var/tmp/qwen-decode/reference-tokens.json --ports-dir /var/tmp/qwen-conform-0e2c3743-r2 --ane-run /var/tmp/w73-lookup/hybrid/tools/ane-run --prompts p001 --new-tokens 16 --out /var/tmp/m2slot/qwenres-only-20261010T220414Z/run1 --logits-file /var/tmp/m2slot/qwenres-only-20261010T220414Z/run1/logits.f32 --resident --session-bin /var/tmp/w73-lookup/hybrid/tools/ane-session --resident-lock call
```

The run started after a 60 s idle. It used one decoder process. It used one `ane-session` process.

## Result
- The decode finished with exit code 0 in 35.0 s. It ran 27 steps: 11 prefill steps and 16 generated tokens.
- The sha256 of the logits file is 7384c10e4d21db0246ff853a0afbe03669de84c315af211388fa2c256ab83ea3. It equals the sha256 of the per-call baseline run.
- Decode step wall time: p10 0.819 s, p50 0.826 s, p90 0.835 s. That is 1.2118 tokens per second. The ANE part of a step was p50 0.163 s.
- `bo_total_bytes` after the run was 2,805,055,488. The 38 unique program sections take 2,754,388,032 of that.
- A memory probe after the run found a largest free window of 240 MiB.
- Before and after the run, no dmesg line matched these patterns. The patterns were a fence wait, a hung task, an `ane_t6021` fault or error, `EXCH`, `DART fault` and `quarantin`.

## Earlier runs
On module build `ca09ce8` the same decode gave a step p50 of 0.823 s. That is 1.2129 tokens per second. The logits hash was the same. The difference is 0.4 percent between two single runs. It is not a speed claim.

The per-call path on module `37ffb57` took 6.398 s per step (p50) on another boot. The resident step is about 7.7 times faster than that run.

## Limits
- This is one run, with one prompt and 16 tokens.
- The logits match the per-call baseline.
- In the longer run on `ca09ce8`, the greedy tokens match the reference on 3 of 10 prompts.
- The other 7 prompts diverge at the same steps as the per-call run.
- The 7.7 times figure compares single runs. They ran on different boots and module builds.
- The decoder code is in PR 144 and was not on main when this run took place.
