# Program 20 port binding — root cause and one-run recipe

Date: 2026-09-30. Offline decode; no device time used. Companion analysis note:
docs/plans/2026-09-30-qwen-on-m2.md (M1 section) and the private notebook entry
Prog20Map 20260930T194003Z-ct-prog20-packing.md.

## Root cause (static + numeric proof)

The H14 task stream references surfaces by BAR slot. Apple's surface array for
prog_020 (HWX program-descriptor IOVA list) is:

| slot | surface | address | section | MIL role |
|---|---|---|---|---|
| 4 | t0 [1,1,16,128] | 0x30000000 | const | input |
| 5 | t15 [1,1,1,2048] | 0x30004000 | data | **output** |
| 6 | t2 [1,1,16,128] | 0x30008000 | const | input |
| 7 | t7 [1,1,1,2048] | 0x3000c000 | const | input |

The 20 tasks reference: srcA slots {6, 1, 4, 7}, srcB slot 1, dst slot 5 (task
19 only), kernel/KernelDMA slot 1. Every ref lands on array[slot-4] with a
consistent role; slot 1 is the constant section.

The loader's matmul carve-out (`is_matmul = srcA slots >= 2`, libane/ane_m2.c
bar_ref_tag_extents) maps slot -> slot, so the M1 run bound slot 4 -> ch4: the
kernel read the zero-filled OUTPUT channel in place of t0, and ch5 (t0) was
never read. The M1 failure is exactly the MIL graph with t0 = 0:

| comparison | max_abs | rel L2 | exact fp16 |
|---|---|---|---|
| fp16 MIL (real t0) vs M1 golden | 0.00507 | 0.01124 | 2.00% |
| fp16 MIL (t0 = 0) vs device call 1 | 0.00977 | 0.01216 | 1.61% |
| fp16 MIL (t0 = 0, t2 := t0 data) vs device swap call | 0.00977 | 0.01391 | 1.22% |
| device call 1 vs M1 golden (recorded) | 0.47461 | 0.27566 | 0.05% |

Both device calls track their t0-replaced model to the normal fp16 band, so the
constants, the packing, the feed order, and the output readback are all correct;
the only defect is the slot-4 binding.

## Why the other suspects are cleared

- Packing: the runner's pack (dense fp16 at the HWX descriptor strides
  [4096,4096,256,2] / [4096,4096,4096,2], zero-padded to the 16 KiB tile) is
  byte-identical to the three surfaces the device consumed.
- Feed order: input-0-t0 -> ch5, input-1-t2 -> ch6, input-2-t7 -> ch7 matches
  the port table and the HWX buffer names; the t0/t2 swap call is also fully
  explained by the model above.
- Constants: the ANEC embeds all 83,892,736 constant bytes (Apple's re-serialized
  blob: t3_g at 0x0, t9_g at 0x100 byte-identical to the MIL weights data;
  weights.bin is not embedded verbatim). `ane-run --weights` feeds the host-side
  --check oracle only; the device path never needs it. The "16 MiB BO_INIT cap"
  manifest note does not apply to this loader; the sections loaded and ran.
- A 64-byte-row [1,C,1,1]-style layout for t7/t15 (the rms-island convention) is
  statically impossible here: it needs 128 KiB per surface, the ANEC allocates
  one 16 KiB tile, and the descriptor strides say dense.

## The fix

Bind the BAR patch by the array rule at load time:

```
ANE_M2_OPREFS=1:2,4:5,5:4,6:6,7:7
```

tools/qwen_prog_run.py derives this automatically from the port-table manifest,
the HWX surface array, and the ANEC stream (and refuses programs whose slots do
not fit the rule), packs the inputs, runs ane-run, and unpacks the output.

## One device run of program 20 (after the M2 is released)

```sh
cd ~/src/omarchy-ane-m2-installed-wt
A=~/.local/share/apple-silicon-lab/artifacts/QwenM2/2026-09-30-t6021-qwen-chain/prog020-classC
python3 tools/qwen_prog_run.py --prog prog_020 --repeat 2 \
  --in t0=$A/input-0-t0.f16 --in t2=$A/input-1-t2.f16 --in t7=$A/input-2-t7.f16 \
  --golden $A/golden-t15.f16
```

The script wraps the device call it prints, of the form:

```sh
flock /var/tmp/ane-run.lock timeout 60 env ANE_M2_OPREFS=1:2,4:5,5:4,6:6,7:7 \
  /var/tmp/inst/tools/ane-run --anec /var/tmp/qwen-real-anec-h14/prog_020/program-0.anec \
  --in 0=.../in-t0.surface --in 1=.../in-t2.surface --in 2=.../in-t7.surface \
  --out 0=.../out.surface --repeat 2
```

Pass band (operation-derived): max_abs <= ~0.011, rel L2 <= ~0.023, exact
fp16 fraction >= ~1% (twice the M1-vs-fp64 band). `--repeat 2` doubles as an
aliasing probe: with a correct binding the two calls must return identical
bytes, because no input surface aliases the output channel any more.

## Fallback hypotheses (one run each, ranked)

1. **Port-name swap, binding kept.** `--in t0=$A/input-1-t2.f16 --in
   t2=$A/input-0-t0.f16` with the same OPREFS. Prediction: if the HWX buffer
   names do not match MIL semantics, output matches the golden; if names are
   right (expected), output matches fp16 MIL(t2 := t0) and stays ~0.28 rel L2
   — offline-computable discriminator, only worth running if hypothesis 0 fails.
2. **Override ignored by the deployed libane.** If the output is byte-for-byte
   the recorded broken output (max_abs 0.47461), ANE_M2_OPREFS did not reach
   oprefs_apply: the target's installed libane predates the select-island
   build. Remedy: rebuild/install libane from this branch, then rerun
   hypothesis 0. Not a packing problem.
3. **Orientation transpose.** Pack t0/t2 as [128,16] (transpose within the same
4096 B) with the fixed binding. Command: add `--transpose t0 --transpose t2`
to the runner invocation. Prediction: matters only if Apple's surface names
encode a transposed row convention; expected rel L2 ~0.27 with a structured
(row-permuted) error signature. Run only after 0 and 1 fail.
4. **Control (no override).** `--no-oprefs` rerun must reproduce the recorded
   broken metrics exactly. Use it to confirm the harness when anything else is
   surprising; it is also the clean A/B pair for the receipt.

## Genericity

The runner parses the port manifest for every staged program, but only prog_020
of the 38 entries fits the current loader contract (one output on ch4 and at most
three inputs on ch5..7). The other 37 are refused before packing; most expose
multiple outputs, which libane does not model. Running all 38 requires a separate
converter/loader extension and is not claimed by this script.
