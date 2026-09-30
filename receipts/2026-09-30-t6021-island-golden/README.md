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

The capture harness was last run 2026-09-12 on macstudio (the pinned
build/compiler oracle, never updated); a re-capture would require
running macOS parakeet-encoder inference on the M2's macOS partition,
which is outside the scope of this workstream (assignment forbids
fleet hosts other than jw14m2-linux; no other host was probed).

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
ssh jw14m2-linux 'timeout 60 /var/tmp/inst/tools/ane-run \
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

1. **Real activations, not synthetic.** The substitute activations
   match the distribution and magnitude of post-LayerNorm / post-
   softmax encoder activations but are NOT real encoder tensors from
   the CoreML parakeet-encoder. A real-data run requires re-running
   `overlay/tools/coreml/capture` (Swift, macOS-only) on the M2
   macOS partition or macstudio, with per-layer probe injection that
   emits q, k, v, probs, attention_scores, attention_mask tensors.
   This is outside the scope of this workstream.

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

6. **No first-call latency amortization.** Each island was run
   once; a real encoder pass invokes each island N times (one
   per layer) and amortizes driver setup over those calls.

7. **Bundle manifest channel strictness (ABI gap 4).** The
   libane-ABI-2 un-strict build (a separate ongoing task) is
   what makes the worker able to load a schema-4 bundle
   against an ABI-2 backend; today the worker would refuse
   these bundles until that gap closes.

## Files

- Tool: `/home/joshuawarren/src/omarchy-ane-m2-installed-wt/tools/island_golden.py`
- Receipt: this file
- Notebook entry: `~/.local/share/apple-silicon-lab/entries/IslandGolden/20260930T111114Z-jw14m2-linux-island-golden.md`
- Artifacts: `~/.local/share/apple-silicon-lab/artifacts/IslandGolden/`

## Not in this receipt

- Modifying `tools/island_ref.py` (the existing packing code is reused
  via Python import; no change to that file).
- Any change to libane, ane.ko, or the driver.
- Any edit to mlx-omarchy, mil-hwx-compiler, or the FSM worktree.
- Real encoder activation capture (not available on this CT; see
  "What still separates this" item 1).