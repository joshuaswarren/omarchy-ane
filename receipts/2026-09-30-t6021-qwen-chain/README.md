# Qwen staged decode on T6021 — milestone receipt

Date: 2026-09-30. Worktree branch: agent/m2-installed-path. Private preflight and program-20 run records: under ~/.local/share/apple-silicon-lab/entries/QwenM2/.

## M0 — inventory: PASS

- The M1 contract contains 38 programs, bank hashes, and a paired lane/port/shape manifest. The hashes are not output arrays.
- The e0438 arrays in Jw16Levers8/e5rt-embed-capture match all four program-20 M1 logical fp16 hashes. The similarly named e0020 arrays from e5rt-capture3 do not match this program-20 golden and were not used.
- All 38 M1 MIL and weights.bin exports are present under /var/tmp/qwen38-staged-<M1-host>/prog_NNN/. The plan records every staged program's constant inventory, source, and weight-file size.
- The real-weight H14 ANEC set is /var/tmp/qwen-real-anec-h14/, distinct from the synthetic batch8 proxy. Program 20 has 20 tasks, 3 inputs, 1 output, and 83,892,736 constant bytes. The manifest warns that tensor/channel identity is inferred from size/order.
- The program-20 M1 MIL has inputs t7, t2, t0 and output t15. The real ANEC metadata names its input channels t15, t2, t7 and output channel t0. Shape-based channel mapping remains unverified.

## M1 — class C / program 20: FAIL

The H14 ANEC loaded and ran on T6021. The first 4-KiB inputs were rejected because the runner required 16-KiB channel surfaces; zero-padding to 16 KiB produced two completed calls. Both calls used the same ANEC. One distinct program was loaded against the 250-per-boot limit. The successful call logs show channels 5, 6, 7, and 4 at 16,384 bytes. No EXCH failure or DART fault appeared in the observed log tail.

With the shape-based input mapping (M1 t0, t2, t7 → ANEC slots 0, 1, 2), M2 output versus the M1 golden had max absolute error 0.474609375, relative L2 0.2756553426, and exact fp16 fraction 0.00048828125. Swapping the two same-shaped inputs did not help: 0.455078125 max absolute, 0.2758836011 relative L2, and zero exact fp16 elements.

The fp64 reference evaluates the M1 MIL graph using its real weights.bin constants. BLOBFILE data begins 64 bytes after each recorded offset. M1 golden versus reference: max absolute 0.0050986026, relative L2 0.0112268657, exact fp16 fraction 0.02099609375. M2 output versus reference: max absolute 0.4724890215, relative L2 0.2756774076, exact fp16 fraction 0.00048828125. The M1 golden is much closer to the reference; the M2 result is not an fp16 rounding difference.

**Stop at M1.** Resolve the converter's tensor-to-channel identity and port mapping, then rerun this one program with the M1 vectors. Do not begin M2/M3/M4 until program 20 matches within an operation-derived tolerance. The ANEC/MIL port-name inversion and the explicit heuristic warning make binding/conversion the leading hypothesis; the precise cause is not yet proven.

## M2–M4: not started

M2 requires a passing program-20 contract and a matching class-A capture. M3 requires a matching seven-part state capture. M4 requires all 38 real-weight programs, verified layouts, and a working repacking executor. No tools/qwen_m2_chain.py was added because M1 failed at the port contract.

STAGED-QWEN-REF on T6021 remains unproven. Acceptance requires all 38 stages with real GGUF-derived constants, chained for 10 prompts × 32 tokens.

## Evidence

- Inventory and per-program weight table: docs/plans/2026-09-30-qwen-on-m2.md.
- M1 vectors, ANEC, exact fp64 reference output, and numerical results: ~/.local/share/apple-silicon-lab/artifacts/QwenM2/2026-09-30-t6021-qwen-chain/prog020-classC/; its SHA256SUMS records each file hash.
- Inventory preflight artifact: ~/.local/share/apple-silicon-lab/artifacts/QwenM2/2026-09-30-t6021-qwen-chain/inventory.txt.
- After M1 failed, the lead paused all M2 device use for a stock-kernel mailbox test. No further device actions are taken until the lead releases the machine.

## Addendum — offline slot decode (2026-09-30, post-M1)

The M1 failure root cause is found without device time: the task stream binds
BAR slot 4 to input t0, slot 5 to the OUTPUT t15, slots 6/7 to t2/t7 (slot =
Apple surface-array index + 4, decoded from the HWX IOVA list). The loader's
matmul carve-out bound slot 4 -> ch4, so the kernel read the zero-filled output
channel in place of t0; the saved device output equals the fp16 MIL graph with
t0 = 0 (rel L2 0.012 vs the model, 0.276 vs the golden). Packing, feed order,
and the in-ANEC constants are proven correct. Fix and one-run recipe:
prog20-port-binding.md; runner: tools/qwen_prog_run.py
(ANE_M2_OPREFS=1:2,4:5,5:4,6:6,7:7).

## Addendum — program 20 on the port-table path (2026-09-30, 23:11Z)

### Loader fix (commit 49eb8ed)

Before this commit, `ane-run --ports` used the port table only for
`--dry-run`. A device run built the program with the derived binding
(refs `1:2 4:4 5:4 6:6 7:7`), which is the failed M1 binding. Now
`ane_m2_init_ports()` loads the program through
`ane_m2_program_build_ports()`. For program 20 the six sections and the io
table are byte-identical to the derived build with
`ANE_M2_OPREFS=1:2,4:5,5:4,6:6,7:7` (host compare, all six sections and io
`5,6,7 in / 4 out`, 16,384 B each). `qwen_prog_run.py` no longer sets
`ANE_M2_OPREFS`: the port build ignores it. The runner now wraps each call
as `flock /var/tmp/ane-run.lock timeout 60 ane-run ... --time`.

`ane-run` now refuses a port whose `surface_bytes` is larger than its
`tile_bytes`, because the task DMA could write past the io BO. 37 of the 38
generated tables fail this check; only `prog_020` passes. Example:
`prog_000` puts `t38` (393,216 B) on channel 10 (16,384 B), but the ANEC
allocates 393,216 B only on channels 6 and 12. `hwx_ports.py` now reports
such ports as exceptions, and the runner refuses tables with exceptions.

### Device run: FAIL, output all zero

Stock kernel `7.1.13-3-1-ARCH`, module `ane_t6021` SHA-256 `7b592674…`
(PR #8 `hello_wait_ms=0`, PR #9 io BO pool), the same boot as the BO pool
receipt. `ane-run` built on the M2 with gcc 16.1.1 at 49eb8ed, no warnings.
One run of `qwen_prog_run.py --prog prog_020 --repeat 2` with the M1 vectors:

| Check | Result |
| --- | --- |
| ane-run exit | 0 |
| exec ms, 2 calls | 1.280, 1.374 |
| output surface | 16,384 B, all zero |
| t15 vs M1 golden | max abs 2.14453125, rel L2 1.0, exact 0 |
| t15 vs fp64 reference | max abs 2.142410896, rel L2 1.0, exact 0 |
| new dmesg lines | 0 (no EXCH failure, DART fault, or quarantine) |

An all-zero output is not a binding signature, so the ranked binding
fallbacks in `prog20-port-binding.md` cannot discriminate. They were not
run. The 50-run determinism loop needs a pass first. It was not run.

Inference, from source, not measured: the call returned before the program
finished. With `hello_wait_ms=0`, `ane_rtclient_call_wait`
(`ane/t6021/ane_t6021_rtclient_main.c:564`) returns when the TD counter at
TM+0x20458 moves by any amount and all eight TQ status words read idle. Then
it waits `call_settle_us` (1,000 µs). Program 20 has 20 tasks and
83,892,736 B of constants. Its 1.28-1.37 ms per call equals the add latency
on this module (p90 1.29-1.42 ms). On this module, only programs with 1 or 2
tasks have run (add, mul, relu, scalar ops, clip, matvec). The multi-task
Parakeet islands and the first program-20 run used the poll-TX kernel. If
the inference is true, `--repeat 2` sent the second call while the first
was still running. Also, later firmware writes go to io BOs that the pool
keeps mapped and gives to the next process.

**Stop.** Do not run programs with more than 2 tasks on this module until
the call waits for the program's own TD count. The task count is known at
PROG_LOAD (tdprop block count). After that driver change and a reboot, run
this one program again with the same pass criteria (rel L2 <= 0.02 against
the golden and the fp64 reference).

Programs 0 and 1 were not run. Their port tables fail the surface check, and
the port build accepts only one output (program 0 has 7 outputs, program 1
has 2). M1 inputs and goldens exist for both: captures e0418 and e0419, which
match `goldens.json` by SHA-256.

Private record: entry `entries/Prog20Run/20260930T230934Z-…-prog020-ports.md`
and `artifacts/Prog20Run/2026-09-30-prog020-ports/` (MANIFEST.txt,
SHA256SUMS).
