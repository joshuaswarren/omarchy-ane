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

### All 38 programs, boot `0e2c3743`: PASS

Boot `0e2c3743`, stock `7.1.13-3-1-ARCH`, module `a584a967` (BO cap, 12 GiB),
no module options. All 38 programs were already loaded, so `bo_total_bytes`
was 2,818,932,736 before and after every run below.

**Run 1 (invalid).** It used ane-run `238eb85b` (sources equal main). It made
414 calls with exit 0 and no stop. prog_020 passed, and the other 37 programs
failed every binding trial. Every output surface of one allocation size held
the same bytes, those of output 0 (prog_000 step 11: six 16 KiB outputs, all
SHA-256 `6fe8d891`, equal to q). The ane-run log said `LIBANE: ERR: tried to
index N but max is 1; bailing.` for each later output. `__ane_send` and
`__ane_read` (libane/ane.c) checked the index against the ANEC header's
src/dst count before they dispatched to the M2 port path. All 38 Qwen ANEC
headers record `dst_count = 1`; `src_count` equals the table's input count. So
only output 0 was read. For every other output, ane-run wrote an uninitialized
`malloc` buffer to the file, and glibc reused the freed chunk of the same
size. The kernel io binding, the BO pool, mmap and `ane_m2_read` are correct.
Fix (cb5a8f1): send and read dispatch to the M2 path first, as the size
helpers already did. The harness now fails any call whose ane-run output has a
`LIBANE: ERR` line, and it writes `ports.resolved.json` only when a binding
trial passes.

**Run 2.** ane-run `e7986b48` (cb5a8f1), work `/var/tmp/qwen-conform-0e2c3743-r2`.
It made 794 device calls: 338 binding trials and 456 conform runs, each exiting 0.
There was no stop. Four calls saw new kernel lines; past 1,000 s of uptime all
of them were `UFW BLOCK` firewall lines (47), with 0 EXCH-fail, DART-fault,
quarantine or completion-wait lines:

- **Verdicts:** 456 of 456 runs PASS. That is 38 programs x 6 steps with the M1
  state inputs, plus 38 x 6 chained (0→1→2 and 11→12→13, where each state input
  is the M2's own state output of the previous step). Over all 2,304 port
  results, the largest rel L2 vs the M1 is 0.00698 (prog_002, step 12 chained,
  k lane, max abs 0.0050). No output exceeds 0.02, so every output also passes a
  flat 0.02. No output has a NaN or Inf.
- **Bit-exact:** 146 of the 384 port results at steps 0 and 11 equal the M1
  output byte for byte. None of them equals an M1 input of the same execution,
  and none is all-zero. The 18 DeltaNet programs (1, 3, …, 19, 22, …, 36) are
  bit-exact in all 432 of their port results, chained or not. In the other 20
  programs, every lane and state kind has some non-exact results.
- **Bindings:** all 114 ambiguity groups of the 37 tables are `decided`, and
  all of them are the generated table order (identity). The runner-up input
  permutation is 7.4 to 47 times the threshold. prog_006's pairs t22/t27,
  t28/t44 and t32/t39 are decided at 0.063 vs 16.1. Per group:
  [conformance-bindings.tsv](conformance-bindings.tsv).
- **Determinism:** each program ran with the same step-0 and step-11 inputs
  in two processes (chained and unchained series). The outputs were
  byte-identical in 76 of 76 pairs. The step-11 binding trial equals the
  step-11 conform run in 38 of 38.
- **Chaining:** in 89 of 152 chained runs at steps 1, 2, 12 and 13, the outputs
  are byte-identical to the run with the M1 state. The largest drift is
  prog_002 step 13: conv state t53 at rel L2 0.0041 vs threshold 0.0202 (0.20 of
  it), against 0.10 of the threshold with the M1 state.

Rel L2 vs M1 of the port nearest its threshold, with the M1 state inputs. The
chained columns give the largest worst-port ratio to the threshold:

| prog | outputs | s0 | s1 | s2 | s11 | s12 | s13 | chained 0-2 | chained 11-13 | exec ms |
|---|---|---|---|---|---|---|---|---|---|---|
| 000 | 7 | 0.00041 | 0.00045 | 0.00044 | 0.00034 | 0.00035 | 0.0005 | 0.006 | 0.006 | 3.20 |
| 001-019 odd, 022-036 even (18 DeltaNet) | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1.60-1.69 |
| 002 | 8 | 0.00041 | 0.0038 | 0.0038 | 0.0042 | 0.0035 | 0.002 | 0.110 | 0.205 | 6.48 |
| 004 | 8 | 0.0025 | 0.00031 | 0.0016 | 0.00037 | 0.00042 | 0.0025 | 0.081 | 0.079 | 6.47 |
| 006 | 10 | 0.00053 | 0.0022 | 0.0025 | 0.0027 | 0.0027 | 0.0015 | 0.119 | 0.144 | 10.72 |
| 008 | 8 | 0.00092 | 0.0014 | 0.0018 | 0.0004 | 0.002 | 0.0016 | 0.074 | 0.069 | 6.46 |
| 010 | 8 | 0.00083 | 0.0017 | 0.0016 | 0.0025 | 0.0018 | 0.0015 | 0.061 | 0.097 | 6.47 |
| 012 | 10 | 0.00075 | 0.0029 | 0.0021 | 0.0036 | 0.0034 | 0.0021 | 0.092 | 0.111 | 10.81 |
| 014 | 8 | 0.00033 | 0.0012 | 0.00047 | 0.0014 | 0.00031 | 0.00026 | 0.051 | 0.055 | 6.46 |
| 016 | 8 | 0.00043 | 0.00074 | 0.00051 | 0.0018 | 0.00047 | 0.0013 | 0.037 | 0.071 | 6.46 |
| 018 | 10 | 0.0011 | 0.0029 | 0.0021 | 0.0019 | 0.0019 | 0.0022 | 0.103 | 0.066 | 10.82 |
| 020 | 1 | 0 | 0.0015 | 0.0015 | 0.0012 | 0.0013 | 0 | 0.070 | 0.052 | 4.98 |
| 021 | 7 | 0.00039 | 0.0008 | 0.00057 | 0.00077 | 0.00071 | 0.00046 | 0.040 | 0.038 | 3.12 |
| 023 | 8 | 0.001 | 0.00045 | 0.00062 | 0.00053 | 0.0018 | 0.00094 | 0.050 | 0.073 | 6.47 |
| 025 | 10 | 0.00088 | 0.0013 | 0.00075 | 0.0023 | 0.00096 | 0.0018 | 0.059 | 0.101 | 10.80 |
| 027 | 8 | 0.00097 | 0.0006 | 0.0012 | 0.00076 | 0.00083 | 0.00098 | 0.058 | 0.049 | 6.47 |
| 029 | 8 | 0.0012 | 0.00073 | 0.00058 | 0.0007 | 0.0011 | 0.0013 | 0.060 | 0.063 | 6.46 |
| 031 | 10 | 0.0013 | 0.00079 | 0.001 | 0.0012 | 0.0011 | 0.0016 | 0.065 | 0.061 | 10.73 |
| 033 | 8 | 0.00066 | 0.00081 | 0.00045 | 0.0005 | 0.00097 | 0.00063 | 0.040 | 0.048 | 6.39 |
| 035 | 8 | 0.00061 | 0.00088 | 0.0005 | 0.00037 | 0.00038 | 0.001 | 0.044 | 0.050 | 6.46 |
| 037 | 3 | 0.00099 | 0.0011 | 0.00093 | 0.00073 | 0.00091 | 0.00085 | 0.049 | 0.047 | 9.24 |

Not covered: the host work between programs (embedding gather, context
tables, lm_head and argmax) and a token-level decode that chains all 38
programs. Each program here got the M1 inputs of its own step; only the
resident state was chained. One boot.

Private record: entry `entries/QwenConform/20261001T004100Z-…-conformance.md`
and `artifacts/QwenConform/` (SHA256SUMS).
