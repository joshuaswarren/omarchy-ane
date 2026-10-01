# T6021: the M2's own native encoder program under the Linux driver (2026-10-01)

## Result: H_driver

The M2's native H14 build of the whole Parakeet encoder runs under our Linux
driver in **253.10 ms**. The Mac Studio's cross-compiled build runs in
**254.27 ms** in the same session. The native build gives the same output bits.
macOS 27 runs the native build in 89.3 ms
([2026-10-01-t6021-native-macos](../2026-10-01-t6021-native-macos/README.md)).
So the compiled program is not the cause of the 2.8x gap. The cause is on the
Linux side: the driver, the firmware build, or the ANE operating point.

## Question and decision rule

The rule was written before the run (private notebook entry, 17:07Z).

- Arm X (control): the encoder ANEC from the Mac Studio h14 cross-compile
  (HWX 450,920,448 B, 3,597 tasks, ANEC sha256 `82ce8a1a…`).
- Arm N: the encoder ANEC from the M2's own compile under macOS 27
  (HWX `6430da18…`, 450,904,064 B, 3,597 tasks, ANEC sha256 `6c53d8c5…`).
  The two port tables have the same names, slots, shapes, strides and
  buffer ids. The task stream (157 re-sized regions, 960 B shorter), the
  register payloads and the weight tiling are different.
- Metric: ane-run `--time` exec ms per CALL. One block is one process with
  `--repeat 16`. Each arm has 20 blocks, run X N X N in one session, so drift
  cancels. minmin = min of the block mins; medmed = median of the block
  medians. R = minmin(N) / minmin(X).
- H_compile: minmin(N) <= 130 ms. H_driver: R >= 0.95. Mixed: between the two.
  The medmed ratio must fall in the same band.
- Validity: every process bit-exact with the golden (fp16 sha256
  `fca96f13…`), X within 254.4 +- 1.5 ms, 0 error blocks, 0 new bad kernel
  lines.

## Run

- Device: M2 Max (T6021), stock `7.1.13-3-1-ARCH`, disk boot `65d832d5`,
  release module 0.4.0 (sha256 `54c1da56…`, srcversion
  `1FEE1B2BCF064C6109E1904`), `trace_td` N, `fw_perf_mode` N,
  `call_settle_us` 1000. Userspace from the release tree `9f37b47`
  (`tools/ane-run` sha256 `f687a7ab…`, `tools/qwen_prog_run.py`).
- One `gpu-turn` ticket, 17:11:38-17:15:21Z, so no GPU job ran. Each ane-run
  ran as `flock /var/tmp/ane-run.lock timeout 120`. Script: [turn.sh](turn.sh).
- The native ANECs and port tables went to the M2 as a stream. The sha256 was
  equal on both ends for all six files.
- No reboot, no module change, and no `/sys` write.

## Encoder

| Arm | Blocks | minmin (ms) | median of block mins (ms) | medmed (ms) | Golden |
| --- | ---: | ---: | ---: | ---: | --- |
| X, cross-compiled | 20 | 254.274 | 254.333 | 254.478 | 20/20 bit-exact |
| N, M2 native | 20 | 253.095 | 253.129 | 253.201 | 20/20 bit-exact |

- R = 0.9954 and the medmed ratio is 0.9950. Block by block, the N block
  median divided by the X block median just before it is 0.9948-0.9952, so
  the window had no drift.
- The two single-call correctness processes before the blocks gave 254.476 ms
  (X) and 253.147 ms (N). All 42 processes give hidden fp16 sha256
  `fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063`, the
  golden.
- Decision rule: minmin(N) 253.095 > 130 and R >= 0.95, so the result is
  **H_driver**. All validity items hold (X anchor 254.274 ms, 0 error blocks,
  0 new kernel lines other than `UFW BLOCK`).
- Of the 165.0 ms gap between X and macOS (89.3 ms), the native program
  recovers 1.18 ms (0.7%). The other 99.3% is on the Linux side. The macOS
  number was taken at a load average of 25, so it is an upper bound, and 99.3%
  is a lower bound.

## Qwen prog_020 and prog_006

The same protocol, 20 blocks per arm, with the step-11 golden inputs bound by
port name in table order.

| Program | Tasks X / N | X minmin / medmed (ms) | N minmin / medmed (ms) | R | medmed ratio | Outputs N vs X |
| --- | --- | --- | --- | ---: | ---: | --- |
| prog_020 | 20 / 19 | 4.776 / 4.870 | 4.790 / 4.866 | 1.0029 | 0.9993 | byte-identical (1/1) |
| prog_006 | 120 / 119 | 10.618 / 10.677 | 10.583 / 10.675 | 0.9967 | 0.9998 | byte-identical (10/10) |

- prog_020 t15 against the M1 golden: rel L2 0.001174, as in the 0.4.0 gate.
- prog_006 against the M1 golden: rel L2 0 to 0.0027 on all 10 outputs, both
  arms. Every same-shape port group of this table (inputs t0/t2, t22/t27,
  t28/t44, t32/t39; outputs t106/t97, t108/t87/t91/t93, t30/t46) agrees with
  the golden under the name binding.

## Per-task timeline

Not taken. The rule (written before the run) took a `trace_td` timeline of
the native encoder only if R <= 0.90. R is 0.9954. The cross timeline is in
[2026-10-01-t6021-trace-td](../2026-10-01-t6021-trace-td/README.md).

## What this changes

- The trace receipt left two causes for the linear layers: a low clock, or a
  core use that the cross-compiled program cannot raise. The native program
  has the same time, so the second cause does not apply to the cross-compile
  alone.
- The M2 runs the same program bytes 2.8x faster under macOS than under
  Linux. This is an inference: the bytes that the macOS runtime executed are
  in a cache that System Integrity Protection makes unreadable. The compile
  is deterministic (two compiles of this MIL give the same sha256), so the
  direct compile here is the best available copy.
- Linux-side candidates that this run did not test:
  - The ANE clock and DVFS state. On this boot (`65d832d5`) the live tree has
    `pmp@28e700000` (`apple,t6020-pmp-v2`) with `status = "disabled"`, so
    Linux starts no PMP coprocessor.
  - The DPE/PPT power limit. The driver sends CSNE_CMD_CH_PROPERTY_WRITE
    (command 0x001f) at one site only, with property 0x10aa
    (`ane/t6021/ane_t6021_rtclient_main.c:1565` at `9f37b47`, the release
    source of the tested module, and at `8709d67`). It never sends property
    0x1701, which the selene 13.5 firmware routes to its PPT write.
  - The firmware build. This boot's `/chosen/asahi,os-fw-version` is 13.5,
    so Linux runs the macOS 13.5 ANE firmware. macOS 27.0 (26A428) runs its
    own.

## Limits

- One boot, one session, one fixture per program. Both arms include the
  1.0-1.1 ms CALL settle.
- The macOS time comes from another boot under load.
- After the run the driver holds about 1.64 GB of cached programs
  (`bo_total_bytes` 1,635,024,896; it was 654,049,280), until the next reboot.
  The module cap is 12 GiB.

## Files

- [turn.sh](turn.sh): the device window that ran (sha256 `c559c7e0…`).
- [analyze.py](analyze.py): ratios, decision rule and output comparison.
- `logs/`: `enc.summary` (each encoder process: min, median, golden line,
  fp16 sha256), `prog_020.blocks` and `prog_006.blocks` (arm, block, min,
  median), `analysis-output.txt`, `state-start.txt`, `state-end.txt`.
- `SHA256SUMS` covers the files above.
- The private notebook holds the entry `entries/NativeVsCross/` and
  `artifacts/NativeVsCross/` (transfer log, dry runs, all process logs, the
  Qwen outputs, the encoder outputs of the correctness processes and kernel
  log lines).
