# Staged Qwen3.8-2B greedy decode on the M2 ANE (STAGED-QWEN-REF)

Date: 2026-10-01 (UTC). Branch: agent/qwen-m2-decode. Host: the M2 (T6021),
stock 7.1.13-3-1-ARCH, boot 0e2c3743, module `ane_t6021` a584a967 (BO cap
12 GiB), ane-run e7986b48 (cb5a8f1). All runs below used one boot.

**Result: STAGED-QWEN-REF FAIL, 3 of 10 prompts.** The M2 runs all 38
programs per token end to end, and every host-built input equals the M1 bit
for bit. p002, p005 and p009 produce all 32 reference tokens. The other seven
prompts diverge, and each divergence is at a token where the reference itself
has a top1-top2 logit margin of 0.008 to 0.343. While the tokens agree, the
M2 logits differ from the reference logits by a median rel L2 of 0.071 (max
abs median 0.88). The reference is the bit-exact output of the H13 compile;
the M2 runs the H14 compile, so this gate cannot pass without bit-exact
numerics (see "Why the tokens diverge").

## Tool

`tools/qwen_m2_decode.py` runs one greedy decode per prompt. It repeats the
host work of `tools/staged-qwen/staged_qwen_runner.py`, the M1 Linux runner
that passed 10/10:

- the fp16 embedding row from the GGUF `token_embd` (gguf-py dequantize);
- the oh/inv/mask/cosp/sinp context tables (rope base and dimensions from the
  GGUF metadata);
- the 38 programs in manifest order, with lanes keyed by name, and every
  resident state chained on the host (a state output becomes the next step's
  state input, and all states are zero at the start of each prompt);
- float32 logits = dequantized `token_embd` @ h after program 37, and argmax,
  for positions >= len(prompt) - 1.

Each program call is `flock /var/tmp/ane-run.lock timeout 120 ane-run --ports`
through `qwen_prog_run.ane_call`, with the resolved port tables of the
conformance run. The tool refuses a port table that does not hold exactly the
manifest's ports. `--dump` compares every port with the M1 per-step dump.
`--ref-logits` compares the logit vector with the reference logits while the
generated prefix still equals the reference. `tests/test_qwen_m2_decode.py`
checks the context tables against the dump's SHA-256 at steps 0 and 11, the
lane and state chaining, and the token helpers (3 passed).
`reference-tokens.json` holds the frozen prompt ids and greedy tokens from
`chunk_00.json` (sha256 8269c36b…, contract GGUF 4aa0fb13…).

## Host tables and the step-0 proof

On p001 the tool compared every port of every program at the dump's steps
(0, 1, 2, 11, 12, 13) with the M1 execution. All host-built inputs are
bit-exact: 79 at step 0 (the program-0 embedding of token 15666, the
oh/inv/mask/cosp/sinp tables of the six attention programs, all 48 zero
states) and 31 at each later step. Steps 11, 12 and 13 produce 271, 248068,
271, the dump's generated ids, and the M2 h at those steps gives logits within
rel L2 0.063-0.090 of the reference.

The outputs do not stay within the conformance threshold max(0.02, 2 b) once
the programs are chained. At step 0, 12 of 192 output ports exceed it (worst
prog_035 x at 0.0360 against 0.02, prog_037 h at 0.0315), and 474 output
verdicts fail over the six steps; the final h is 0.0315, 0.0467, 0.0504,
0.0855, 0.122, 0.0636 rel L2 from the M1. That threshold bounds one program on
the M1's own inputs, and the drift comes from amplification along the chain:
at step 0, prog_002 receives o at 0.0021 from the M1 and returns x at 0.0191.

A float64 evaluation of the same 38 MIL programs, chained on the same inputs
(`tools/mil_eval.py` run_mil, fp16 constants, no rounding), shows that both
chips are far from the exact chain and about equally far. Median rel L2 of the
192 output ports from the fp64 chain:

| step | M1 | M2 | M2 vs M1 | ports where the M2 is closer to fp64 |
|---|---|---|---|---|
| 0 | 0.1188 | 0.1185 | 0.0099 | 145 |
| 1 | 0.3933 | 0.3907 | 0.0267 | 159 |
| 2 | 0.3447 | 0.3476 | 0.0293 | 144 |
| 11 | 0.2705 | 0.2684 | 0.0508 | 81 |
| 12 | 0.3116 | 0.3128 | 0.0525 | 129 |
| 13 | 0.2338 | 0.2352 | 0.0324 | 108 |

The fp64 chain's greedy tokens equal the reference for p001 gen 0-12.

## STAGED-QWEN-REF: 10 prompts x 32 greedy tokens

One process, 446 steps (126 prefill, 320 generated), 16,948 ane-run calls,
all exit 0, 1,965 s wall. No new kernel line other than `[UFW BLOCK]`.
Per prompt ([prompts.tsv](prompts.tsv); every step in [steps.tsv](steps.tsv)):

| prompt | 32/32 | first divergence | M2 token (margin) | reference token (reference margin) | M2 vs reference logit max abs there |
|---|---|---|---|---|---|
| p001 | no | 12 | 35003 (0.3249) | 11247 (0.1208) | 1.373 |
| p002 | yes | | | | |
| p003 | no | 22 | 8964 (0.1683) | 35003 (0.0082) | 1.226 |
| p004 | no | 29 | 561 (0.0121) | 8964 (0.2235) | 1.192 |
| p005 | yes | | | | |
| p006 | no | 13 | 561 (0.0404) | 12996 (0.3432) | 0.751 |
| p007 | no | 23 | 10838 (0.1521) | 220 (0.0568) | 0.768 |
| p008 | no | 23 | 8396 (0.0728) | 13660 (0.0188) | 1.053 |
| p009 | yes | | | | |
| p010 | no | 18 | 13962 (0.2265) | 332 (0.2172) | 1.428 |

Margins are top1 - top2 of the float32 logits. At each divergence, the M2's
top2 is the reference token. Over the 236 generated tokens where the M2 and
the reference agree with the same prefix, |M2 margin - reference margin| has
median 0.163, p90 0.449 and max 1.486; 23 of those tokens have a reference
margin below 0.5 and still agree. p001 gave the same 32 tokens in three runs,
and the M2 arrays of p001 steps 0-13 were byte-identical in two runs (2,497
files).

## Why the tokens diverge

- The reference (`chunk_00.json`) and the M1 dump are one deterministic path:
  the dump's h at steps 11-13 reproduces the reference logits exactly with
  this tool's head. The M1 Linux runner passed 10/10 because it replays the
  same H13 programs bit for bit.
- The M2 runs the H14 compile. Program by program it is within 0.007 rel L2 of
  the M1, and the 18 DeltaNet programs are bit-exact. Chained over 38 programs
  and the resident states, the differences grow about tenfold (step 0: at most
  0.0025 per program, 0.0315 at the final h). The M1's own fp16 error grows the
  same way (b medians 0.0007-0.057 per program; 0.119 median port and 0.346 at
  h from the fp64 chain at step 0), so the M2 logits land a median 0.071 rel L2
  from the reference.
- Every divergence is at a reference margin of 0.008-0.343, below the p90
  margin error of 0.449. These are precision flips at near ties, not lane or
  state errors: a wiring error would not keep the host tables exact and the
  logits at a median rel L2 of 0.071 (max 0.297) from the reference over 243
  generated tokens.
- At p001 gen 12 the fp64 chain prefers the reference token 11247 over 35003
  by 1.049. The M1 keeps that order with 0.121; the M2 reverses it by 0.325.

The max_len 513 export showed the same effect on the M1 itself
(ane-linux-experiments receipt 2026-09-25-qwen-ane-export-513): its new compile
scored 2/10 against this reference, and the gate there became a reference from
Apple's runtime on that compile. For the M2, that is the H14 programs run by
macOS e5rt on the M2.

## Speed

The subprocess path took 1,965 s for the 10 prompts, under the 90 min trigger,
so the persistent worker was not built. Median step wall: 3.88 s of ane-run
calls, plus 0.668 s for the host head on generated steps (4.567 s), which is
0.219 tokens/s. The engine time is 176 ms of each step; the rest is per-process
setup, which grows with the program's constants:

| programs | ane-run wall median | p95 | exec median |
|---|---|---|---|
| 18 DeltaNet | 8.0 ms | 10.8 ms | 1.672 ms |
| 0, 21 (group start) | 53.0 ms | 72.3 ms | 3.110 ms |
| 11 readout + MLP + next block | 154.9 ms | 188.4 ms | 6.467 ms |
| 6, 12, 18, 25, 31 (attention) | 280.1 ms | 310.9 ms | 10.818 ms |
| 20 (group end) | 113.9 ms | 143.8 ms | 4.940 ms |
| 37 (group end) | 245.0 ms | 271.6 ms | 9.204 ms |

The host head (248,320 x 2,048 float32 matvec) runs on one core with the
system's reference cblas (numpy 2.5.3), so it adds little CPU load next to the
GPU work on the same host.

## Reproduce (on the M2)

```sh
cd tools && PYTHONPATH=<gguf-py 0.19.0> nice -n 10 python3 qwen_m2_decode.py \
  --gguf Qwen3.8-2B-Q4_K_M.gguf --manifest manifest.json \
  --ref ../receipts/2026-10-01-t6021-qwen-decode/reference-tokens.json \
  --anec-dir /var/tmp/qwen-real-anec-h14 --ports-dir <qwen_m2_conform work dir> \
  --ane-run tools/ane-run --ref-logits chunk_00.npz \
  --dump /var/tmp/qwen38-step-goldens --band <mil_eval band jsonl> --out <dir>
python3 qwen_m2_decode.py --out <dir> --summary
```

Inputs: GGUF 4aa0fb13…, manifest.json 808d676d…, chunk_00.npz d7dd5f7f…
(the reference logits), reference-tokens.json 0fed31c8…. Hashes of the tables
in this directory: [SHA256SUMS](SHA256SUMS).

Not covered: a reference produced on the M2 by Apple's runtime, an
instrumented M1 run at a divergence step (the dump covers steps 0-2 and 11-13
only), and more than one boot.
