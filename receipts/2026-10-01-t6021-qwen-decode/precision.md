# Qwen decode precision on the M2: why 7 of 10 prompts diverge, and the pre-registered native comparison

Date: 2026-10-01 (UTC). Branch: agent/qwen-precision. Offline analysis on a
build host, CPU only. No ANE run is part of this receipt.

Inputs: the M2 run `ref` of [README.md](README.md) (its results.jsonl, sha256
6c997ccc…, and the M2 mirror dump of p001, 2,497 files, byte-identical to the
hashes recorded on the M2); the M1 per-step dump of p001 at steps 0, 1, 2, 11,
12, 13 (2,499 files, byte-equal to its tarball a7e88df7…); the reference
logits chunk_00.npz (d7dd5f7f…) and tokens chunk_00.json (8269c36b…); the 38
MIL exports (manifest.json 808d676d…); the GGUF (4aa0fb13…).

Tool: `tools/qwen_precision.py`, subcommands `chain`, `decompose`, `gaps`,
`noise` and `gate`. `tools/mil_eval.py` now parses a program once
(`parse_mil`, weights.bin memory-mapped) and runs it many times (`run_mil`).
On programs 0, 1 and 20 its outputs are bit-identical to the old `run_mil`.
The float64 chain of p001 equals the earlier one within 2e-15 at step 0.

## Result

- **3 of 10 is the expected result of the measured noise.** A flip model
  built from the 243 matched-prefix positions of run `ref` expects 2.9 to 3.5
  prompts of 10 to stay equal to the reference (P(N <= 3) = 0.51 to 0.69). It
  ranks p002, p005 and p009, the three that match, among its four most likely
  prompts. Under that noise, 10/10 has a probability of 2.6e-7 to 4.3e-6.
- **The float64 chain does not decide the ties.** It sides with the reference
  at 4 of 7 divergences and with the M2 at 3. Its logits are a median 6.70
  (max abs) from the reference. Its top1-top2 margin error is 14 times the
  M2's (median 1.944 against 0.137 on the same 140 positions).
- **The difference grows along the chain; no single program makes it.** On
  identical inputs, all 216 DeltaNet output ports are bit-exact between the
  chips. The other programs differ by at most 0.0068 rel L2; the attention
  programs have the largest differences. In the chain, the input difference
  pushed through the exact program explains the M2-vs-M1 difference of h
  within 0.0006 at every dump step.
- **The T6021 output grid does not cause the flip.** With the grid on the bmm
  outputs, the p001 gen-12 gap moves from +1.049 to +1.197, away from the M2's
  -0.325. Fp16 rounding and the grid move the chain toward both chips by the
  same amount.
- **The decision rule for the native macOS run is fixed below** (section 5,
  written 2026-10-01T11:55Z, before that run). `qwen_precision.py gate`
  applies it.

## 1. The seven divergences

g = logit(reference token) - logit(M2 token). A = the M1 reference. The fp64
chain runs the 38 programs in float64 on the reference token sequence up to the
divergence (`qwen_precision.py chain`, then `gaps`).

| prompt | gen | A token | M2 token | g in A | g in M2 | g in fp64 | fp64 sides with | fp64 top-2 | fp64 argmax = A on the prefix |
|---|---|---|---|---|---|---|---|---|---|
| p001 | 12 | 11247 | 35003 | +0.1208 | -0.3249 | +1.0493 | A | 11247, 35003 | 12/12 |
| p003 | 22 | 35003 | 8964 | +0.0082 | -0.1683 | +0.6282 | A | 35003, 8964 | 22/22 |
| p004 | 29 | 8964 | 561 | +0.2235 | -0.0121 | -1.2993 | M2 | 561, 2972 | 26/29 |
| p006 | 13 | 12996 | 561 | +0.3432 | -0.0404 | -6.8847 | M2 | 561, 357 | 13/13 |
| p007 | 23 | 220 | 10838 | +0.0568 | -0.1521 | +2.9690 | A | 220, 561 | 19/23 |
| p008 | 23 | 13660 | 8396 | +0.0188 | -0.0728 | -1.5233 | M2 | 8396, 46480 | 13/23 |
| p010 | 18 | 332 | 13962 | +0.2172 | -0.2265 | +2.0675 | A | 33704, 760 | 15/18 |

- In A and in the M2, the two tokens are each other's top-2 at all seven
  divergences. The change of g from A to the M2 is -0.09 to -0.45. The p90 of
  |M2 margin - A margin| on agreeing tokens is 0.444.
- |g in fp64| is below 0.88 (the median max-abs logit difference between the
  M2 and A) at 1 of 7 divergences (p003). It is below the fp64 chain's own
  median margin error (1.944) at 4 of 7 (p001, p003, p004, p008).
- At p004, p006, p008 and p010 the fp64 top-2 does not hold both tokens.
- On the 140 prefix positions before the divergences: |margin - A margin|
  median / p90 / max is 0.137 / 0.441 / 1.486 for the M2 and 1.944 / 5.451 /
  9.037 for fp64. The fp64 logits differ from A by a median max abs of 6.70
  (p90 8.82, max 11.07). The fp64 argmax differs from A at 20 of the 140
  positions.

Both chips have a large common deviation from exact arithmetic (0.12 to 0.83
rel L2 at the ports and at h, table in section 2). They differ from each other
by 0.01 to 0.15. A chain that is 14 times farther from the reference than the
M2 cannot decide gaps of 0.01 to 0.34.

## 2. Error budget by depth (p001)

`qwen_precision.py decompose` runs each program in float64 (f) on the M1 inputs
and on the M2 inputs from the two dumps. Per output port, relative to |M1 out|:
M2 out - M1 out = propagated + rest, propagated = f(M2 in) - f(M1 in),
rest = (M2 out - f(M2 in)) - (M1 out - f(M1 in)). The band is the M1's own
distance from f on its own inputs (rel L2), the basis of the conformance
threshold.

Lane h after each group (rel L2):

| step | prog 20: M2 vs M1 | propagated | band | M1 vs fp64 chain | M2 vs fp64 chain | prog 37: M2 vs M1 | propagated | band | M1 vs fp64 chain | M2 vs fp64 chain |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0.0126 | 0.0124 | 0.0061 | 0.1446 | 0.1430 | 0.0315 | 0.0313 | 0.0071 | 0.3458 | 0.3393 |
| 1 | 0.0811 | 0.0807 | 0.0106 | 0.8259 | 0.8142 | 0.0467 | 0.0469 | 0.0044 | 0.6863 | 0.6796 |
| 2 | 0.0717 | 0.0713 | 0.0111 | 0.5363 | 0.5324 | 0.0504 | 0.0503 | 0.0050 | 0.3002 | 0.3052 |
| 11 | 0.1544 | 0.1542 | 0.0112 | 0.5444 | 0.5386 | 0.0855 | 0.0856 | 0.0042 | 0.3123 | 0.3233 |
| 12 | 0.1386 | 0.1380 | 0.0135 | 0.6494 | 0.6577 | 0.1220 | 0.1226 | 0.0041 | 0.5147 | 0.4713 |
| 13 | 0.0545 | 0.0540 | 0.0126 | 0.3043 | 0.3049 | 0.0636 | 0.0637 | 0.0063 | 0.3692 | 0.3721 |

- The M2-vs-M1 difference of h is 2 to 30 times the band, and 3.5 to 15 times
  smaller than either chip's distance from the fp64 chain.
- First output port above its band: step 0, program 2 (beta 0.0164 > 0.0090);
  steps 1 and 2, program 3 (state0 0.0175 > 0.0012, 0.0128 > 0.0011); steps 11
  to 13, programs 0 and 1 (state0 0.0007 > 0.0003 to 0.0004, the resident
  states). Ports above band: 106, 135, 146, 160, 163, 148 of 192.
- Origin at step 0: program 0 outputs k 0.0005 apart (its own H14-vs-H13
  difference). DeltaNet program 1 is bit-exact on equal inputs and returns o
  0.0021 apart. Program 2 has no difference on equal inputs (x 0.0) and
  returns x 0.0191 apart: a gain of about 9.
- Residual stream x after each readout/attention program, M2 vs M1:

| step | 2 | 4 | 6 | 8 | 10 | 12 | 14 | 16 | 18 | 20 h | 23 | 25 | 27 | 29 | 31 | 33 | 35 | 37 h |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | .019 | .020 | .021 | .022 | .021 | .007 | .007 | .007 | .011 | .013 | .014 | .016 | .019 | .020 | .027 | .032 | .036 | .032 |
| 1 | .007 | .047 | .061 | .070 | .076 | .074 | .080 | .084 | .081 | .081 | .078 | .070 | .069 | .067 | .059 | .055 | .056 | .047 |
| 11 | .009 | .026 | .059 | .072 | .092 | .101 | .117 | .118 | .156 | .154 | .172 | .142 | .135 | .135 | .110 | .110 | .104 | .086 |
| 12 | .010 | .042 | .048 | .067 | .071 | .106 | .112 | .113 | .125 | .139 | .156 | .138 | .133 | .129 | .111 | .117 | .115 | .122 |

  At step 0 the difference rises at program 2 and then changes by a factor of
  0.31 to 1.51 per program. At steps 1 to 13 the largest rise is at program 4
  (a factor of 1.8 to 6.7). Program 4 reads o from DeltaNet program 3, whose
  resident state carries the difference of the earlier tokens (program 3 state0
  is the first port above band at steps 1 and 2). On equal inputs program 4
  differs by at most 0.0028. Every later program changes the difference by a
  factor of 0.79 to 2.25.
- Same inputs (the conformance run, 6 steps), M2 vs M1 rel L2 by class:
  DeltaNet 216 of 216 ports exact; group start median 0 (max 0.0009);
  readout + MLP median 0.00048 (max 0.0046); attention median 0.00063 (max
  0.0068); group end median 0.0006 (max 0.0015).
- The three programs with the largest M2-vs-M1 difference relative to the band:
  - Same inputs: program 21 z (2.15 times: 0.00077 against 0.00036), program
    33 gt (1.86), program 29 gt (0.91); every other program is below 0.86.
    These are differences of about 1 ulp (max abs at most 0.0039 at rms 0.6 to
    2.0). By absolute size: program 6 x 0.0068, program 2 x 0.0046, program 12
    x 0.0036.
  - In the chain: program 21 state0 (216 times: 0.102 against 0.0003), program
    28 state0 (102), program 19 state0 (90), all at step 11. These are resident
    states with small bands. They add up the input difference over tokens. The
    DeltaNet programs are bit-exact on equal inputs, so they carry the
    difference and do not make it.
- Programs 2 and 35, named earlier: program 2 is the first amplifier (gain
  about 9 at step 0, where it has no own difference; at most 0.0046 at other
  steps); program 35 x (0.036 at step 0) is the end of the growth through
  group 2 (own difference 0.0006).

## 3. T6021 output quantization

Rounding models, applied after every MIL op of the chain (`chain
p001:12:<model>`): fp16 rounding; fp16 plus the T6021 grid on bmm outputs (a
matmul of two activations); fp16 plus the grid on every matmul. Grid
([accumulator receipt](../2026-09-30-t6021-accumulator/README.md), probe 3):
below 2^-6 the nearest multiple of 2^-16, ties away from zero.

| model | median port rel L2 to M1 / M2, step 0 | step 12 | h37 to M1 / M2, step 0 | step 12 | ports where the M2 is closer, step 0 | p001 gen 12: g |
|---|---|---|---|---|---|---|
| float64 | 0.1188 / 0.1185 | 0.3116 / 0.3128 | 0.3458 / 0.3393 | 0.5147 / 0.4713 | 145 | +1.0493 |
| fp16 | 0.1188 / 0.1183 | 0.3116 / 0.3127 | 0.3454 / 0.3389 | 0.5145 / 0.4710 | 145 | +1.0496 |
| fp16 + grid on bmm | 0.1161 / 0.1127 | 0.3120 / 0.3114 | 0.3268 / 0.3208 | 0.5091 / 0.4653 | 144 | +1.1967 |
| fp16 + grid on matmul | 0.1170 / 0.1135 | 0.3151 / 0.3149 | 0.3297 / 0.3235 | 0.5173 / 0.4737 | 142 | +1.0918 |

- Fp16 rounding after every op changes the median port distance to either chip
  by at most 0.0014. The chips' 0.12 to 0.39 distance from exact arithmetic is
  not output rounding.
- The grid moves the chain toward both chips by about the same amount (h37 at
  step 0: 0.019 for each). The number of ports where the M2 is closer changes
  by -4 to +2 over the six dump steps. All four chains keep the reference
  tokens at gen 0-11 and 11247 at gen 12.
- The M2 has g = -0.325 at gen 12. The grid models move g up, not down.

Verdict: the divergence agrees with small H14-vs-H13 rounding differences that
the chain amplifies. The largest ones are on the attention programs, which hold
the bmm path. The T6021 grid alone does not reproduce the flip. This receipt
does not measure whether the M1 has the same grid.

## 4. Noise model (`qwen_precision.py noise`)

delta = M2 margin - A margin at the 243 positions with the reference prefix
(at a divergence: M2 gap of the A token - A margin). A token with A margin m
flips when delta < -m. |delta|: p50 0.168, p90 0.445, p95 0.551, p99 0.828,
max 1.486. A's margins over its 320 tokens: p5 0.213, p10 0.317, p25 0.796,
p50 1.931.

| model | E[prompts equal to A] | P(N <= 3) | P(N >= 8) | P(10/10) | expected flips at A margin > 0.84 / 1.2 / 1.5 |
|---|---|---|---|---|---|
| empirical delta | 2.88 | 0.695 | 0.00033 | 2.6e-7 | 0.037 / 0 / 0 |
| symmetrized \|delta\| | 3.49 | 0.513 | 0.0021 | 4.3e-6 | 0.226 / 0.072 / 0 |
| Laplace, scale median\|delta\|/ln 2 = 0.242 | 2.88 | 0.689 | 0.00048 | 5.8e-7 | 0.361 / 0.061 / 0.014 |

P(32/32) per prompt (empirical): p001 0.072, p002 0.422, p003 0.347, p004
0.190, p005 0.726, p006 0.156, p007 0.040, p008 0.236, p009 0.375, p010 0.316.
Observed: p002, p005, p009 match.

## 5. Pre-registered comparison for the native macOS run

Written 2026-10-01T11:55Z, before any number from that run existed.

Runs. A = the M1 reference (H13 compile). L = the Linux M2 run `ref`. M = a
run on the M2 under macOS with Apple's runtime: the same 10 prompts, 32 greedy
tokens each, either the cross-minted h14 HWX (Apple's compiler, target h14,
on macOS 26.6.2) that L's ANEC packages come from, or a native compile of the
same 38 MIL on the M2.

Files M must save:

1. `chunk.json` in the chunk_00.json layout (prompts[i]: id,
   prompt_token_ids, runs[0].generated_ids), in the reference prompt order,
   with the runner's sha256, the macOS build, the compile path (cross-minted HWX or
   native compile, with the compiler build), the warmup count and the UTC
   times.
2. `chunk.npz` in the chunk_00.npz layout: prompt_NNN float32 [32, 248320],
   the logits at each greedy position, as float32 dequantized GGUF token_embd
   @ h after program 37. If the runner computes the head another way, record
   how.
3. The 38 executed HWX, or their sha256 and a comparison with the cross-minted set
   after normalizing the embedded input/output path strings and the
   strings-section length (Apple's compiler output differs only there).
4. A p001 per-program dump at steps 0, 1, 2, 11, 12, 13 in the layout of the
   M1 dump (index.json, stepS/prog_NNN/{in,out}_tX.f16, float16 little-endian,
   the manifest port names), so `qwen_precision.py decompose --dump <A or L
   dump> --m2-dump <M dump>` runs unchanged.
5. The run log with the command lines and wall times.

Requested, not used by the rule: `forced.npz`, the logits with A's 32 tokens
forced as inputs.

Statistics (`gate` computes all except HWX-same and H-same):

- Validity: M's prompt ids equal A's; 32 ids per prompt; finite logits; the
  argmax of each M logit row equals M's id there.
- N(X, Y): prompts whose 32 ids are equal in X and Y. N(L, A) = 3.
- HWX-same: 38 of 38 executed HWX equal the cross-minted set after normalization.
- H-same: M's p001 h (prog_037 out_t72) at steps 11, 12, 13 equals L's bytes:
  - step 11: f5228a050a6102f24db9125f433d294d3e53ce5d1edba3e6b0d2119dfa387987
  - step 12: e51d2dfd8d391e1df0e0aff07d9ab3fc7ad23318cd4bd12f0c203d872e483c28
  - step 13: a8326a12079ba992dd781d9de1c610ae2f76d188cfec4d78c86f301e847f1e3c
- dL: the largest |M logit - L logit| over L's top1 and top2 ids at every
  position where M and L have the same generated prefix (L's values are in
  results.jsonl).
- delta_M: |M margin - A margin| where M and A agree with the same prefix (L:
  median 0.163, p90 0.444 on 236 positions).

Thresholds:

- e_driver = 1e-3 on dL. The float32 head summation order moves the logits by
  at most 5.8e-6 (float32 against float64, and two float32 orders, p001 steps
  11 to 13), and this host reproduces L's recorded top1/top2 values within
  1e-5. A bit-identical h gives dL below 1e-5; 1e-3 allows a few fp16 ulps in
  h and nothing more.
- H_compile at N(M, A) >= 8: a run at L's noise level reaches 8 with P =
  0.00033 / 0.0021 / 0.00048 (section 4).
- G2 divergence bound 1.5: expected noise flips at A margins above 1.5 over
  A's 320 tokens: 0 / 0 / 0.014.

Decision rule, in order:

0. Validity fails: no verdict. Repair the run.
1. **H_driver** if dL <= 1e-3 and every M-vs-L divergence is at an L margin
   <= 2e-3 (with no divergence, N(M, L) = 10). Our Linux driver runs the H14
   programs as Apple's runtime does. L's 3/10 against A is H14-vs-H13
   numerics, not the driver. Record HWX-same and H-same. If N(M, L) = 10 and
   dL > 1e-3: H_driver for tokens only. Find the host-side difference
   (embedding, context tables, state precision, head) with the p001 dumps.
2. **H_compile** if N(M, A) >= 8. With HWX-same: the Linux path runs the same
   programs differently from Apple's runtime, a driver or host defect. Next:
   `decompose` of the M dump against the L dump, to find the first program
   whose output differs on equal inputs. Without HWX-same: the native compile
   follows the H13 numerics. Next: mint the 38 programs with the native
   compiler and run Linux on them.
3. **H_noise** otherwise: M differs from A and from L. With HWX-same: the same
   programs on the same chip disagree between Apple's runtime and our driver.
   The p001 dumps decide: every program bit-exact on equal inputs means a host
   side difference; a program that differs on equal inputs means an execution
   difference. Without HWX-same: the two H14 compiles disagree, so the
   numerics depend on the compile, and G2 is the gate for every compile.
   Report delta_M next to L's 0.163 / 0.444.

Rules 1 and 2 cannot both hold, because N(L, A) = 3.

Gates:

- Strict cross-chip gate (L tokens equal to A, 10/10): stays FAIL. It is not
  a correctness test for any H14 compile: at the measured noise, P(10/10) is
  2.6e-7 to 4.3e-6. It stays a regression test for the H13 path on the M1.
- **G1, the strict gate for the M2:** L tokens equal to M tokens for 10 of 10
  prompts x 32 tokens and dL <= 1e-3 (rule 1). If rule 1 holds, run `ref`
  passes G1 as it is, with no Linux rerun.
- **G2, the cross-compile tolerance gate against A**, frozen from run `ref`:
  (a) each prompt matches A for 32/32, or at its first divergence the two
  tokens are each other's top-2 in A and in the run and g in A < 1.5;
  (b) on the matched-prefix positions, median logit rel L2 against A <= 0.10
  and median |margin - A margin| <= 0.25 (run `ref`: 0.0708 and 0.163);
  (c) N(run, A) >= 1. Run `ref` meets G2 (largest g in A at a divergence:
  0.343). G2's numbers come from run `ref`, so G2 cannot certify run `ref`. It
  is the regression gate for later runs: driver changes, other boots, other
  compiles.

Expected, not part of the rule: rule 1 if M runs the cross-minted HWX, because the
Linux path is deterministic and per-program conformant (456/456, DeltaNet
programs bit-exact against the M1).

## Reproduce

```sh
cd tools
# fp64 chains: 7 diverging prompts up to their divergence, plus the p001 rounding models (one process, about 45 min)
PYTHONPATH=<gguf-py> python3 qwen_precision.py chain --gguf <GGUF> --manifest <manifest.json> --mil-dir <MIL exports> \
  --ref ../receipts/2026-10-01-t6021-qwen-decode/reference-tokens.json --dump <M1 dump> --m2-dump <M2 mirror dump> --out <chains> \
  p001:12:none p003:22:none p004:29:none p006:13:none p007:23:none p008:23:none p010:18:none \
  p001:12:fp16 p001:12:fp16-grid-bmm p001:12:fp16-grid-matmul > chain-ports.jsonl
python3 qwen_precision.py decompose --manifest <manifest.json> --mil-dir <MIL exports> --dump <M1 dump> --m2-dump <M2 mirror dump> > decompose.jsonl
PYTHONPATH=<gguf-py> python3 qwen_precision.py gaps --gguf <GGUF> --ref <reference-tokens.json> \
  --ref-logits chunk_00.npz --chains <chains> --results <run ref results.jsonl>
python3 qwen_precision.py noise --results <run ref results.jsonl> --ref-json chunk_00.json --ref-logits chunk_00.npz
# the morning comparison
python3 qwen_precision.py gate --ref-json chunk_00.json --ref-logits chunk_00.npz \
  --run-json <M chunk.json> --run-logits <M chunk.npz> --linux <run ref results.jsonl> --hwx-same yes|no|unknown
```

`tests/test_qwen_precision.py` checks the grid rule against the probe 3
facts, the Poisson-binomial count, and the gate verdicts on synthetic runs.
As a smoke run, `gate` with A as the native run gives N(run, A) = 10,
N(run, L) = 3, dL = 1.82 and the verdict H_compile.

## Limits

- One M2 boot. The dumps cover p001 at six steps; the M1 dump does not cover
  the divergence steps.
- `rest` in the decomposition is not the H14-vs-H13 difference when a
  program's device error depends on its input: the DeltaNet programs show
  rest up to 0.047 while they are bit-exact across chips. The same-input
  conformance numbers are the H14-vs-H13 difference.
- The grid model comes from single-term probes of the M2 bmm path.
- The margin noise on agreeing tokens uses the run's own top-2, which can be
  another id than A's top-2. The flip model treats the tokens as independent.
