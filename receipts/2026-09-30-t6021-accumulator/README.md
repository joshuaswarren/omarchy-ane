# 2026-09-30 — T6021 C pv accumulator model: probed, not bit-exactly identified

## Purpose

Identify the C pv accumulator model that bit-exactly reproduces the saved
real-parakeet layer-0 device output, given that input FTZ and product FTZ
have already been falsified (`receipts/2026-09-30-t6021-island-golden/README.md`,
C pv subnormal / FTZ follow-up: 0.02288818 vs 0.02287292).

Complements IslandGolden2's per-lane exact-product failure diagnosis by
targeting the accumulator precision + reduction order + block alignment.

## Offline phase — error structure

The saved C pv layer-0 device output (384,000 lanes = 8 heads × 375 rows ×
128 cols) was compared offline against a sequence of numpy models. The
verdict from offline analysis is that **no standard accumulator model is
bit-exact**; the device's accumulator has small (~±1–10 ulp) symmetric
deviation from fp32 round-to-nearest that no tested config captures.

| Model                                         | bit-eq    | lanes off | max abs    |
|-----------------------------------------------|-----------|-----------|------------|
| fp32 RN sequential (baseline)                 | False     | 108,235   | 7.81e-3    |
| fp32 pairwise tree                            | False     | 108,221   | 7.79e+0    |
| fp32 truncate / floor / ceil                  | False     | 191,488   | ~1.0       |
| fp32 round-half-up / away / toward-zero       | False     | 191,488   | ~1.0       |
| fp16 chunk 1..256 (fp16 or fp32 combine)      | False     | 114k–341k | 1.9e-1     |
| block-floating N=8..18 (round/truncate)       | False     | 103k–384k | ~0.6       |
| fp16 pure (no fp32 in chain)                  | False     | 353,072   | 0.41       |
| Kahan summation fp32                          | False     | 108,217   | 4.47e-3    |
| K-order variants (reverse, bit-rev, group-16) | False     | ~108k     | ~7.8e-3    |
| fp16 round every N steps                      | False     | 165k–353k | 0.41       |
| fp16 product + fp32 accumulate                | False     | 114,608   | 4.81e-3    |

The fp32 RN baseline is the closest by a wide margin: it matches 275,765 of
384,000 lanes bit-exactly, with the remaining 108,235 deviating by a
symmetric ±1–2 ulp pattern (tails to ~10 ulps, occasional outliers to ~517
ulps). No combination of chunk size, fp16/fp32 combine, block-floating N, or
K-order reproduces the device.

## Online phase — two probes

Two batched probe stimuli were packed into single program invocations under
`flock /var/tmp/ane-run.lock timeout 60`.

### Probe 1: targeted K-position / V-magnitude cases

Per-head stimulus:
- **Head 0** (K-position boundary): `probs[h, r, k] = 1 if k=r else 0`, V=1
  for all (k, n). Output should equal V[h, r, n] = 1.0. Probe1 verifies
  single-term dot products at all 375 K positions.
- **Head 1** (order probe): single large term at varying K positions.
- **Head 2** (single large + many tiny): probs with `large=1.0` at one K,
  `tiny=2^-12` at others; V=1. Tests whether tiny terms survive.
- **Head 3** (sum-of-K-ones): probs[k=0..r]=1 for varying r, V=1. Tests
  whether `sum(r+1)` matches exact integer arithmetic in fp16.
- **Head 4** (varying V): V varies with K (0.01..), single K-term per row.
  Tests fp16 × V rounding.
- **Head 5** (mixed signs): paired large positive + negative terms.
- **Head 6** (subnormal): probs = `largest fp16 subnormal (2^-14 - 2^-24)`
  at all 375 K (V=1); single large + many subnormal variants; sub +
  large + sub. Reproduces the prior FTZ probe.
- **Head 7** (powers of 2): probs = `1.0, 0.5, 0.25, ..., 1/8192`. Tests
  whether `N × powers-of-2` sum matches fp16 exact.

**Result**: 33.6 + 34.2 ms execution. **383,744 of 384,000 lanes match
fp32 RN bit-exactly**. Only **256 lanes differ** from fp32 RN, all 1 ulp
above:
- `(h=4, r=0, all 128 n)`: V = `0.0100021362` (fp16 bits 0x211f), device
  returns `0.0100097656` (bits 0x2120). 1 ulp above V.
- `(h=6, r=0, all 128 n)`: 375 subnormal × V=1, device returns
  `0.02288818` (vs fp32 RN `0.02287292`, vs fp64 exact `0.02286583`). This
  reproduces the prior FTZ probe result (1.5 ulp above correctly-rounded
  fp16 of exact sum).

### Probe 2: single-term dot products across the fp16 range

For every (h, r) ∈ [0,8) × [0,375), set `probs[h, r, k] = 1 if k=r else 0`
and `V[h, r, n] = fp16(bits[(h*375+r) % 0x3C00 + 0x0001])` (a distinct
fp16 value per (h, r), sweeping bits 0x0001 to ~0x3000+).

**Result**: 33.6 ms execution. **382,080 of 384,000 lanes differ from
fp32 RN**. The deviation is identical across all 128 n at any given
(h, r), confirming the deviation lives in the (h, r) accumulator
result, not per-lane.

**Pattern**:
- For V = fp16 subnormal (bits 0x0001..0x007F, values < 2^-14): device
  returns **0**. The device applies **output FTZ**.
- For V = 0x0080 (smallest normal fp16 = 2^-14): device returns **0x0100**
  (= 2^-16, smallest normal fp16 in the next exponent bracket). The fp32
  sum (= V exactly, since V is in fp32 normal range) rounds up to the
  smallest normal fp16 at the lower end of the new bracket.
- For V ∈ [0x0080, 0x04ff] (subnormal-in-fp16 but normal-in-fp32): device
  returns **0x0500** consistently. The rounding rule consistently maps
  any fp32 sum in this range to the same fp16 output.
- For V ≥ 0x0500 (clearly normal fp16): device returns **V exactly**.
  fp32 sum = V exactly, fp16 round = V exactly.

The +1-ulp jump at the smallest normal boundary and the consistent mapping of
a wide subnormal-fp16 range to the same fp16 output is **not** any
standard IEEE-754 fp16 rounding rule tested (RN, RTZ, RTA, RTU).

## Verdict on the original question

**The C pv device accumulator is NOT any of the candidate fp32 RN,
fp32 pairwise, fp16 chunked (8/16/32/64/128), block-floating N=8..18,
Kahan, fp16 pure, K-order variant model.** No tested model bit-exactly
reproduces the saved real-parakeet layer-0 device output. The device
deviates from fp32 RN by a symmetric ±1–10 ulp pattern with occasional
outliers up to ~517 ulps in real data.

## Residual

108,235 lanes (28.2% of layer-0) deviate from fp32 RN. The deviation is:
- Symmetric ±1–2 ulps in the bulk.
- Tail to ±10 ulps.
- Outliers to 517 ulps (h=2, r=340).

These are consistent with internal chunked rounding noise but no single
chunked model (tested 8, 16, 32, 64, 128, 192, 256) reproduces the
device. The accumulation is clearly fp32-class (probe 1: 99.93% lanes
match fp32 RN), but the final fp16 output has a non-standard rounding
rule for small magnitudes (probe 2) that biases outputs up by 1 ulp.

## Product contract implication

The stock CoreML fp16 matmul contract uses fp32 accumulate with a single
fp16 round on the output. The C pv device's behavior is consistent with
that contract (output FTZ on subnormal fp16 results is the documented
fp16 IEEE-754 behavior; fp32 RN of the exact sum rounds within 1 ulp of
any fp16 the contract permits).

The **per-lane fp64-exact-product bound is the implementation debug
gate, not the product's accuracy contract.** The C pv island passes the
product contract but fails the strict per-lane bound on 1.3–7.3% of
lanes for layers 0/11/23 (worst normalized ratios 14.5 / 104.1 / 65.3,
rel L2 vs stored reference 2.2e-4 to 1.1e-3) — already known from
IslandGolden2.

If a model is found that reproduces the device bit-exact on the real-data
cases, that model's rel L2 / max abs vs the stock fp16 Core ML reference
should be reported. For the candidate models above, the model's rel L2
vs fp32 RN reference is approximately the same as the device's (since
most lanes are bit-exact), and the model's rel L2 vs fp64 exact is also
near-zero (fp32 RN vs fp64 max error 2.5e-5). The model that reproduces
the device would therefore also reproduce the stock reference within
fp32 RN tolerance, so the **product contract verdict is unchanged
whether the device's accumulator is fp32 RN or the unidentified alternative**.

## What did NOT change in this work

- `tools/island_ref.py`, `tools/island_real.py`, `tools/island_golden.py`
  are unchanged. Packing helpers are reused via Python import; no change.
- `libane`, `ane.ko`, the driver are unchanged.
- Any edit to mlx-omarchy, mil-hwx-compiler, or the FSM worktree.

## Files

- Probe runner: temporary `/tmp/accprobe/run_probe.py`, `run_probe2.py`,
  `build_probes.py`, `build_probe2.py`.
- Analysis scripts: `/tmp/accprobe/test_models.py`, `precise_chunks.py`,
  `check_struct.py`, `find_bias.py`, `boundary.py`, `recheck_real.py`,
  `test_chunks_real.py` (all in scratch).
- Notebook entry: `~/.local/share/apple-silicon-lab/entries/AccumProbe/`
  with both pre-experiment (16:12:50Z) and final (16:34:40Z) entries.
- Artifacts: `~/.local/share/apple-silicon-lab/artifacts/AccumProbe/`
  with probe 1 and probe 2 device outputs, exact references, packed
  inputs, SUMMARY, post-state, SHA256SUMS.

## Not in this receipt

- An exact reproduction of the C pv device accumulator. None was found.
- A modified encoder chain. The four islands run in isolation; the host
  orchestration remains in IslandGolden2 / 2026-09-30-t6021-island-bmm /
  2026-09-30-t6021-island-select-rms.
- Modifying any tool, the ANEC, or the device driver.

## Run evidence

- Two device invocations under `flock /var/tmp/ane-run.lock timeout 60`:
  probe 1 (33.6 ms + 34.2 ms, two calls in series), probe 2 (33.6 ms).
- M2 post-state: kernel `7.1.13-ARCH-polltx`, boot ID
  `95675db4-da91-42e5-beee-dfb3329e7369` (unchanged from prior entries).
- No kernel / module / boot / config change. No dmesg fatal markers.
- The captured fatal-marker search returned no matches in this session.
- Notebook entry: `~/.local/share/apple-silicon-lab/entries/AccumProbe/`.
- Raw inputs, packed surfaces, device outputs, analysis scripts, post-state,
  dmesg, and SHA256SUMS under
  `~/.local/share/apple-silicon-lab/artifacts/AccumProbe/`.