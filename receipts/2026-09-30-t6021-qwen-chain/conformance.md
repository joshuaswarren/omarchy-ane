# Qwen per-program conformance against the M1 step dump

Date: 2026-09-30. Branch: agent/qwen-m2-conform.

## Tools

- `tools/qwen_m2_conform.py` runs each staged program with the M1 inputs of one
  decode step from the per-step dump (`tools/staged-qwen/dump_step_ports.py`;
  6 steps x 38 programs) and compares every output port with the M1 output:
  rel L2, max abs, exact fp16 fraction, NaN/Inf count, and the SHA-256 of each
  output. A program passes a step when every lane and state output is finite
  and within its threshold.
- Before a program's first comparison, the harness resolves the ambiguity
  groups of its port table on the device at step 11. It makes one run per
  permutation of the input groups, and it matches the outputs of each output
  group to the M1 outputs by error. A group is `decided` when exactly one
  binding passes. It is `undecided` when several bindings pass (the graph is
  symmetric in those inputs). It is `identical-inputs` when the M1 inputs of
  the group are equal at that step. It is `fail` when no binding passes. The
  best binding is written to `<work>/prog_NNN/ports.resolved.json`, which
  renames each slot to the tensor that it holds. All later runs use it.
- `--chain-state` feeds each state input from the harness's own state output
  of step S-1, so steps 12 and 13 test the chaining of resident state on the
  M2. On the M2, a state has separate input and output BOs.
- Each device call is `flock /var/tmp/ane-run.lock timeout 120 ane-run --ports`.
  The harness stops on a timeout, on `Connection timed out` or
  `Input/output error` from ane-run, or on a new kernel line that matches
  `EXCH ... failed`. Results append to `results.jsonl` as flat records, and a
  rerun skips every recorded run.
- `tools/mil_eval.py` evaluates a staged MIL program in float64 with its fp16
  constants. On program 20 step 11 it reproduces the earlier fp64 reference
  bit for bit (max abs difference 0.0) and the M1 band 0.011227.
- `tools/qwen_prog_run.py` and the harness share one pack/run/unpack path
  (`ane_call`). The prog_020 dry run prints the same command as before and
  packs the same surfaces (`c2f1483b`, `d1fa1370`, `9e996bea`).

## Threshold

The threshold of an output at a step is `max(0.02, 2 b)`. Here b is the rel L2
of the M1 output against the float64 evaluation of the same program on the
same inputs (`mil_eval.py`, 1,152 outputs = 38 programs x 6 steps). If the M2
stays within the M1's band of the exact result, then
|M2 - M1| <= |M2 - f64| + |M1 - f64| <= 2 b. A flat 0.02 is not justified,
because the M1 itself is further than 0.02 from the exact result on several
lane classes:

| Output class | n | median b | max b (prog, step) | threshold range |
|---|---|---|---|---|
| DeltaNet state | 108 | 0.0007 | 0.0080 (prog_017, step 0) | 0.020-0.020 |
| KV cache | 72 | 0.0022 | 0.0125 (prog_006, step 0) | 0.020-0.025 |
| conv state | 108 | 0.0040 | 0.0204 (prog_002, step 0) | 0.020-0.041 |
| lane beta | 108 | 0.0026 | 0.0307 (prog_002, step 11) | 0.020-0.061 |
| lane gt | 108 | 0.0010 | 0.0076 (prog_002, step 13) | 0.020-0.020 |
| lane h | 12 | 0.0067 | 0.0135 (prog_020, step 12) | 0.020-0.027 |
| lane k | 108 | 0.0574 | 0.4062 (prog_014, step 0) | 0.032-0.812 |
| lane o | 108 | 0.0452 | 0.3644 (prog_022, step 0) | 0.020-0.729 |
| lane q | 108 | 0.0397 | 0.0747 (prog_002, step 12) | 0.030-0.149 |
| lane v | 108 | 0.0226 | 0.1321 (prog_016, step 0) | 0.020-0.264 |
| lane x | 96 | 0.0119 | 0.0309 (prog_002, step 12) | 0.020-0.062 |
| lane z | 108 | 0.0058 | 0.0243 (prog_002, step 11) | 0.020-0.049 |

The large k and o bands come from fp16 conditioning, not from the evaluator.
At prog_014 step 0, k heads 2 and 7 are unit-normalized vectors with a per-head
rel error of 1.2 and 1.0, and the other heads have 0.03-0.22. At prog_022
step 0, the o head norms are 1e-4 or smaller. Each record carries its `m1_band`
and `threshold`, so a tighter ratio can be applied afterward. Program 20 is the
known reference: its M2 output was at 0.00117 vs the M1 with b = 0.0112.

## Host verification (no device)

- `tests/test_qwen_m2_conform.py`: metrics, output matching, the binding
  decision, the resolved-table rename (accepted by `ane-run --dry-run`), and
  the packing of a real dump input (prog_000 step 11 `t1` 16,384 B and `t5`
  393,216 B surfaces against the port table). 6 passed; the existing
  `tests/test_qwen_prog_run.py` still passes.
- Dry run of all 38 programs at steps 0 and 11: every table matches the dump,
  every input packs, and the run needs 414 device calls including resolution.
- Simulated device (float64 MIL with swapped slots): the harness picked the
  swapped prog_020 inputs (worst ratio 0.50 vs 12.2 for the table order) and
  the swapped prog_000 outputs. It decided prog_001's identity binding among 12
  trials (next best ratio 33). A rerun made 0 new calls, and chained steps 12
  and 13 ran on their own state.

## Device results

Sanity run before the BO-cap reboot. Boot `b68db721`, stock `7.1.13-3-1-ARCH`,
boot-time module from the call-wait receipt. ane-run was built on the M2 at
8e5699e (sources equal main eaf16f9) with no warnings, sha `238eb85b…`.
`qwen_m2_conform.py --progs 20 --steps 11` made 3 device calls of 0.13 s wall
each (exec 4.89 ms), and no new kernel lines appeared:

| Run | rel L2 vs M1 | max abs | exact | threshold | verdict |
|---|---|---|---|---|---|
| binding trial, table order (t0 at slot 4) | 0.0011742 | | | 0.022454 | pass |
| binding trial, t0/t2 swapped | 0.27548 | | | 0.022454 | fail |
| conform, step 11 | 0.0011742 | 0.000732 | 17.29% | 0.022454 | PASS |

The binding is `decided`: the table order has 0.052 of the threshold and the
swap has 12.27 of it. The conform result equals the call-wait receipt (rel L2
0.00117, 17.3% exact). The swap equals the earlier swapped M1-input run (0.2759).
The full 38-program run waits for the BO-cap reboot.
