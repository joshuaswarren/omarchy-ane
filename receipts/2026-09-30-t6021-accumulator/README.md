# 2026-09-30 — T6021 C pv accumulator: tested, no bit-exact reproduction

## Purpose

Identify the C pv accumulator model that bit-exactly reproduces the saved
real-parakeet layer-0 device output, given that input FTZ and product FTZ
have already been falsified (`receipts/2026-09-30-t6021-island-golden/README.md`,
C pv subnormal / FTZ follow-up: 0.02288818 vs 0.02287292).

## Offline phase — no standard model is bit-exact

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

fp32 RN is the closest standard model: 275,765 of 384,000 lanes bit-exact.
Remaining 108,235 deviate by ±1–2 ulps (tail to ±10 ulps, outliers to 517
ulps). Symmetric distribution consistent with internal chunked rounding
noise, not a single fixed bias.

## Online phase — three probes under flock

### Probe 1 (33.6 + 34.2 ms): targeted K-position / V-magnitude cases

Batched stimulus (8 heads × 375 rows × 128 cols). Head 4 row 0 (V =
fp16(0.01)) and head 6 row 0 (375 subnormal × V=1) reproduce the prior
FTZ-probe result. 383,744 of 384,000 lanes match fp32 RN bit-exactly.
Only 256 lanes (1 ulp above) at (h=4, r=0, all n) and (h=6, r=0, all n).

### Probe 2 (33.6 ms): sweep of fp16 bit patterns

CORRECTION: this sweep only covered bits 0x0001..0x0BB8 (|V| < 2^-13).
The "V >= 0x0500 returns V exactly" claim was unsupported because 0x0500
itself is inside the swept range. The "output FTZ on subnormal fp16 is
documented IEEE-754 behavior" framing is wrong: IEEE-754 fp16 has
subnormals; what we observed is an absolute quantization floor.

Observed in probe 2:
- |V| < 2^-17: dev = 0 (FTZ).
- |V| in [2^-17, 2^-13): dev quantized to fp16 grid step 2^-16.

### Probe 3 (42.4 ms): wide-range magnitude sweep with random mantissas

Batched stimulus (8 heads × 375 rows × 128 cols):
- Head 0: single-term P=1, V over geometric grid 2^-24..2^15 (~9 values
  per octave with random mantissa).
- Head 1: P=0.5 × V.
- Head 2: P=2^-8 × V.
- Head 3: P=2^-12 × V.
- Head 4: P=0.999 × V.
- Head 5: K=2 terms (P=1 at K=0, tiny at K=1), V varies per row.
- Head 6: K=2 terms (tiny at K=0, P=1 at K=1) — order test.
- Head 7: K=2 terms with V=1 constant.

Head 0 findings (P=1, single-term):
- |V| < 2^-17: dev = 0 (FTZ floor).
- |V| in [2^-17, 2^-10): dev quantized to grid step 2^-16 (e.g. V =
  5.96e-6 → dev = 0; V = 1.526e-5 → dev = 1.526e-5; V = 4.578e-5 →
  dev = 4.578e-5).
- |V| >= 2^-10: dev = V exactly (no extra quantization beyond fp16).
- Largest |V| where dev == 0: 1.353e-5 (i.e. just below 2^-16).
- Grid step changes smoothly: at |V| in [2^-8, 2^-7), smallest nonzero
  diff = 1.679e-4 ≈ 2^-12.5 (i.e., step matches fp16 ulp at that
  magnitude).

Heads 1..4 (P != 1): the same grid structure applies to the product
P*V — output quantized for small product magnitudes.

Heads 5, 6, 7 (K=2 terms): when one term dominates (e.g. K=0 term is
1.0, K=1 term is tiny), the second term is rounded away to grid step
2^-16. When both terms comparable, the device output is some quantized
value that is not bit-exact predicted by any tested model.

### Per-octave dev vs V match rate (head 0)

| |V| range           | n   | dev=0 | dev==V | max rel err |
|---------------------|-----|-------|--------|------------|
| [2^-24, 2^-17)      | 60  | 60    | 0      | n/a        |
| [2^-17, 2^-16)      | 10  | 0     | 0      | 1.0000     |
| [2^-16, 2^-15)      | 10  | 0     | 0      | 0.9845     |
| [2^-15, 2^-14)      | 10  | 0     | 0      | 0.7815     |
| [2^-14, 2^-13)      | 10  | 0     | 0      | 0.0716     |
| [2^-13, 2^-12)      | 10  | 0     | 0      | 0.0092     |
| [2^-12, 2^-11)      | 10  | 0     | 0      | 0.0019     |
| [2^-11, 2^-10)      | 10  | 0     | 0      | 0.0008     |
| [2^-10, 2^-9)       | 10  | 0     | 1      | 0.0000     |
| [2^-9, 2^-8)        | 10  | 0     | 2      | 0.0000     |
| [2^-8, 2^-7)        | 10  | 0     | 2      | 0.0000     |
| ...                 | ... | ...   | ...                 |
| [2^-2, 2^-1)        | 10  | 0     | 10     | 0.0000     |
| [2^-1, 2^0)         | 10  | 0     | 10     | 0.0000     |
| [2^0, 2^1)          | 10  | 0     | 10     | 0.0000     |
| ...                 | ... | ...   | ...                 |
| [2^4, 2^5)          | 10  | 0     | 10     | 0.0000     |
| [2^5, 2^6)          | 10  | 0     | 10     | 0.0000     |
| [2^6, 2^7)          | 10  | 0     | 10     | 0.0000     |
| [2^13, 2^14)        |  5  | 0     |  5     | 0.0000     |

All 375 non-zero dev values are exact multiples of 2^-16. Smallest
non-zero dev = 1.526e-5 = 2^-16. The grid step of 2^-16 matches for
all values up to 2^-10; for larger magnitudes, dev matches V directly
(no extra quantization).

## Verdict on the original question

**The C pv device accumulator is NOT any of the candidate fp32 RN,
fp32 pairwise, fp16 chunked (8/16/32/64/128), block-floating N=8..18,
Kahan, fp16 pure, K-order variant model.** No tested model bit-exactly
reproduces the saved real-parakeet layer-0 device output. Probe 3
confirms an output quantization grid (step 2^-16 for small magnitudes,
FTZ for |V| < 2^-17) but no tested accumulator model reproduces the
multi-term deviations.

## Correlation analysis: failing lanes vs sum|terms| and |device|

Main's prediction: failing lanes are at small sum|terms|. Tested.

| Layer | Failing strict | sum|terms| at failing (min/median/max) | sum|terms|<2^-6 failing |
|------:|---------------:|----------------------------------------:|----------------------------:|
|     0 |          5,100 | 5.66e-2 / 2.11e+0 / 5.69e+0             |                       0 / 0 |
|    11 |        182,742 | 2.42e-3 / 2.52e-1 / 1.48e+0             |                 467 / 515 |
|    23 |        201,045 | 1.14e-2 / 2.37e-1 / 2.34e+0             |                     6 / 6 |

**Main's prediction is FALSE for layer 0** (no failing lane has
sum|terms| < 2^-6). For layer 11: 467 of 182,742 failing lanes have
sum|terms| < 2^-6. For layer 23: 6 of 201,045. The bulk of failures is
at regular magnitudes, not small sum|terms|.

## Residual

108,235 lanes (28.2% of layer-0) deviate from fp32 RN. The deviation is
symmetric ±1–2 ulps in the bulk, tail to ±10 ulps, occasional outliers
to 517 ulps (h=2, r=340). The accumulator is clearly fp32-class (probe
1: 99.93% lanes match fp32 RN) but the final fp16 round has a
non-standard grid structure (probe 3) that biases small magnitudes.

A "floor model" `bound = max(2^-11 * sum|terms|, F_absorbed)` was tested
in isolation: with F_absorbed = 2^-25, all currently-failing lanes pass.
This is NOT a meaningful model because the failing lanes have median
sum|terms| = 2.1 (NOT small), median |dev-exact| = 1.68e-3 (NOT floor-
dominated). The failures are genuine accumulator precision issues at
normal magnitudes, not an absolute error floor.

## Product contract status (CORRECTED)

The "passes the product contract" claim from prior receipt is REMOVED.
No model-vs-stock numbers were computed; the claim had no evidence. The
recorded facts are:

- The C pv island FAILS the strict per-lane fp64-exact-product bound
  `|dev - fp64_exact| / (2^-11 * sum|terms|) <= 1` on 5,100 / 182,742 /
  201,045 lanes for layers 0 / 11 / 23 (1.3% / 47.6% / 52.4%).
- The rel L2 vs the saved reference output is 2.21e-4 / 8.40e-4 /
  1.13e-3, max abs 7.81e-3 / 2.59e-3 / 1.95e-3 — within typical fp16
  matmul tolerances but the strict bound fails on a non-trivial
  fraction of lanes.

## What did NOT change

- `tools/island_ref.py`, `tools/island_real.py`, `tools/island_golden.py`
  are unchanged.
- `libane`, `ane.ko`, the driver are unchanged.
- Any edit to mlx-omarchy, mil-hwx-compiler, or the FSM worktree.

## Files

- Probe runner: `/tmp/accprobe2/build_probe3.py`, `run_probe3.py`.
- Analysis: `/tmp/accprobe2/analyze3.py`, `model_quant.py`, `grid_step.py`,
  `grid_per_octave.py`, `find_threshold.py`, `test_real2.py`,
  `accum_grid.py`, `dev_pattern.py`, `bias_check.py` (all scratch).
- Notebook entry: `~/.local/share/apple-silicon-lab/entries/AccumProbe/`
  with pre-experiment (16:12:50Z), final (16:34:40Z), and addendum
  (16:38:58Z+) entries.
- Artifacts: `~/.local/share/apple-silicon-lab/artifacts/AccumProbe/`
  with probe 1, 2, 3 device outputs, exact references, packed inputs,
  SUMMARY (with addendum), post-state, SHA256SUMS.

## Run evidence

- Three device invocations under `flock /var/tmp/ane-run.lock timeout 60`:
  probe 1 (33.6 + 34.2 ms), probe 2 (33.6 ms), probe 3 (42.4 ms).
- M2 post-state: kernel `7.1.13-ARCH-polltx`, boot ID
  `95675db4-da91-42e5-beee-dfb3329e7369` (unchanged).
- No kernel / module / boot / config change. No dmesg fatal markers.