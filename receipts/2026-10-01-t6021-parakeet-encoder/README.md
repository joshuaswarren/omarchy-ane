# 2026-10-01 — T6021: the whole Parakeet encoder as one Apple-compiled H14 program

## Result: PASS, bit-exact with the golden

The 24-layer Parakeet TDT 0.6B v3 encoder ran on the M2 (T6021) ANE under
Linux as one program and one call per inference. Every encoder op ran on the
ANE. The output is bit-exact with the golden macOS ANE capture, and the
greedy TDT decode gives the golden 104 tokens.

| Check | Result |
|---|---|
| Apple h14 compile of the whole encoder MIL | accepted, one program, 14.6 s |
| Static port gates (`hwx_ports.py`, `ane-run --dry-run`) | 0 exceptions, 0 ambiguities, exit 0 |
| Device calls (3 processes: 1 + 20 + 20 calls) | exit 0; 0 new ANE, DART, EXCH or quarantine kernel lines |
| encoder_hidden [1,375,640] vs golden | rel L2 0, max abs 0, bit-exact (fp16 sha256 `fca96f13…`) |
| encoder_hidden vs CPU NumPy MIL fp32 reference | rel L2 2.4651e-02, max abs 1.4642e-01 |
| output_mask vs golden | equal (375 of 375 frames) |
| Tokens vs golden | 104 of 104 equal; transcript equal; WER 0 |
| Determinism | the last output of each of the 3 processes is byte-identical |
| Exec time per call | min 254.405 ms, median 254.50 ms (40 calls, 2 processes) |

The islands run ([2026-09-30-t6021-parakeet-encoder-islands](../2026-09-30-t6021-parakeet-encoder-islands/README.md))
put four attention families on the ANE and gave rel L2 2.4736e-02 against the
golden. This run puts the whole encoder on the ANE and gives 0.

Fixture: LibriSpeech test-clean 1089-134686-0000; capture
`captures/b650695c-75aec2a/20260912T154759Z-librispeech/ane` of
`mweinbach1/parakeet-tdt-0.6b-v3-coreml@b650695c`. Inputs: the capture's
`encoder_input_features` [1,3000,128] and `encoder_input_mask` [1,3000] (all
3000 frames valid), cast to fp16.

## Source program

The H13 whole-encoder container (sha256 `13c74423…`, 13,701 tasks) comes from
the macOS-compiled HWX `020428fc…`. Its compile input is the ANE-segment MIL
that CoreML wrote for the encoder:
`ane-linux-experiments receipts/2026-09-22-encoder-island-cost/capture/model.mil`
(sha256 `4e3d2e8d4fc1…`), with its `weights/weight.bin` (sha256
`295dccd4a5b8…`, 444,016,768 B, 814 blobs). The MIL is `program(1.3)`,
`ios18`, 3,350 statements, 194 `constexpr_lut_to_dense` (palettized
weights). Inputs are fp16 `attention_mask` [1,3000] and `input_features`
[1,3000,128]. Outputs are `linear_217_cast_fp16` [1,375,640] and
`output_mask_f` [1,375]. The sequence length is fixed at 3000 mel frames
(375 encoder frames).

The constants are palettized, so they are 448 MB, not 1.2 GB of fp16.

## Compile (Apple compiler, h14 cross target)

`ane-compile-hwx <dir> <dir>/out h14` on an M1 Ultra with macOS 26.6.2 (tool
sha256 `2060776c98e3…`), at nice 20 and load average below 8. The directory
holds the MIL verbatim, `weights/weight.bin` (the MIL's BLOBFILE path), and
`weights.bin` (a hard link for the tool's file check). The MIL needs no
change.

- `ANECompile=0 callback_status=0`, 14.58 s wall, 699 MB peak RSS.
- HWX 450,920,448 B, sha256 `aa84e0622d844671eb4e955fd1d8744c9763e9040c1ca96f6b62cc2acdd85607`.
- One kind-4 program descriptor: 3,597 tasks (H13: 13,701),
  `__TEXT,__text` 903,208 B, `__TEXT,__const` 447,803,456 B,
  `__DATA,__bss` 61,440,064 B (scratch, BAR slot 3).

No split and no H13 control compile were necessary.

## Conversion and port table

`tools/hwx_h14_staged_to_anec.py` (new in the repo here; the Qwen ANEC
manifest names this path) is the multi-port converter that made the 38 Qwen
ANECs. On Qwen program 20 it reproduces the device-proven ANEC byte for
byte. On the encoder HWX:

- ANEC 448,710,784 B, sha256 `82ce8a1a46d637c565abedfe284aa68ecefe141f9609d6f1f9d89ffdd1fc4acf`,
  firstTaskBytes 156, 2 inputs, 2 outputs.
- Channels: ch4 `linear_217_cast_fp16` (480,000 B), ch5 `attention_mask`
  (6,016 B), ch6 `input_features` (768,000 B), ch7 `output_mask_f` (768 B).
  All four sizes differ, so the size match is not ambiguous.

`tools/hwx_ports.py` (`derive_program`) writes [ports.json](ports.json):
0 exceptions, 0 ambiguity groups. The task DMA reads slots 4 and 5 (srcA, one
task each), writes slot 6 (3 tasks, largest offset 1,024 B) and slot 7, and
uses the scratch slot 3 in 2,275 tasks and the constants slot 1 in 2,038
tasks. Dry run:

```
opref 6:1:2,3:64,4:5,5:6,6:4,7:7
io 5:5:16384,6:770048,4:491520,7:16384,64:61456384
```

## Two tool fixes

1. **LC 0x40 names longer than 8 bytes.** The HWX names a tensor in an
   LC 0x40 record. A name longer than 8 bytes makes the record 0x28 or 0x30
   bytes long. `hwx_ports.py` and the converter read only 0x20-byte records,
   so no slot of this program had a name. Both now read the name to the end
   of the record. The 38 Qwen tables regenerate byte-identical. New test:
   `tests/test_hwx_ports.py::test_parse_hwx_reads_lc40_names_longer_than_8_bytes`
   (fails on main, passes here).
2. **Task count on the port-table path.** libane split the task stream into
   a stack array of `ANE_M2_MAX_CALLS` (128) tasks on both build paths, so
   `ane-run --ports` refused this program ("more tasks than the builder
   envelope holds"). The bound belongs to the derived build, which keeps one
   ref record per task. The port-table build uses one ref set for every task,
   so it now takes the task list from the header's taskCount. The
   `--dry-run` output of all 38 Qwen tables is identical before and after the
   change. New `ane-selfcheck` case: matvec's two tasks repeated 100 times
   build through a port table (fails on main, passes here).

## Device run

Stock kernel `7.1.13-3-1-ARCH`, module sha256 `a584a967…` (BO cap), boot
`40f95214`, `hello_wait_ms=0`, `call_settle_us=1000`, `fw_perf_mode` N.
`ane-run` built on the M2 from this branch (gcc 16.1.1, -Werror, no warnings),
sha256 `0563c81edb02…`. Each call:
`flock /var/tmp/ane-run.lock timeout 120 ane-run --anec program-0.anec --ports ports.json ...`,
packed and unpacked by `tools/qwen_prog_run.py`. The 20-call processes ran
inside a GPU turn, so no GPU job ran during the timing.

- `bo_total_bytes` 330,219,520 before, 841,744,384 after the first load, and
  unchanged by the next two processes (the program-cache hit frees their
  section BOs).
- Exec ms, process 1 (1 call): 254.492. Process 2 (20 calls): min 254.425,
  median 254.497, max 254.633. Process 3 (20 calls): min 254.405, median
  254.517, max 254.675.
- Kernel log: no new line from the ANE driver; 3 unrelated firewall lines.

`tools/parakeet_encoder_whole.py` makes the comparison and the decode on the
host. The decoder is the NumPy BNNS-contract decoder, joint and greedy TDT of
`tools/parakeet_encoder_islands.py`, with the pinned tokenizer.

## Speed

| Run | Time |
|---|---|
| M2 (T6021), Linux, this receipt: whole-encoder ANE exec per call | 254.4 ms min |
| M1 (T8103), Linux, the same program compiled for h13: ane_exec_ms | about 139.4 ms |
| M2, macOS 27.0, CoreML encoder predict, ANE units (1,346 ANE + 28 CPU ops) | 90.62 ms min, 90.73 ms median ([2026-09-24-m2-macos-denominators](../2026-09-24-m2-macos-denominators/encoder.jsonl)) |

The M2 under Linux is 2.8 times slower than CoreML on the same machine under
macOS, and 1.8 times slower than the M1 under Linux. The numerics are correct,
so the gap is speed only. The macOS row is a different program (the macOS 27
compiler, not the cross-compiled HWX) and includes CoreML's host work, so it is
not a like-for-like engine number. Inference, not measured: the ANE runs at a
lower operating point under Linux. Section 19 of the findings shows that the
M2's pmgr has no ANE perf-state path, and the firmware perf-mode property did
not change T6021 exec times (2026-10-01 A/B: no ratio at or below 0.85).

## Limits

- One fixture, one boot. The sequence length is fixed at 3000 frames.
- The decoder, joint and tokenizer run on the host CPU.
- The M2 macOS transcript mismatch (107 vs 104 tokens,
  [MISMATCH.txt](../2026-09-24-m2-macos-denominators/MISMATCH.txt)) is still
  open. This run shows that the M2 ANE gives the golden bits with the
  cross-compiled program, so the mismatch is in what macOS 27 runs, not in the
  engine (inference).

## Next lever

The ANE operating point on T6021 (the 254 ms vs 91 ms gap). A discriminator:
time this same ANEC and Qwen program 6 under macOS on the M2 with the private
runtime, then compare per-task time with the Linux numbers here.

Private record: entry `entries/ParakeetFull/20261001T062400Z-…-encoder-whole.md`
and `artifacts/ParakeetFull/` (logs, port table, output tensors, SHA256SUMS).
