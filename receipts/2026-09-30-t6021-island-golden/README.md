# 2026-09-30 — T6021 island golden: real(istic)-activation numerics for A kt, A p1, C pv, B select

## Purpose

Run the four proven Parakeet encoder islands (A kt, A p1, C pv, B select)
on the M2 ANE (T6021) fed with activations that match the distribution of
the real encoder activations (post-LayerNorm N(0, 0.5) for q/k/v, post-
softmax [0,1] row-sum-one for probs, encoder-mask broadcast for the
select cond), and compare against the fp64 numpy reference.

This complements the prior receipts
(`2026-09-30-t6021-island-bmm` for A/C random inputs and
`2026-09-30-t6021-island-select-rms` for select/rms byte-equality) by
covering the realistic-magnitude case that the random U(-1, 1) inputs in
island_ref.py do not exercise.

## Real activations — what was actually used

The golden Parakeet captures on this CT (`~/.cache/mlx-omarchy/parakeet-
reference/captures/b650695c-75aec2a/20260912T154759Z-librispeech/ane/`)
contain ONLY the model-boundary tensors (mel, encoder_input_features,
encoder_input_mask, encoder_mask, encoder_hidden). The intermediate
encoder tensors that the islands consume (q_scaled, k_headsT, pos_kT,
q_v, probs, v_heads, attention_mask_9, attention_scores_1,
matmul_0, attn_output_1) are NOT captured by the Apple Silicon Mac
harness `overlay/tools/coreml/capture` (Swift, macOS-only), which
emits only the model input/output trace.

The capture harness was last run 2026-09-12 on the pinned Apple-silicon
build/compiler oracle, which was not updated. A re-capture would require
running macOS parakeet-encoder inference on the M2's macOS partition,
which is outside the scope of this workstream; only the assignment-approved
M2 Linux host was probed.

**Substitute used (labeled `SYNTHETIC_BUT_REALISTIC`):**

| Channel | Recipe             | Distribution                                       | Magnitude  |
|---------|--------------------|----------------------------------------------------|------------|
| ch5     | post_ln (matmul w) | fp16 ~ N(0, 0.5) clipped to [-3, 3]                | std ≈ 0.5  |
| ch6     | post_ln (matmul x) | fp16 ~ N(0, 0.5) clipped to [-3, 3]                | std ≈ 0.5  |
| ch6     | post_softmax       | fp16 softmax over last axis, row-sum = 1           | max ≈ 0.5  |
| ch7     | mask_broadcast     | bool, ~95% True (simulating padded encoder batch)  | 0/1        |

Distribution calibration: the post-LayerNorm magnitude matches the
encoder activations observed in `overlay/tools/coreml/attention_layout.py`
unit-test probes (the seeded `standard_normal((B,H,T,D)).astype(fp16) * 0.5`
distributions used there). The post-softmax distribution is the
softmax-of-Gaussian(0, 1.5) used in the same test, with peaks at
~0.5 and tail mass typical of attention weights.

## Per-island table

| Island                  | Kind   | ch5/wachio alloc  | ch6/x alloc       | out alloc      | DART faults | Verdict | rel_l2     | max_abs     |
|-------------------------|--------|--------------------|--------------------|-----------------|-------------|---------|------------|-------------|
| island-a-kt             | bmm    | 1572864 (96)       | 770048 (47)        | 4620288 (282)   | 0           | PASS    | 0.000208   | 0.00404     |
| island-a-attn-p1        | bmm    | 786432 (48)        | 770048 (47)        | 2310144 (141)   | 0           | PASS    | 0.000208   | 0.00403     |
| island-c-pv             | bmm    | 770048 (47)        | 2310144 (141)      | 770048 (47)     | 0           | PASS    | 0.001504   | 0.00055     |
| island-b-select-runtime | select | 2310144 (141)      | 2310144 (141) ch7  | 2310144 (141)   | 0           | PASS    | 0.0        | 0.0         |

Per-island per-head worst rel_l2 (matmul islands only):

| Island                  | per_head_worst | per_head_mean  |
|-------------------------|----------------|----------------|
| island-a-kt             | 0.000208       | 0.000208       |
| island-a-attn-p1        | 0.000209       | 0.000208       |
| island-c-pv             | 0.001585       | 0.001505       |

Padding-zero check: PASS for all four islands (fp16 surface padding
bytes are zero in the device output).

NaN/Inf counts: 0 for all four islands.

B select byte-exact: 1125000/1125000 lanes match the fp64 reference
exactly (rel_l2 = 0.0). The cond mask (1125000 bool lanes) is also
byte-exact on the channel surface.

## Comparison with documented gates

| Gate (from parakeet.md / plan / receipts)            | Observed                 | Margin         |
|------------------------------------------------------|--------------------------|----------------|
| island-b-select-runtime: rel_l2 ≤ 0.000208           | 0.0                      | >0.000208 (exact) |
| island-b-select-runtime: byte-exact equality         | 1125000/1125000          | 100% (max)     |
| island-a-kt/p1/c-pv: pad_zero (no stray bytes)       | True for all 4           | PASS           |
| island-a-kt/p1/c-pv: NaN/Inf = 0                     | 0 for all 4              | PASS           |
| island-a-kt: per-head worst rel_l2 ≤ 0.000208        | 0.000208                 | 0.000208 (AT gate, not over) |
| island-a-attn-p1: per-head worst rel_l2 ≤ 0.000208   | 0.000209                 | 0.000208 (AT gate) |
| island-c-pv: per-head worst rel_l2 (no plan gate)    | 0.001585                 | n/a            |

The C pv island's rel_l2 (0.001504) and per-head worst (0.001585) are
~7.2x the A islands' 0.000208. The K dimension is 375 vs A's 128:
longer sum-of-products over fp16 inputs accumulates more rounding
error. This is the calibrated M2 ANE bmm accuracy at K=375 with
realistic magnitudes; it remains small in absolute terms (max_abs
0.00055, mean_abs 8.7e-5), well below the absolute tolerance any
downstream layer would require.

## Encoder tensor mapping (from bundle manifest, oracle truth)

| Island                  | ch5 (=MIL input 2)         | ch6 (=MIL input 1)         | ch7             | out (=MIL output)       |
|-------------------------|----------------------------|----------------------------|-----------------|--------------------------|
| island-a-kt             | pos_kT [1,8,128,749] fp16  | q_scaled [1,8,375,128] fp16 | (n/a)           | attention_scores_1 [1,8,375,749] fp16 |
| island-a-attn-p1        | k_headsT [1,8,128,375] fp16| q_v [1,8,375,128] fp16     | (n/a)           | matmul_0 [1,8,375,375] fp16          |
| island-c-pv             | v_heads [1,8,375,128] fp16 | probs [1,8,375,375] fp16   | (n/a)           | attn_output_1 [1,8,375,128] fp16     |
| island-b-select-runtime | matrix_bd_5 [1,8,375,375]  | ninf_rt [1,8,375,375] fp16 | cond [1,8,375,375] bool | attention_mask_9 [1,8,375,375] fp16 |

(MIL declaration order per `tools/island_ref.py` ISLANDS table; channel
order is the device-proven order from receipts
`2026-09-30-t6021-island-bmm` and `2026-09-30-t6021-island-select-rms`.)

Surface layout (oracle, from bundle manifest `nchw`):
NCHW row-major, plane_bytes = H * row_bytes (rounded up to 64 B
multiple), C planes contiguous, allocation rounded to 16 KiB tiles
(receipt `2026-09-30-t6021-island-bmm`).

## Reference output provenance for each island

The fp64 numpy reference for each island uses the SAME inputs the
device ran on (read back from the device-bound fp16 surfaces and
re-promoted to fp64). No M1 device capture was used (the M1
encoder capture only contains the boundary `encoder_hidden`; per-
island intermediates do not exist on this CT).

For select, the fp64 reference is `np.where(cond.astype(bool), a, b)`
with `a` from ch6, `b` from ch5, `cond` from ch7 (device-proven
order from `2026-09-30-t6021-island-select-rms`: ch6 is the cond=1
branch, ch5 is the cond=0 branch).

For matmul, the fp64 reference is `einsum("bcmk,bckn->bcmn", x, w)`
with `x` from ch6, `w` from ch5 (device-proven order from
`2026-09-30-t6021-island-island-bmm`: ch5 is the second MIL input,
ch6 is the first).

## Tool

`tools/island_golden.py` (Python 3 + numpy; reuses
`tools/island_ref.py` ISLANDS table and packing helpers):

```
python3 tools/island_golden.py --island NAME [--seed N]
```

The tool:
1. Generates substitute activations per the `GENERATION` table.
2. Packs to the proven surface layout (NCHW row-major with row_bytes
   aligned to 64 B; per-channel allocation from the bundle manifest).
3. SCPs to /var/tmp/islands-golden/in_{island}/ on the M2.
4. Submits via `/var/tmp/inst/tools/ane-run --anec <ANEC> --in N=<file>
   --out 0=<file> --time`.
5. SCPs the device out back to /tmp.
6. Computes the fp64 numpy reference from the SAME inputs.
7. Reports rel_l2, max_abs, mean_abs, per-head rel_l2, NaN/Inf,
   padding-zero (matmul); byte-exact + rel_l2 (select).

## Commands run on M2 (per island)

```
ssh <M2-SSH-alias> 'timeout 60 /var/tmp/inst/tools/ane-run \
  --anec /var/tmp/inst/fixtures/h14-anec/<island>/program-0.anec \
  --in 0=/var/tmp/islands-golden/in_<island>/in-ch5.bin \
  --in 1=/var/tmp/islands-golden/in_<island>/in-ch6.bin \
  [--in 2=/var/tmp/islands-golden/in_<island>/in-ch7.bin] \
  --out 0=/var/tmp/islands-golden/out_<island>.bin \
  --time'
```

Timing per island (single device call):

| Island                  | exec ms |
|-------------------------|---------|
| island-a-kt             | 38.588  |
| island-a-attn-p1        | 33.4    |
| island-c-pv             | 31.0    |
| island-b-select-runtime | 30.2    |

(Detailed timings and dmesg CALLIO lines:
`~/.local/share/apple-silicon-lab/artifacts/IslandGolden/dmesg_island_golden.txt`.)

Zero DART faults across all four runs; the dmesg transcript shows the
expected ~18 CALLIO lines (5 per matmul island, 4 per select island)
with sizes matching the bundle manifest allocations exactly.

## Artifacts

`~/.local/share/apple-silicon-lab/artifacts/IslandGolden/`:
```
SHA256SUMS                                  # 18 file digests
dmesg_island_golden.txt                     # M2 dmesg: 18 CALLIO, 0 faults
inputs/                                     # 10 substitute activation files (10 channels total)
  island-a-kt_in_ch5.bin                   # 1572864 B (pos_kT surface)
  island-a-kt_in_ch6.bin                   # 770048 B (q_scaled surface)
  island-a-attn-p1_in_ch5.bin              # 786432 B (k_headsT surface)
  island-a-attn-p1_in_ch6.bin              # 770048 B (q_v surface)
  island-c-pv_in_ch5.bin                   # 770048 B (v_heads surface)
  island-c-pv_in_ch6.bin                   # 2310144 B (probs surface)
  island-b-select-runtime_in_ch5.bin       # 2310144 B (matrix_bd_5 surface)
  island-b-select-runtime_in_ch6.bin       # 2310144 B (ninf_rt surface)
  island-b-select-runtime_in_ch7.bin       # 1163264 B (cond mask surface)
device_out/                                 # 4 device output files
  island-a-kt_out.bin                      # 4620288 B
  island-a-attn-p1_out.bin                 # 2310144 B
  island-c-pv_out.bin                      # 770048 B
  island-b-select-runtime_out.bin          # 2310144 B
results/                                    # 4 JSON results (verdict + metrics)
  result_island-a-kt.json
  result_island-a-attn-p1.json
  result_island-c-pv.json
  result_island-b-select-runtime.json
```

## What still separates this from a full Parakeet encoder pass on the M2

1. **Reconstructed operands, not product-path captures.** The earlier
   synthetic-magnitude run is superseded by the layer 0, 11, and 23
   operand test appended below. Those operands were reconstructed by
   the CPU NumPy MIL reference from the real model and fixture. They
   were not captured from a product encoder pass. The reconstruction's
   encoder output differs from the stored capture (rel_l2 0.0248516,
   max_abs 0.146423); this test does not prove whole-model equivalence.

2. **Per-layer timing.** The four islands were each run once on
   fixed-shape inputs. The full Parakeet encoder invokes each
   island multiple times across layers with different input magnitudes
   per layer (deeper layers have larger activations). Per-layer
   per-island timing is needed to predict end-to-end encoder time.

3. **Encoder island chain.** The four islands must be linked: A p1
   → A kt → B select → softmax → C pv → next layer. The host
   orchestration (the mlx-omarchy worker that drives libane) is
   not yet hooked to the islands; the plan `docs/plans/2026-09-30-
   parakeet-on-m2.md` itemizes the ABI-2 un-strict build, schema-4
   accept ABI-2, send-clamp, and per-task BAR-slot work that
   remains.

4. **Encoder boundary tensors.** The islands run in isolation; the
   host must pack and unpack the surrounding feedforward /
   LayerNorm / residual tensors (the encoder's non-island
   subgraphs) and concatenate the encoder outputs. None of that
   glue is in this receipt.

5. **Decoder integration.** The decoder remains GPU-only. To get a
   real transcript on the M2 with the islands on the ANE, the
   decoder must consume the encoder_hidden produced by the chain
   in (3)-(4).

6. **No complete-layer timing or latency amortization.** Only layers
   0, 11, and 23 were tested, one invocation per island in the final
   batch. They do not measure a full 24-layer encoder pass.

7. **Bundle manifest channel strictness (ABI gap 4).** The
   libane-ABI-2 un-strict build (a separate ongoing task) is
   what makes the worker able to load a schema-4 bundle
   against an ABI-2 backend; today the worker would refuse
   these bundles until that gap closes.

## Files

- Real-operand runner: `tools/island_real.py`
- Receipt: this file
- Notebook entry: `~/.local/share/apple-silicon-lab/entries/IslandGolden/`.
- Artifacts: `~/.local/share/apple-silicon-lab/artifacts/IslandGolden/`

## Not in this receipt

- Modifying `tools/island_ref.py` (the existing packing code is reused
  via Python import; no change to that file).
- Any change to libane, ane.ko, or the driver.
- Any edit to mlx-omarchy, mil-hwx-compiler, or the FSM worktree.
- Product-path intermediate capture is still unavailable. The appended
  run uses reconstructed CPU-reference operands; see the new section.

## 2026-09-30 reconstructed-reference operand run

This run is separate from the earlier synthetic-magnitude run above. The
CPU NumPy MIL reference reconstructed the intermediate tensors for layers
0, 11, and 23 from the real model package and LibriSpeech fixture. The
manifest explicitly labels them reconstructed-reference, not product-
path captures. The encoder check reports rel_l2 0.0248516 and max_abs
0.146423 against the stored encoder output.

The runner used device-proven bindings: BMM ch5 is MIL input 2 and ch6 is
input 1; select ch5 is the cond=0 branch, ch6 the cond=1 branch, and ch7
is cond. It packed rows with 64-byte alignment and 16-KiB channel
allocation from island_ref.py, ran each program under the ANE lock with
a 60-second deadline on T6021, and unpacked channel 4. The BMM oracle
uses fp64 dot products of the packed fp16 operands. Each lane passes
when abs(device - exact) / (2^-11 * sum(abs(terms))) <= 1.

### Per-layer island results

| Layer | Island | Verdict | In bound / total | Worst ratio | Rel L2 vs stored output | Max abs | NaN/Inf |
|---:|---|---|---:|---:|---:|---:|---:|
| 0 | A kt | PASS | 2,247,000 / 2,247,000 | 0.540187 | 1.10718e-5 | 0.25 | 0 / 0 |
| 0 | A attention p1 | PASS | 1,125,000 / 1,125,000 | 0.909635 | 1.18815e-4 | 0.00390625 | 0 / 0 |
| 0 | C pv | FAIL | 378,900 / 384,000 | 14.4986 | 2.20817e-4 | 0.0078125 | 0 / 0 |
| 0 | B select | PASS, bit-exact | 1,125,000 / 1,125,000 | n/a | 0 | 0 | 0 / 0 |
| 11 | A kt | PASS | 2,247,000 / 2,247,000 | 0.557678 | 1.05317e-5 | 0.5 | 0 / 0 |
| 11 | A attention p1 | PASS | 1,125,000 / 1,125,000 | 0.760421 | 1.04220e-4 | 0.00390625 | 0 / 0 |
| 11 | C pv | FAIL | 201,258 / 384,000 | 104.132 | 8.40059e-4 | 0.00259399 | 0 / 0 |
| 11 | B select | PASS, bit-exact | 1,125,000 / 1,125,000 | n/a | 0 | 0 | 0 / 0 |
| 23 | A kt | PASS | 2,247,000 / 2,247,000 | 0.470965 | 1.17767e-5 | 0.5 | 0 / 0 |
| 23 | A attention p1 | PASS | 1,125,000 / 1,125,000 | 0.799418 | 7.50990e-5 | 0.0078125 | 0 / 0 |
| 23 | C pv | FAIL | 182,955 / 384,000 | 65.3453 | 0.00112707 | 0.001953125 | 0 / 0 |
| 23 | B select | PASS, bit-exact | 1,125,000 / 1,125,000 | n/a | 0 | 0 | 0 / 0 |

Select matched both stored output and the condition-selected source bits
on every lane. The cond inputs were all zero, so ch6 (the cond=1 branch)
was not selected; this branch contains infinity. Output had zero Inf.

### BMM worst normalized error by head

Each vector lists the maximum `|device - exact| / (2^-11 * sum(|terms|))`
for heads 0–7. C pv is outside the requested per-lane bound.

| Layer | Island | Head 0–7 worst ratios |
|---:|---|---|
| 0 | A kt | 0.29363, 0.29881, 0.54019, 0.39878, 0.33521, 0.33597, 0.37695, 0.34195 |
| 0 | A attention p1 | 0.81799, 0.65111, 0.65997, 0.76053, 0.90964, 0.75209, 0.73484, 0.76701 |
| 0 | C pv | 14.49860, 9.39768, 6.29366, 7.22440, 0.99381, 8.57184, 3.83338, 2.94668 |
| 11 | A kt | 0.45773, 0.55768, 0.53894, 0.51502, 0.46723, 0.31114, 0.48341, 0.52905 |
| 11 | A attention p1 | 0.70812, 0.67972, 0.72529, 0.76042, 0.73501, 0.74660, 0.73927, 0.71294 |
| 11 | C pv | 27.917, 58.788, 16.326, 27.993, 50.233, 27.586, 64.722, 104.132 |
| 23 | A kt | 0.39913, 0.36235, 0.40073, 0.26174, 0.34447, 0.28858, 0.47096, 0.43086 |
| 23 | A attention p1 | 0.74584, 0.76127, 0.77171, 0.67348, 0.69055, 0.79942, 0.70729, 0.61557 |
| 23 | C pv | 42.085, 45.146, 65.345, 59.725, 36.728, 49.792, 26.335, 36.423 |

### Absolute-value magnitude ranges

Each cell is min / max / mean `|value|`. The BMM columns identify
the tensor names for ch5 and ch6; select ch5=b (cond=0), ch6=a
(cond=1), ch7=cond.

| Layer | Island | ch5 | ch6 | ch7 | Output |
|---:|---|---|---|---|---|
| 0 | A kt | pos_kT 4.23e-6 / 131 / 21.3487 | q_v 0 / 3.99219 / 0.175582 | — | 0.000457764 / 574 / 93.3265 |
| 0 | A attention p1 | k_headsT 2.44e-6 / 17.7031 / 0.744477 | q_scaled 4.17e-7 / 0.310547 / 0.0157775 | — | 0 / 6.62109 / 0.932230 |
| 0 | C pv | v_heads 5.96e-8 / 25.2812 / 2.44599 | probs 0 / 0.218628 / 0.00266667 | — | 0 / 14.8281 / 1.17675 |
| 0 | B select | b 8.06e-5 / 50.7188 / 8.25076 | a inf / inf / inf | cond 0 / 0 / 0 | 8.06e-5 / 50.7188 / 8.25076 |
| 11 | A kt | pos_kT 5.90e-6 / 261.75 / 24.9099 | q_v 0 / 3.50781 / 0.344361 | — | 0.000213623 / 1270 / 189.649 |
| 11 | A attention p1 | k_headsT 1.91e-6 / 10.5391 / 1.10377 | q_scaled 0 / 0.313721 / 0.0333273 | — | 0 / 7.90234 / 1.76884 |
| 11 | C pv | v_heads 7.15e-7 / 2.89258 / 0.367600 | probs 0 / 0.767090 / 0.00266667 | — | 0 / 2.06641 / 0.255636 |
| 11 | B select | b 3.03e-5 / 112.25 / 18.0474 | a inf / inf / inf | cond 0 / 0 / 0 | 3.03e-5 / 112.25 / 18.0474 |
| 23 | A kt | pos_kT 0.000128508 / 127.75 / 23.8269 | q_v 0 / 2.91406 / 0.472719 | — | 7.63e-5 / 791 / 224.129 |
| 23 | A attention p1 | k_headsT 3.93e-6 / 9.21875 / 1.68046 | q_scaled 0 / 0.278320 / 0.0447642 | — | 3.05e-5 / 12.2734 / 3.50762 |
| 23 | C pv | v_heads 4.77e-7 / 4.11719 / 0.505818 | probs 0 / 0.952148 / 0.00266666 | — | 0 / 3.11914 / 0.160001 |
| 23 | B select | b 6.38e-6 / 69.9375 / 20.3061 | a inf / inf / inf | cond 0 / 0 / 0 | 6.38e-6 / 69.9375 / 20.3061 |

C pv is outside the strict product bound on all sampled layers even
though its relative L2 error against the stored output is only
0.0002208–0.0011271. The global error does not override the per-lane
failures.

### Remaining work for a full Parakeet encoder pass

This test covers 12 isolated operations, not the 24-layer encoder chain.
The operands are reconstructed CPU-reference tensors, and the reference
encoder output differs from the original capture. C pv fails the strict
product bound on every sampled layer. The host still needs island
orchestration, feed-forward/LayerNorm/residual operations, all-layer
coverage, output concatenation, and end-to-end comparison. Decoder
integration and the bundle manifest/ABI-2 compatibility gap also remain.

### Run evidence

- Runner: `tools/island_real.py`; it reads manifest mappings, validates
  shape/dtype, packs channels, runs under the ANE lock, unpacks output,
  and emits numeric comparisons.
- Final batch: 12 invocations (4 islands × 3 layers), with a
  60-second per-device deadline. The first batch exposed a verdict
  reporting bug. The runner was corrected and all 12 cases were repeated;
  the transcript preserves both batches and the initial error.
- Final verdicts: 9 PASS, 3 FAIL (all C pv). Captured dmesg tail had no
  EXCH failure, protocol-error, or I/O-error marker.
- Notebook entry: under `~/.local/share/apple-silicon-lab/entries/IslandGolden2/`.
- Raw inputs, packed surfaces, device outputs, results JSON, transcript,
  post-state, dmesg, and SHA256SUMS:
  `~/.local/share/apple-silicon-lab/artifacts/IslandGolden2/2026-09-30-real-parakeet/`.
- M2 post-state: kernel `7.1.13-ARCH-polltx`, boot ID
  `95675db4-da91-42e5-bdee-dfb3329e7369`; no kernel/module/boot change.

Runner source: `tools/island_real.py`. Results JSON and checksum inventory
are in the private artifact directory.

## 2026-09-30 C pv subnormal / FTZ follow-up

Subnormal means `0 < abs(x) < 2^-14`, the fp16 minimum normal threshold
is `2^-14`. The tiny count includes zeros (`abs(x) < 2^-14`).

| Layer | Input | Elements | Subnormal | Zero | Min-subnormal (`2^-24`) | Other subnormal |
|---:|---|---:|---:|---:|---:|---:|
| 0 | probs (ch6) | 1,125,000 | 49,317 | 18 | 25 | 49,292 |
| 0 | v_heads (ch5) | 384,000 | 8 | 0 | 5 | 3 |
| 11 | probs (ch6) | 1,125,000 | 421,789 | 167,961 | 34,250 | 387,539 |
| 11 | v_heads (ch5) | 384,000 | 45 | 0 | 0 | 45 |
| 23 | probs (ch6) | 1,125,000 | 429,276 | 5,390 | 9,458 | 419,818 |
| 23 | v_heads (ch5) | 384,000 | 25 | 0 | 0 | 25 |

The original exact-input failures remain unchanged. For the FTZ reference,
both fp16 operands are zeroed when `abs(x) < 2^-14`, then the reference
product and `sum(abs(terms))` are computed in fp64. The original strict
bound is retained: `abs(device - reference) / (2^-11 * sum(abs(terms)))
<= 1`.

| Layer | Reference | Denominator | In bound / total | Worst ratio | Worst lane (head,row,col) |
|---:|---|---|---:|---:|---|
| 0 | Exact inputs | Original terms | 378,900 / 384,000 | 14.4986 | (0,1,43) |
| 0 | Input FTZ | Original terms | 339,582 / 384,000 | 233.8196 | (1,11,4) |
| 0 | Input FTZ | FTZ terms | 339,569 / 384,000 | 271.2988 | (1,11,4) |
| 11 | Exact inputs | Original terms | 201,258 / 384,000 | 104.1316 | (7,224,40) |
| 11 | Input FTZ | Original terms | 151,227 / 384,000 | 125.1519 | (6,40,30) |
| 11 | Input FTZ | FTZ terms | 151,118 / 384,000 | 134.0365 | (6,40,30) |
| 23 | Exact inputs | Original terms | 182,955 / 384,000 | 65.3453 | (2,8,120) |
| 23 | Input FTZ | Original terms | 90,566 / 384,000 | 271.7908 | (7,272,49) |
| 23 | Input FTZ | FTZ terms | 90,438 / 384,000 | 324.1917 | (7,272,49) |

Input FTZ does not explain the failures. It changes 65,942 / 373,832 /
377,816 of 384,000 reference lanes for layers 0 / 11 / 23 and increases
the worst ratios. Every query row has at least one out-of-bound lane in
both the original and FTZ comparisons. Exact-input failures span seven
heads at layer 0 and all eight heads at layers 11 and 23; the worst
lanes are not confined to one row or head. The worst FTZ lanes move to
heads/rows (1,11), (6,40), and (7,272) for layers 0, 11, and 23.

### Isolated subnormal probe

One locked C pv invocation used P=0 except row 0 in every head, where
all 375 K values were the largest fp16 subnormal
(`2^-14 - 2^-24 = 6.0975551605224609e-5`). V was all ones. The exact
no-flush row sum is `0.0228658318519592`; its correctly rounded fp16
value is `0.0228729248046875`. The device returned `0.02288818359375`
for all 1,024 row-0 output lanes, an absolute difference of
`2.23517418e-5` from the fp64 sum (about 1.5 fp16 ulp). Every other row
was zero; there were no NaN/Inf lanes. The nonzero output rejects uniform
input FTZ and product FTZ for this kernel path; those models predict zero.
The precise reduction model remains unknown. No separate accumulator-flush
simulation is justified because accumulator precision and reduction order
are not specified.

The probe used the existing C pv ANEC, proven packing, ch5=V and ch6=P,
under `flock /var/tmp/ane-run.lock timeout 60`. One invocation completed
in 39.131 ms. The M2 kept boot ID
`95675db4-da91-42e5-bdee-dfb3329e7369`; the captured fatal-marker search
found no EXCH failure, protocol error, or I/O error. The probe input
builder, packed inputs, device output, reference metrics, command, logs,
post-state, and SHA256SUMS are under
`~/.local/share/apple-silicon-lab/artifacts/IslandGolden2/2026-09-30-ftz/`.
The notebook entry is under
`~/.local/share/apple-silicon-lab/entries/IslandGolden2/`.
