# T6021: the ten Linux-only P-1 engine writes skipped on the M2, and the DART DVA window read (2026-10-01)

Status: rejected. The test parameter `p1_groups` stays on branch `agent/m2-p1-groups` (`46636d3`) and is not
merged: it showed no effect, as `af_bridge_macos` before it.

## Result

P-1, the first step of our firmware start, writes 12 words in the ANE engine page at 0x284000000. The macOS 13.5
hv trace never writes ten of them (rank 4 of
[2026-10-01-t6021-macos-vs-linux-mmio](../2026-10-01-t6021-macos-vs-linux-mmio/README.md)). A boot that skips all
ten starts the firmware (`booted=1 fw_alive=1`), passes every correctness check, and runs the encoder at
**254.147 ms**, against **254.349 ms** and **254.121 ms** on the default boots before and after (minimum of 20
block minimums, 16 calls per block). The drop is +0.08%, far below the 3% band, so the hypothesis "the T8103
values in these ten words slow the ANE" is **rejected**. By the pre-registered rule no per-group arm ran.

On the skip boot nothing writes the ten words, so a guarded probe read gives the state the firmware sees without
P-1. Five of the ten (0x600, 0x738, 0x798, 0x7f8, 0x900) read 0 with and without P-1: the P-1 writes leave no
readable value there. The other five hold a different value only with P-1.

Step 1, the DART DVA window words 0x300-0x310, is an offline analysis plus read-only probe reads:
[step1-dva-window.md](step1-dva-window.md). They are ADT tunables of all three ANE DARTs. Their values are bit 0
of 0x300 and the bounds of the IOMapper range [0x100_0000_0000, 0x400_0000_0000) [INFERENCE: an enable bit and a
DVA window]. Under Linux bit 0 reads 0 on the two bulk DARTs, and all BO IOVAs lie below 4 GiB. The file lists
what BO IOVAs inside the window would need.

## Question and decision rule

The rule was written before the first device access (private notebook entry, 21:58Z):

- Arms, one boot each, same ESP, kernel and tree: A0 = default (release module 0.4.0, no option file); S =
  `p1_groups=0x1f` (all ten words skipped); A-after = default.
- Groups (bit, words): A 0x1 = 0x038, 0x03c; B 0x2 = 0x600; C 0x4 = 0x738, 0x798, 0x7f8; D 0x8 = 0x900; E 0x10 =
  0x410, 0x420, 0x430. 0x000 and 0x400 are written in every arm.
- Encoder minmin of S against A0: a drop of 15% or more (216 ms or less) supports the hypothesis; 3-15% is
  partial (then one boot per group); below 3% rejects it, and the run stops before the per-group arms. medmed
  must fall in the same band. A-after must lie within 1% of A0.
- If S does not boot the firmware or fails the first gate, the result is "the boot needs one of the ten words",
  and one boot per group finds which.
- Correctness in every arm: gates add, mul and matvec 2048x5120 PASS; 21 encoder processes bit-exact (fp16 sha256
  `fca96f13…`); prog_020 t15 rel L2 0.00117; prog_006 10 of 10 outputs byte-identical to the NativeVsCross cross
  outputs; a 60 s four-worker burst with 0 fail; 0 new bad kernel lines.

## The parameter and the proof that the default is unchanged

`p1_groups` (uint, 0444, default 0) is a load-time parameter of `ane_t6021` (`ane_t6021_boot.c`, passed to the
boot core as `cfg.p1_skip`). The P-1 table in `ane_t6021_boot.h` gets a group column. A skipped word logs
`P-1 write SKIPPED (p1_groups)` in place of its write. Bits outside 0x1f refuse the boot before any write.

The default path issues the same MMIO sequence as main (`scripts/offline-proof.sh`, `logs/offline/`):

- The io trace of `ane_t6021_boot_run` (every read, write, phase and poll call; 30 configurations, the AfBridgeRun
  `default-trace.c`) is identical for the main header, the new header, and the new header with `p1_skip = 0`:
  1,252 lines, 468 writes, sha256 `2fcf7d86…`, the same hash the AfBridgeRun receipt recorded. With
  `p1_skip = 0x1f` the trace loses exactly the ten writes in each of the 20 configurations that reach P-1.
- `tools/h14_boot_regression.c` from main gives byte-identical output with both headers (105 of 105). The branch
  version passes 111 of 111; the six new checks compare each mask with a group list written in the test. A copy
  of the header with 0x410 moved to group D fails two of them.
- The driver sources are equal in the 0.4.0 release tree `9f37b47` and main `547ae7c`, so the test module differs
  from the release module only by `46636d3`. A W=1 build on the M2 gives the same six warning lines as main, none
  in the changed files.

The option was one-shot, as in AfBridgeRun: a modprobe.d `install` line deleted its own file and ran `sync`
before it loaded the module with `p1_groups=0x1f`. A reset during P-1 would have given a default load on the next
boot. There was no reset.

## Arms

Device: M2 Max (T6021), stock `7.1.13-3-1-ARCH`, disk boot, os-fw 13.5, ESP unchanged in every boot. Release
module 0.4.0 (sha256 `54c1da56…`); test module 0.4.0-p1g-46636d3 (sha256 `e865dec3…`). Each arm ran about 45 s
after boot in one GPU-idle ticket, every ANE call under the ANE lock.

| arm (boot) | P-1 writes | encoder minmin / medmed | prog_020 minmin / medmed | prog_006 minmin / medmed |
|---|---|---|---|---|
| A0, default | 12 | 254.349 / 254.531 | 4.758 / 4.829 | 10.584 / 10.642 |
| S, `p1_groups=0x1f` | 2 (0x000, 0x400) | 254.147 / 254.456 | 4.764 / 4.829 | 10.519 / 10.631 |
| A-after, default | 12 | 254.121 / 254.349 | 4.755 / 4.827 | 10.570 / 10.634 |

All times in ms per CALL (20 blocks of 16 CALLs; `logs/analysis-all.txt`). S against A0: encoder minmin +0.08%,
medmed +0.03%: **rejected**. A-after against A0: -0.09% / -0.07%, inside 1%. S lies between the two default arms.
prog_020 moves 0.1% or less; the prog_006 minmin of S is 0.6% lower, with the medmed within 0.1%.

Every arm passed every correctness item: gates add, mul and matvec 2048x5120 PASS (5120/5120 bit-exact); 21 of 21
encoder processes bit-exact; prog_020 t15 max abs 0.0007324219, rel L2 0.001174227; prog_006 10 of 10 outputs
byte-identical; burst 13,234-13,301 runs with 0 fail; 0 bad kernel lines (`logs/*/console.log`).

On the S boot (`logs/S/boot-p1.txt`) the kernel logs `P-1 skips groups 0x1f`, then P-1a and P-1d `write done`
and ten `write SKIPPED` lines, and `run returned 0 (booted=1 fw_alive=1)` 0.4 s after P-1. The firmware boot does
not need the ten words.

## The ten words with and without P-1

`ane_afbridge_probe` (branch `46636d3`: the 26 bridge words plus the ten P-1 words; read only, island guard
before each read) ran on the A0 and the S boot, after the firmware start.

| offset | group | P-1 writes | read after P-1 (A0) | read without P-1 (S) |
|---|---|---|---|---|
| 0x038 | A | 0x00050020 | 0x00000020 | 0x00000000 |
| 0x03c | A | 0x000a0030 | 0x000a0030 | 0x0000ffff |
| 0x600 | B | 0x01ffffff | 0x00000000 | 0x00000000 |
| 0x738 | C | 0x00200020 | 0x00000000 | 0x00000000 |
| 0x798 | C | 0x00100030 | 0x00000000 | 0x00000000 |
| 0x7f8 | C | 0x0100000a | 0x00000000 | 0x00000000 |
| 0x900 | D | 0x00000101 | 0x00000000 | 0x00000000 |
| 0x410 | E | 0x00001100 | 0x00001100 | 0x00000100 |
| 0x420 | E | 0x00001100 | 0x00001100 | 0x00000100 |
| 0x430 | E | 0x00001100 | 0x00001100 | 0x00000100 |

The 26 bridge words read the same on both boots (the AfBridgeRun S1 values). AfBridgeRun could not read these
words before P-1, because the islands are off until the driver raises them. The S boot gives the same answer
inside the guard: the driver raises the islands and then writes nothing there.

[INFERENCE] Groups B, C and D are write-ignored or self-clearing on T6021: the m1n1 T8103 values have no
readable effect. Groups A and E change stored values, and the change does not move the speed.

The DART probe (read only) on the same two boots found all 177 words equal except the boot-variable BRD/BWR words
0x308 and 0x310 (`logs/*/ane_dart_probe.log`).

## What this shows and what it does not

- The ten Linux-only P-1 words do not explain the 254 ms vs 89 ms gap. With the bridge words
  ([2026-10-01-t6021-af-bridge-run](../2026-10-01-t6021-af-bridge-run/README.md)) and the bulk-DART tunables that
  apply after attach ([2026-10-01-t6021-dart-tunables](../2026-10-01-t6021-dart-tunables/README.md)), every
  engine-page and DART word of the 13.5 trace that Linux can write after attach is now tested. Not tested: DART
  0x20c (it must go in at DART init) and the 0x300-0x310 window (it needs BO IOVAs inside the window).
- S is a correct boot at the default speed. So P-1 could drop the ten words, but that changes the release boot
  sequence on the evidence of one boot. This receipt does not make that change.
- The probe read the words once per boot, after the firmware started. A firmware write to them is not excluded.

## Commands

    # offline proof (any checkout with both commits)
    bash receipts/2026-10-01-t6021-p1-groups/scripts/offline-proof.sh [OUTDIR]
    # build on the M2 (nice 19), from git archive 46636d3 (ane/t6021, ane/src/uapi)
    make -C /usr/lib/modules/7.1.13-3-1-ARCH/build M=$PWD/ane/t6021 ANE_VERSION=0.4.0-p1g-46636d3 W=1 modules
    make -C /usr/lib/modules/7.1.13-3-1-ARCH/build M=$PWD/ane/t6021/probes W=1 modules
    # per boot: one GPU ticket for the window (probes, then the arm)
    gpu-turn -m 25 -- bash window.sh A0            # S: same; A-after: window.sh A-after noprobe
    bash install-p1g.sh 0x1f                       # before the S reboot
    bash restore-default.sh                        # before the A-after reboot
    python3 analyze.py A0=RUN_A0 S=RUN_S A-after=RUN_A_AFTER

## Limits

- One M2, one boot per arm, one fixture per program. The arms ran in sequence on separate boots, not interleaved.
- The per-group arms did not run (rule). A group effect that cancels another group's effect is not excluded; the
  read table makes it unlikely for B, C and D, which change no readable value.
- The test shows that the ten words do not change the speed. It does not decode what the registers do.

## Files

| file | content |
|---|---|
| `step1-dva-window.md` | step 1: the window words, ADT against Linux reads on five boots, and what BO IOVAs in the window would need |
| `logs/A0`, `logs/S`, `logs/A-after` | per arm: `console.log` (checks), `enc.summary`, `prog_020.blocks`, `prog_006.blocks`, `boot-p1.txt` (P-1 and firmware boot lines); A0 and S: `ane_afbridge_probe.log`, `ane_dart_probe.log` |
| `logs/analysis-all.txt` | `analyze.py` over the three arms: statistics, the verdict, the drift check |
| `logs/offline/` | regression outputs (new; main on the old and the new header), the proof log, the trace hashes |
| `scripts/` | `offline-proof.sh`, `window.sh`, `ab-turn.sh` (the AfBridgeRun harness; changes: output directory and the `p1_groups` state line), `install-p1g.sh` (one-shot option), `restore-default.sh`, `analyze.py` |
| `SHA256SUMS` | sha256 of every file here |
