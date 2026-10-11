# M2 Max (T6021): resident Qwen3.8-2B decode with numpy on OpenBLAS, 2026-10-10

Two runs on one boot. The raw files are in this directory. The decoder, the module and the programs are the ones of [2026-10-10-m2-max-resident-qwen-decode](../2026-10-10-m2-max-resident-qwen-decode/README.md). Only the system BLAS that numpy links changed.

## What changed
On Arch the `blas`, `cblas` and `lapack` packages are the reference implementation. numpy linked `/usr/lib/libcblas.so.3`, which pointed at `libcblas.so.3.12.0`. Its matvec runs on one thread.

On 2026-10-10 at 23:49Z `sudo pacman -S blas-openblas` replaced the three packages with `blas-openblas` 0.3.34-1. The package `openblas` 0.3.34-1 was already installed. `libcblas.so.3` now points at `libopenblas.so.0.3`. Package states before and after are in `packages/m2-max.txt`.

No file of the decoder changed. `tools/qwen_m2_decode.py` has git blob id f27b3f9e45 and `tools/qwen_prog_run.py` has 3f60fd6182, the same as in the earlier receipt.

## Setup
- Chip: Apple M2 Max (T6021), 12 cores.
- Kernel: 7.1.12-2-12.6-sep-ARCH. Boot id starts e12304e0.
- Driver module: `ane_t6021` build `bd34a12`, srcversion C133FDFAB7C937204AE2186. This is the module of the earlier receipt.
- `ane-run` sha256 a3e9ce81fe83587aeebb95b8733ce6bcac00e5343fecd30a0a16f87409b0e66f.
- `ane-session` sha256 af1828e893fe2938e244b8c5e5f891ce25481660a17a48b8f02c07b88fe34b3c. Both binaries are the files of the earlier receipt.
- numpy 2.5.3 (`python-numpy` 2.5.3-1).
- Allocator: `bo_total_bytes` was 7,127,040 at the start. A few small programs had run since the reboot.
- Nothing else ran on the ANE, the GPU or the CPU during the runs. The M2 idle guard was paused for them.
- Program set: 38 programs for Qwen3.8-2B, compiled for the H14 engine.

## Command
The arguments are the same as in the earlier receipt (`run1/results.jsonl`, first record). Run 1 came from `qwen-resident-only-ticket.sh`: 60 s idle, then one decode process. Run 2 used the same decoder arguments in a new process on the same boot, started right after run 1.

```
--manifest /var/tmp/qwen-decode/manifest.json --anec-dir /var/tmp/qwen-real-anec-h14 --gguf /var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf --ref /var/tmp/qwen-decode/reference-tokens.json --ports-dir /var/tmp/qwen-conform-0e2c3743-r2 --prompts p001 --new-tokens 16 --resident --resident-lock call
```

## Result
Both runs ended with exit code 0. Each ran 27 steps: 11 prefill steps and 16 generated tokens. The rate is 16 tokens divided by the sum of the 16 step times.

||reference BLAS (earlier receipt)|OpenBLAS run 1|OpenBLAS run 2|
|---|---|---|---|
|generated-token step p10 / p50 / p90|0.819 / 0.826 / 0.835 s|0.196 / 0.211 / 0.234 s|0.206 / 0.226 / 0.246 s|
|tokens per second|1.2118|4.6417|4.483|
|ANE part of a step p50|0.163 s|0.185 s|0.200 s|
|host part of a step (step p50 minus ANE p50)|0.663 s|0.026 s|0.026 s|
|prefill step ANE p50|0.182 s|0.220 s|0.184 s|
|decode wall time|35.0 s|25.8 s|22.8 s|

- The generation step is 3.9 times shorter in run 1 and 3.7 times shorter in run 2.
- The 16 generated tokens equal those of the earlier receipt. They also equal each other in run 1 and run 2 (`logits-compare.txt`). The top-1 margins agree to four decimals.
- As in the earlier receipt, the token list for p001 does not equal the reference token list.
- The float32 logits are not bit-identical to the earlier run. The largest absolute difference is 4.0e-5 on logits of size up to 26, which is 1.5e-6 relative to the largest logit. In total 3,836,967 of 3,973,120 floats differ in the last bits.
- The cause is the summation order. OpenBLAS adds the 2048 terms of each dot product in a different order than the reference BLAS.
- The logits hash is 5c359d366ad883ea911e2bfba5f2e600dc6b22fd34eaad5c12059ef5c35ab37a in both runs. The hash of the earlier receipt, 7384c10e4d21db0246ff853a0afbe03669de84c315af211388fa2c256ab83ea3, does not apply to an OpenBLAS run.
- The 16 MB logits files are not in this directory. Their sha256 is in `results.txt` and `run2/summary.txt`.
- `bo_total_bytes` was 2,811,789,312 after run 1 and 3,258,482,688 after run 2.

## Driver log
After run 2 no dmesg line matched `EXCH.*failed`, `DART.*(fault|error)`, `quarantin` or `BO_INIT`. The line is in `run2/summary.txt`.

For run 1 no dmesg output was saved. Its log check was done in the session and has no committed line, so this receipt makes no driver-log claim for run 1.

## The output projection alone
`head-bench/` times `logits = head @ hidden` with a float32 matrix of 248320 x 2048. The values are random, with the same shape and type, so the memory traffic is the same. Each time is the best of several repeats.

|BLAS and method|time|
|---|---|
|reference BLAS, `OPENBLAS_NUM_THREADS` 1, 4, 8 and 12|604 ms at every setting|
|reference BLAS, float16 head with numpy|786 ms|
|reference BLAS, float16 head chunked to float32|755 ms|
|reference BLAS, 12 Python threads over row chunks|64.6 ms (result identical to the unchunked one)|
|OpenBLAS, `OPENBLAS_NUM_THREADS` 1, 4, 8, 12 and default|20.2 to 21.6 ms|
|OpenBLAS, 2 Python threads over row chunks|23.1 ms|
|OpenBLAS, 4 Python threads over row chunks|24.0 ms|
|OpenBLAS, 8 Python threads over row chunks|24.4 ms|
|OpenBLAS, 12 Python threads over row chunks|28.2 ms|

Chunking over Python threads gives OpenBLAS no gain. The best chunked time is 23.1 ms, which is slower than the 20.2 ms unchunked time.

The generation step is longer than the prefill step because of this projection. Prefill steps skip the logits. In the earlier receipt this matvec took 604 ms of the 663 ms host part.

On the reference BLAS, chunking over threads is faster than the reference alone. It is still slower than OpenBLAS. For that reason only the BLAS change is made.

## Other hosts
The same package swap ran on an M1 Max and an M1 (`packages/`). The matvec takes 21.0 ms on the M1 Max and 47.5 ms on the M1. The M1 had no numpy, so `python-numpy` was installed with it. No decode ran on either machine.

## Limits
- This is two runs on one boot, with one prompt and 16 tokens.
- The ANE part of a step was slower on this boot than in the earlier receipt (0.185 s and 0.200 s against 0.163 s). The cause was not isolated. The host part fell from 0.663 s to 0.026 s in both runs. The step ratio mixes that fall with the ANE difference.
- The logits hash changed, as explained above. The tokens did not change.
- A numpy wheel with its own OpenBLAS (a virtual environment) was not run.
