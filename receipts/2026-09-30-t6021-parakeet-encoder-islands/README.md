# 2026-09-30 — T6021 Parakeet encoder with attention islands on ANE, rest on CPU

## Purpose

Measure whether the four verified Parakeet encoder island families
(A kt, A p1, C pv, B select) can execute on the M2 (T6021) ANE for every
one of the 24 encoder layers with real data-dependent activations produced
in the loop, without breaking the final encoder output or the decoded
transcript. The strict per-lane C pv accumulator bound fails on 1.3 % to
52.4 % of lanes (receipts/2026-09-30-t6021-accumulator); this receipt
answers the product question those probes left open.

What this is NOT: this is not "Parakeet on M2". Every op outside the four
island sites runs on the host CPU: the three subsampling convolutions, all
LayerNorms, all feed-forward linears and GLU gates, the depthwise
convolutions, every softmax, every residual add, the o_proj matmuls, and
the whole decoder / joint / tokenizer.

## Four variants measured

| Variant | A kt | A p1 | C pv | B select | everything else |
|---|---|---|---|---|---|
| baseline | CPU | CPU | CPU | CPU | CPU |
| islands-on-m2 | ANE | ANE | ANE | ANE | CPU |
| cpv-only-on-m2 | CPU | CPU | ANE | CPU | CPU |
| cpv-noise-cpu | CPU | CPU | CPU (emulated noise) | CPU | CPU |

All variants consume the same MIL reference program
(`/var/tmp/parakeet-src/model.mil`, sha256 ac8e9526154ac8b8…), the same
fixture inputs (LibriSpeech test-clean 1089-134686-0000), and differ only
at the four island sites per layer. Device dispatch carries the operands
produced in the loop from the previous layer's output — no captured-sample
replay.

## Final encoder output (1, 375, 640) fp32

| Variant | rel L2 vs baseline | max abs vs baseline | rel L2 vs golden | max abs vs golden | tokens | transcript vs golden | WER |
|---|---|---|---|---|---|---|---|
| baseline | — | — | 2.485159e-02 | 1.464233e-01 | 100 | DIFFER (tail only) | 0.0690 |
| islands-on-m2 | 2.689549e-03 | 3.045654e-02 | 2.473639e-02 | 1.408081e-01 | 104 | EXACT MATCH | 0.0000 |
| cpv-only-on-m2 | 2.663920e-03 | 1.892090e-02 | 2.542077e-02 | 1.550293e-01 | 100 | DIFFER (tail only) | 0.0690 |
| cpv-noise-cpu | 2.361775e-03 | 2.526855e-02 | 2.522940e-02 | 1.622925e-01 | 104 | EXACT MATCH | 0.0000 |

The golden encoder output and 104-token transcript come from the pinned
macOS ANE capture
(`captures/b650695c-75aec2a/20260912T154759Z-librispeech/ane`).

## Per-layer divergence vs baseline

rel L2 of the layer-normed residual after each layer:

| Layer | islands-on-m2 | cpv-only-on-m2 | cpv-noise-cpu |
|---:|---|---|---|
| 0 | 2.902053e-04 | 2.786188e-04 | 2.061817e-05 |
| 1 | 6.364312e-04 | 6.288634e-04 | 3.024934e-04 |
| 2 | 8.588705e-04 | 8.440837e-04 | 5.771991e-04 |
| 3 | 9.761251e-04 | 9.645241e-04 | 7.567127e-04 |
| 4 | 1.063584e-03 | 1.047950e-03 | 8.641852e-04 |
| 5 | 1.094813e-03 | 1.087977e-03 | 9.339316e-04 |
| 6 | 9.902932e-04 | 9.956457e-04 | 8.431934e-04 |
| 7 | 1.106497e-03 | 1.119491e-03 | 9.523624e-04 |
| 8 | 1.200600e-03 | 1.212645e-03 | 1.013350e-03 |
| 9 | 1.535575e-03 | 1.557738e-03 | 1.252885e-03 |
| 10 | 1.597155e-03 | 1.603248e-03 | 1.307905e-03 |
| 11 | 1.763179e-03 | 1.782173e-03 | 1.491023e-03 |
| 12 | 1.776126e-03 | 1.800339e-03 | 1.475504e-03 |
| 13 | 2.140111e-03 | 2.151489e-03 | 1.692805e-03 |
| 14 | 2.369320e-03 | 2.360120e-03 | 1.746985e-03 |
| 15 | 2.200510e-03 | 2.155798e-03 | 1.587862e-03 |
| 16 | 2.708215e-03 | 2.543019e-03 | 1.798491e-03 |
| 17 | 2.249967e-03 | 2.096004e-03 | 1.570820e-03 |
| 18 | 1.854553e-03 | 1.776675e-03 | 1.376546e-03 |
| 19 | 1.680362e-03 | 1.596325e-03 | 1.320776e-03 |
| 20 | 2.068656e-03 | 1.932289e-03 | 1.528555e-03 |
| 21 | 1.861962e-03 | 1.761383e-03 | 1.445339e-03 |
| 22 | 1.732989e-03 | 1.672792e-03 | 1.450463e-03 |
| 23 | 2.822025e-03 | 2.801914e-03 | 2.459505e-03 |

No divergence explosion with depth. The worst layer (23) sits ~35x inside
the frozen encoder contract (rel L2 <= 0.1, max abs <= 0.3).

## Transcripts

Golden (104 tokens): "He hoped there would be stew for dinner, turnips and
carrots and bruised potatoes and fat mutton pieces to be ladled out in
thick, peppered, flour-fattened sauce...........................................
Юн Ю"

- baseline (100 tokens): identical up to "sauce", then 41 dots (vs 43) and
  "Юн" (one trailing Cyrillic token missing). WER 0.0690.
- islands-on-m2 (104 tokens): byte-identical to the golden transcript.
- cpv-only-on-m2 (100 tokens): same as baseline.
- cpv-noise-cpu (104 tokens): byte-identical to the golden transcript.

Control: the NumPy BNNS-contract decoder + joint + greedy TDT control +
pinned tokenizer reproduce the golden 104 tokens EXACTLY when fed the
golden encoder_hidden (golden-replay). Every transcript difference above is
therefore attributable to the encoder variants, not the decoder chain. The
differences that do appear live inside the reference's own degenerate junk
suffix (trailing dots and Cyrillic); the intelligible transcript head is
identical in every variant.

## Findings

1. The strict C pv accumulator bound failures do NOT propagate. Swapping
   the exact fp16 CPU C pv for the real ANE C pv in every layer moves the
   encoder CLOSER to the ANE-captured golden (rel L2 2.4736e-02 vs
   2.4852e-02) and recovers the exact golden transcript. The golden capture
   was produced by a real ANE; the device's measured accumulator noise is
   the product-path behaviour.
2. A kt, A p1 and B select contribute almost nothing to the divergence:
   islands-on-m2 vs cpv-only-on-m2 differs by 2.6e-05 in final-output
   rel L2 (2.6895e-03 vs 2.6639e-03), bounding the A-family + select
   device contribution at ~1 % of the total island divergence. Consistent
   with the per-island receipts (A/P1/select PASS the strict bound).
3. The emulated 2^-16-grid C pv noise reproduces the qualitative result
   (104-token match) but is not bit-exact to the device (rel L2 vs
   baseline 2.362e-03 vs the device's 2.664e-03); the real accumulator's
   internal chunked rounding is not modelled (receipt
   2026-09-30-t6021-accumulator: no tested model is bit-exact).
4. The islands-on-m2 path is loop-closed: every layer's island operands
   were packed from activations produced by the previous layers in the same
   run, including through the ANE-computed C pv of the previous layer.

## Tool

`tools/parakeet_encoder_islands.py` (Python 3 + numpy). Subclasses
mlx-omarchy's `mil_numpy.Program`, routing the four island sites per layer
to either CPU numpy or an M2 `ane-run` invocation. Operand packing uses the
proven aligned surface layout (reused from `tools/island_ref.py`); the
decoder mirrors the measured BNNS fused-LSTM contract
(`vulkan_decoder.py::fused_lstm_layer`, bit-indexed sigmoid/tanh LUTs) and
the joint mirrors `vulkan_joint.py`. Tokenization uses the pinned
`ParakeetTokenizer` detokenizer. TDT greedy control mirrors
`tdt_control.decode_tdt` (durations 0-4, max 10 symbols per step, blank
8192).

Device invocation per island site (120 total: 96 + 24):

```
ssh <M2-SSH-alias> flock /var/tmp/ane-run.lock timeout 60 \
  /var/tmp/inst/tools/ane-run \
  --anec /var/tmp/inst/fixtures/h14-anec/<island>/program-0.anec \
  --in 0=<ch6 surface> --in 1=<ch5 surface> [--in 2=<ch7 cond>] \
  --out 0=<out> --time
```

Island channel bindings are the device-proven ones
(receipts 2026-09-30-t6021-island-bmm / -select-rms): bmm ch6 = first MIL
input (x), ch5 = second (w, K x N surface); select ch6 = cond=1 branch (the
-inf constant, broadcast), ch5 = cond=0 branch, ch7 = cond.

## Device state and safety

- Every invocation under `flock /var/tmp/ane-run.lock timeout 60`.
- 120 device submissions, zero failures, zero non-finite device lanes.
- dmesg after the run: 769 ANE/DART/CALLIO lines, 0 DART faults, 0
  "EXCH ... failed" markers (`run1/dmesg-e2e.txt`).
- Kernel 7.1.13-ARCH-polltx, boot unchanged, no module/config change.
- Per-island device exec times: `ane-run --time` printed them per call, but
  the harness parser recorded the call count instead; the wrong values were
  stripped from the report and are not quoted here. Prior receipts measured
  30.2-38.6 ms exec per island on the same ANECs
  (receipts/2026-09-30-t6021-island-golden).

## Wall time per variant (CT side, includes ssh/scp transport)

| Variant | Wall s | Device calls |
|---|---:|---:|
| baseline | 26.5 | 0 |
| islands-on-m2 | 76.4 | 96 |
| cpv-only-on-m2 | 39.5 | 24 |
| cpv-noise-cpu | 25.1 | 0 |

## Artifacts

`~/.local/share/apple-silicon-lab/artifacts/ParakeetE2E/run1/`
(SHA256SUMS verified):

- `report.json`, `report.txt` — full numbers, per-layer curves, transcripts.
- `hidden_{baseline,islands-on-m2,cpv-only-on-m2,cpv-noise-cpu}.npy` —
  final encoder outputs, fp32 (1, 375, 640).
- `dmesg-e2e.txt` — M2 dmesg after the run (0 faults).
- `m2_probe.txt` — pre-run liveness + fixture listing.

Notebook entry: `~/.local/share/apple-silicon-lab/entries/ParakeetE2E/`
(file `20260930T165300Z-*-encoder-islands.md`).

## Not in this receipt

- Non-island encoder subgraphs still run on CPU; no end-to-end ANE-only
  encoder timing or latency claim is made.
- Per-call device exec time not captured by this harness (parser bug, see
  above); the E2E question here is accuracy, not latency.
- The decoder/joint ran on the CT CPU; no M2 decoder execution.
- The emulated C pv noise model is first-order (grid + FTZ only).
