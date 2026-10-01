# T6021: the ANE DART tunables on the M2, read and applied (2026-10-01)

Status: E1 done. E2 (live write) stopped on DART faults. E2' (the macOS
order, one group at a time) done: every tunable except 0x20c applies cleanly
and does not change the speed; 0x20c breaks translation. The probe is on
main and is read only by default.

## Result

**E1, read only.** Linux leaves the two bulk DARTs of the ANE at their
reset values. On dart-ane1 (BRD) and dart-ane2 (BWR), 0 of the 22 ADT tunable
words hold the macOS value. dart-ane0 (LLT) already holds all 6 of its macOS
values. Something before Linux writes them; Linux does not. The m1n1-named
DART PERF counters read 0 and do not count: 21 encoder CALLs change none of
the 177 words read. So this run cannot give the TLB misses per CALL.

**E2, write.** The probe wrote the 19 macOS words on each bulk DART (38
read-modify-writes, every readback equal) while the ANE was idle. The next
CALL failed. Both bulk DARTs reported `translation fault ... stream:0
code:0x8 (NO PTE FOR IOVA)`, and the gate `add` alternated between an
all-zero output and an exact output: trials 1 and 3 and the reopen run
failed, trials 2 and 4 passed. This is a pre-registered stop condition. The
run wrote nothing after the fault, took no B timing, and rebooted. On the
default boot the 38 words read their reset values again, and the gates,
the encoder (254.286 ms) and the other checks pass.

**E2', write in the macOS order, by group.** Per bulk DART the probe now
disables the streams, flushes all TLBs, writes the group, flushes again and
enables the streams as read. The procedure alone (G0), 0x220/0x224 (G1), the
32 SID words (G2) and G1+G2 together (36 words) give 0 faults, pass every
check, and do not change the speed: encoder minmin drop +0.00% to +0.03%
(about 254.3 ms in every arm), prog_020 and prog_006 within 0.2%. Verdict:
**rejected** for all of them. 0x20c alone breaks translation even in this
order: one `NO PGD FOR IOVA` fault on BRD stream 0, then the CALL hangs and
every later program load times out until a reboot, also after 0x20c is
written back. So the run did not apply G4 (all 19 words), which contains
0x20c.

The hypothesis "the DART tunables cost time" is rejected for 0x220, 0x224 and
the SID words. For 0x20c it is not tested: on a live, attached DART that
word stops the ANE.

## Question and decision rule

The input is step 4 of
[2026-10-01-t6021-macos-vs-linux-mmio](../2026-10-01-t6021-macos-vs-linux-mmio/README.md)
(E1 and E2) and its `dart-tunables.tsv` (ADT masks and values, equal to the
macOS 13.5 hv trace and the 26.6.2 j414c iBoot tables). The rule was written
before the first device access (private notebook entry, 19:16Z):

- E1: read 0x000-0x00c, 0x20c, 0x210, 0x220-0x22c, 0x300-0x310, the PERF
  words and 0x800-0x83c on all three DARTs, idle and after 1 and 20 encoder
  CALLs. Applied means `(read & mask) == value`.
- E2: A (default), apply, B, write back the E1 values, A again, all on one
  boot. Encoder minmin of B against the first A: a drop of 15% or more
  (216 ms or less) supports the hypothesis, 3-15% is partial (then bisect
  0x20c / 0x220-0x224 / the SID words), below 3% rejects it. The second A
  must lie within 1% of the first. A DART fault, a failed CALL, a readback
  mismatch or a reset stops the run: record the last write, reboot if
  needed, no retry.

## E1: what Linux leaves in the three ANE DARTs

Device: M2 Max (T6021), stock `7.1.13-3-1-ARCH`, disk boot, release module
0.4.0 (sha256 `54c1da56…`), userspace from the release tree `9f37b47`. The
ANE device uses stream 0 on each of the three DARTs (DT `iommus`).

The probe ([../../ane/t6021/probes/ane_dart_probe.c](../../ane/t6021/probes/ane_dart_probe.c),
built on the M2, sha256 `3227a050…`) read 177 of 177 words four times: twice
on the idle ANE, after one encoder process with `--repeat 1` (254.307 ms),
and after one with `--repeat 20` (min 254.243 ms). All four reads are equal
word for word (`logs/dart-check.txt`), and netconsole carried the same values.

| offset | macOS mask / value (ADT) | LLT | BRD | BRD after E2 apply | BWR | BWR after E2 apply |
|---|---|---|---|---|---|---|
| 0x20c | 0xff0000b7 / 0xe40000b7 | 0xe40000ff yes | 0x1e000048 no | 0xe40000ff yes | 0x1e000048 no | 0xe40000ff yes |
| 0x220 | 0x000f0f0f / 0x000f0f0f | 0x000f0f0f yes | 0x00020202 no | 0x000f0f0f yes | 0x00020202 no | 0x000f0f0f yes |
| 0x224 | 0x00ffffff / 0x00080808 | 0x00080808 yes | 0 no | 0x00080808 yes | 0 no | 0x00080808 yes |
| 0x300 | 0x00001f31 / 0x00000001 | 0x00000001 yes | 0x00001e10 no | not written | 0x00001f30 no | not written |
| 0x308 | 0x3ffffffc / 0x10000000 | 0x10000000 yes | 0x3ffffdac no | not written | 0x2fedebfc no | not written |
| 0x310 | 0x3ffffffc / 0x3ffffffc | 0x3fffffff yes | 0x3f6cfbcb no | not written | 0x3e3fedff no | not written |
| 0x800, 0x804 | 0x000f007f / 0x00060000 | 0 (no ADT word) | 0x000f0000 no | 0x00060000 yes | 0x000f0000 no | 0x00060000 yes |
| 0x808-0x810 | 0x000f007f / BRD 0x00030040, BWR 0x00010040 | 0 (no ADT word) | 0x000f0000 no | 0x00030040 yes | 0x000f0000 no | 0x00010040 yes |
| 0x814-0x83c | 0x000f007f / BRD 0x00010048, BWR 0x00010040 | 0 (no ADT word) | 0x000f0000 no | 0x00010048 yes | 0x000f0000 no | 0x00010040 yes |

The other words are equal on all three DARTs except PARAMS: DIAG_LOCK
(0x210) 0, 0x228 0x3f3f3f3f, 0x22c 0x01010101, ENABLE_STREAMS[0] (0xc00)
0xffff. PARAMS 0x000 / 0x004 / 0x008 / 0x00c: LLT 0x1e313020 / 0x31221403 /
0x2a2a0200 / 0x00010010; BRD 0x2e313048 / 0x30110023 / 0x2a2a0200 /
0x00100010; BWR 0x2e313044 / 0x30110023 / 0x2a2a0200 / 0x00100010.

After the reboot that ended E2, the 38 BRD/BWR tunable words read the E1
values again (`logs/post-rPost.log`). Only 0x300-0x310 differ from the
first boot: BRD 0x1d10 / 0x3fff7da4 / 0x3f6cfdcf, BWR 0x1f30 / 0x2bedebf4 /
0x3f3fedff.

Notes:

- The receipt's prediction holds for BRD and BWR and fails for LLT. LLT
  already carries the six macOS 27.0 ADT values (0x20c and 0x310 also equal
  the 13.5 trace results), so step E5 has nothing to apply on this boot.
  Linux `apple-dart` writes no tunable. Which earlier stage writes the LLT
  words is not known [INFERENCE: iBoot or m1n1; LLT sits in the `pmp` power
  domain, which is on at handover, while BRD and BWR sit in `ane_cpu`, which
  is off at handover]. The BRD/BWR 0x300-0x310 words change from boot to
  boot, so no one programs them [INFERENCE: power-on patterns].
- The reset value of every BRD/BWR SID word is 0x000f0000: field [19:16] is
  0xf where macOS writes 6, 3 or 1, and field [6:0] is 0 where macOS writes
  0, 0x40 or 0x48. What the fields do is not known.

### PERF counters

m1n1 names 0x700 / 0x704 PERF_INTR_ENABLE / STATUS, 0x720-0x75c two unknown
banks, and 0x760-0x788 TLB, ST and CTC miss / fill / hit counters
(m1n1 `0b1c9d98b709` `proxyclient/m1n1/hw/dart8110.py:216-232`, marked
"completely guessed, unverified"; Linux `apple-dart.c` has no PERF
register). All of these read 0 on all three DARTs
before and after 21 encoder CALLs (`logs/dart-check.txt`). So under Linux the
counters do not count. 0x700 reads 0 [INFERENCE: counting needs an enable
bit that no one sets]. E1 is read only, so it did not write 0x700, and the
miss count per CALL stays unknown. The receipt's two TLB models are not
separated by this run.

[INFERENCE from the m1n1 PARAMS field names, `dart8110.py:11-29` at
`0b1c9d98b709`] TLB_SET_COUNT (PARAMS_0 [11:0])
is 32 on LLT, 72 on BRD and 68 on BWR, and LOG2_NUM_WAYS (PARAMS_4 [30:28])
is 3 on all three. If the names are right, the TLBs hold about 256, 576 and
544 entries. 576 entries of 16 KiB cover 9 MiB, much less than the 61 MB
activation working set of one encoder CALL (3,723 pages). PARAMS_4 bit 5
(SUPPORT_TLB_PREFETCH) is set on BRD and BWR only.

## E2: the bulk-DART tunables applied on a live DART

Window (one GPU-idle ticket, boot unchanged throughout):

1. A-before: the AfBridgeRun harness (`scripts/ab-turn.sh`; only change: the
   output directory). Gates add, mul and matvec 2048x5120 PASS (5120/5120
   bit-exact); 21 encoder processes bit-exact (fp16 sha256 `fca96f13…`);
   prog_020 t15 rel L2 0.001174227; prog_006 10 of 10 outputs
   byte-identical to the NativeVsCross cross outputs; burst 13,091 runs,
   0 fail; 0 bad kernel lines. Timing: encoder minmin **254.197 ms**, medmed
   254.387 ms; prog_020 4.764 / 4.810 ms; prog_006 10.572 / 10.607 ms
   (`logs/a-before-*`).
2. Apply (`apply=1`, under `/var/tmp/ane-run.lock` with no ane-run present):
   38 of 38 RMWs in the macOS trace order, each read back equal
   (`logs/e2-apply.log`). The 0x300-0x310 window and dart-ane0 were not
   touched.
3. Read (`logs/e2-readB.log`): exactly the 38 written words differ from E1;
   LLT and the PERF words are unchanged.
4. B: the first step, gate `add`, failed within 0.5 s (the first fault
   came 12.5 s after the last write, `write 0x28582083c 0x000f0000 ->
   0x00010040`). Trials 1 and 3 and the reopen run gave 0 of 512 lanes (all
   16,384 output values zero); trials 2 and 4 gave 512 of 512
   (`logs/b-gate-add.log`). The kernel logged 10 faults in 37 us, the
   rate-limit burst of `dev_err_ratelimited` (omarchy-linux `57f8f6deaa3a`, on
   asahi-7.1.13-3, `drivers/iommu/apple-dart.c:1292`), so the
   faults of the later failed trials are not in the log (`logs/b-console.log`):

        apple-dart 285820000.iommu: translation fault: status:0x900c0008 stream:0 code:0x8 (NO PTE FOR IOVA) at 0x9f9c0000
        apple-dart 285810000.iommu: translation fault: status:0x800c0008 stream:0 code:0x8 (NO PTE FOR IOVA) at 0x9f990000
        (8 more: BWR 0x9f9c0d00 0x9f9c1380 0x9f9c18c0 0x9f9c1e00, BRD 0x9f9910c0 0x9f991640 0x9f991b80 0x9f992080)

   The window stopped (`logs/e2-console.log`). It did not run the write-back
   or the second A arm.

What the fault says, and what it does not:

- With the macOS values in place, the bulk DARTs miss PTEs for IOVAs below
  4 GiB that the default configuration translates (the same gate passed
  13,091 times in A-before with 0 faults). The faults come on both the read
  DART and the write DART, stream 0, and every second process fails.
- [INFERENCE] macOS writes these words at DART init, after a TLB flush and
  before TTBR and ENABLE_STREAMS (trace events 2856-2916). This run wrote
  them on a live DART with a valid TTBR, enabled streams and warm TLB state,
  and issued no flush. The alternating pattern fits a translation cache that
  the tunables enable and that the Linux per-stream flush (`TLB_CMD` op
  FLUSH_SID, `apple-dart.c:546-549` at `57f8f6deaa3a`; m1n1 names the
  STT/CTC flush bits 12 and 13 of that word, `dart8110.py:65-80`) does not
  clear: a stale entry fails one
  process, the fault clears it, and the next process passes. This is not
  shown; the run could not test it under its stop rule.
- So the result does not show that the tunables are wrong for Linux. It
  shows that a live write of the full set, without a flush, breaks
  translation.

## Back to the default

The run rebooted the M2 with a plain `systemctl reboot` (10-minute notice,
GPU lane idle, sync and 40 s first). ssh answered about 2 minutes after the
reboot command, on a new boot. Nothing was installed and no option file was
set, so this is the default boot: release module `54c1da56…` bound,
firmware `booted=1`, no `ane_t6021` option file, 0 failed units.

On that boot the read probe found the reset values (above), and one default
arm passed every check (`logs/default-post-*`): gates add, mul and matvec
2048x5120 PASS (5120/5120 bit-exact); 21 encoder processes bit-exact;
prog_020 t15 rel L2 0.001174227; prog_006 10 of 10 byte-identical; burst
12,711 runs, 0 fail; 0 bad kernel lines.

| arm | encoder minmin / medmed | prog_020 minmin / medmed | prog_006 minmin / medmed |
|---|---|---|---|
| A-before (first boot, 41 min up) | 254.197 / 254.387 | 4.764 / 4.810 | 10.572 / 10.607 |
| default after the reboot (1 min up) | 254.286 / 254.596 | 4.773 / 4.856 | 10.585 / 10.695 |

All times in ms per CALL (20 blocks of 16 CALLs). On the new boot, blocks
1-3 ran at 255.0-258.0 ms (the warm-up seen in every arm started right
after a boot); they do not set the minimum.

## E2': the macOS order, one group at a time

The E2 fault pattern fits translation state that the tunables change and that
the Linux per-stream flush does not clear. macOS writes the words at DART
init: a TLB flush (`TLB_OP` 0, trace event 2856), the tunables, and the
streams last (2915), with TTBR[0] cleared first (2853). The rule for E2' was
written before the run (private notebook, 20:05Z): the same bands per group
(15% / 3%), the next A arm within 1% of the group's own A arm, and any fault
stops that group.

The probe (`3ec8a28`, ko sha256 `50de061c…`) now does, per bulk DART: read
ENABLE_STREAMS, TCR0, TTBR0, ERROR and PROTECT; write DISABLE_STREAMS with
the enabled mask (ENABLE_STREAMS then reads 0); `TLB_CMD` = 0 (flush all) and
wait for BUSY to clear; the RMWs of the selected `groups`; flush all again;
write ENABLE_STREAMS back (read back equal). It does not touch TTBR or TCR.
Before each group: TCR0 0x9, TTBR0 0x1000fa9d (0x1000fa95 on the next boot),
PROTECT 0, ENABLE 0xffff on both bulk DARTs. Undo is `apply=2` with the E1
values in the same order.

Each window ran in one GPU-idle ticket: read, A arm, apply, read, group arm,
undo, read. Every read after an apply shows exactly the group's words
changed, and every read after an undo equals E1 except the boot-variable
0x300-0x310 (`logs/e2q-reads.txt`; the reads come from the kernel journal,
because the probe output had filled the dmesg ring).

| `groups` (words) | writes | A arm minmin / medmed | group arm minmin / medmed | drop minmin / medmed | next A drift | result |
|---|---|---|---|---|---|---|
| 0 (streams off, flush, flush, on) | 0 | 254.278 / 254.452 | 254.255 / 254.479 | +0.01% / -0.01% | +0.009% | no fault, rejected |
| 1 (0x220, 0x224) | 4 | 254.300 / 254.472 | 254.290 / 254.457 | +0.00% / +0.01% | -0.007% | no fault, rejected |
| 2 (SID words 0x800-0x83c) | 32 | 254.283 / 254.473 | 254.280 / 254.494 | +0.00% / -0.01% | +0.002% | no fault, rejected |
| 4 (0x20c) | 2 | 254.289 / 254.478 | gate FAILED | - | - | fault, ANE wedged |
| 3 (1 + 2), next boot | 36 | 254.367 / 254.591 | 254.349 / 254.522 | +0.01% / +0.03% | -0.004% | no fault, rejected |

Encoder times in ms per CALL (20 blocks of 16). Every passing arm: gates add,
mul and matvec 2048x5120 PASS, 21 encoder processes bit-exact (fp16 sha256
`fca96f13…`), prog_020 t15 rel L2 0.001174227, prog_006 10 of 10
byte-identical, burst 13,127-13,314 runs with 0 fail, 0 bad kernel lines.
prog_020 and prog_006 move by 0.2% or less (`logs/e2q-analysis.txt`).

The 0x20c group (`logs/e2q-g3-fault.txt`): the apply wrote 0x1e000048 ->
0xe40000ff on both bulk DARTs, read back equal, streams back to 0xffff. The
first gate CALL, 12 s later, gave

    apple-dart 285810000.iommu: translation fault: status:0x800c0002 stream:0 code:0x2 (NO PGD FOR IOVA) at 0xfd68c580
    ane_t6021 284000000.ane: call completion wait failed -110

and from then on every `DRM_IOCTL_ANE_PROG_LOAD` failed with `Connection
timed out`. The undo wrote 0x20c back (readback equal); the BRD ERROR word
then read 0x00040000 (before: 0x00f00000; bit 18 is FILL_REGION in m1n1's
guessed names), and the next A arm failed the same way. The run stopped and
rebooted the M2 (10-minute notice).
The run did not apply G4 (all 19 words): it contains 0x20c, and a second
application would knowingly repeat the hang. The groups=3 window replaced it
on the next boot, and a final A arm (254.357 / 254.529 ms) passed.

Protocol note (review finding): after the group fault the window still
loaded the probe for the undo (DART register writes only), and the next
window's A arm submitted work to the hung queue. That arm only confirmed the
hang and is not used as evidence. A hung ANE queue is untrustworthy until
reboot (AGENTS.md), so a later run should stop at the first failed group arm
and reboot before any other probe load or submission. `scripts/e2q-window.sh`
is kept as it ran.

What this shows and what it does not:

- 0x220/0x224 and the SID words are safe to apply after attach in this order
  and have no measurable effect on the ANE speed.
- 0x20c changes how the BRD walker reads the page tables [INFERENCE: `NO PGD`
  means the walk found no top-level entry for an IOVA that the default setup
  translates]. Two readings remain. (a) Walker state built under the old
  value stays stale: then the macOS order with TTBR cleared during the write
  would work. (b) The value selects a table format or walk mode that the Linux
  `io-pgtable-dart` tables do not match: then no write order helps. This run
  cannot tell them apart.

## Where 0x20c can be applied

- **Unbind and rebind of apple-dart: not possible.** The driver is built in
  (`CONFIG_APPLE_DART=y` in the running kernel) and sets
  `.suppress_bind_attrs = true` (omarchy-linux `57f8f6deaa3a`
  `drivers/iommu/apple-dart.c:1640`), so sysfs has no bind/unbind. The ANE
  module cannot be unloaded either.
- **m1n1 or iBoot at boot: not realistic for BRD/BWR.** Both sit in the
  `ane_cpu` power domain, which is off at handover. Linux powers it at ANE
  probe, and the DART comes up at reset values (E1 and the reads after both
  reboots). LLT keeps its tunables because its `pmp` domain stays on.
- **apple-dart at DART init: realistic, needs a kernel build.** Apply the
  tunables from a DT property in `apple_dart_hw_reset()` after
  `apple_dart_hw_disable_dma()` and `apple_dart_hw_clear_all_ttbrs()` and
  before ENABLE_STREAMS (`apple-dart.c:552-574`). `hw_reset` runs at probe and
  on every runtime resume (`apple_dart_resume`, `:1599-1622`), so the values
  survive power cycles. The property can come from the ADT
  `dart-tunables-instance-N` (m1n1 copies it) or from our overlay. This is the
  macOS order, TTBR clear included, and it separates (a) from (b).
- **From our module, closer to macOS (not run):** the E2' sequence plus
  TTBR[0] = 0 during the write (restored after), or the flush with `TLB_CMD`
  bit 13 (ENABLE_STT_FLUSH in m1n1). This needs no kernel build, but if (b)
  holds it wedges the ANE again and needs a reboot.

## Commands

    # build (on the M2, nice 19)
    make -C /usr/lib/modules/7.1.13-3-1-ARCH/build M=$PWD/ane/t6021/probes W=1 modules
    # E1 window: probe x2, encoder --repeat 1, probe, encoder --repeat 20, probe
    gpu-turn -m 10 -- e1-window.sh OUT
    # E2 window: A-before, apply=1, read, B, (write-back apply=2, read, A-after)
    gpu-turn -m 25 -- e2-window.sh OUT
    # after the reboot: read probe, one default arm
    gpu-turn -m 15 -- post-window.sh OUT
    # E2': windows in order (each: read, A, apply groups, read, G arm, undo, read)
    e2q-run.sh ROOT 0 1 2 4 7 final     # stopped at 7 (A arm failed after the 0x20c fault)
    e2q-run.sh ROOT 3 final             # next boot
    python3 scripts/e2q_analyze.py e2q-runs
    python3 scripts/dart_compare.py ../2026-10-01-t6021-macos-vs-linux-mmio/dart-tunables.tsv LOG...

E1 and E2 ran the probe from commit `2f943fe`; `260d869` added a log line
with a 50 ms drain before the first read of each RMW and the source
citations. E2' ran `3ec8a28` (the macOS order and `groups`); its read-only
path is the same as before. `scripts/lib.sh` holds the shared steps: the
probe load under `/var/tmp/ane-run.lock` (it refuses while an ane-run runs),
the encoder run with its golden check, and the stop checks.
`scripts/e2q-window.sh` overrides the bad-line check to read the kernel
journal and to ignore the probe's own lines (its `error 0x…` lines matched
the pattern once and stopped a first W0 before its group arm; that run is in
the private record).

`apply=2 orig_brd=… orig_bwr=…` writes the given values back with the same
RMW. `apply=1` changes only the bits inside each mask, so the RMW of the
masked E1 field is the exact inverse (0x20c: 0x1e000048 -> 0xe40000ff ->
0x1e000048). E2' used it for every undo.

## Limits

- One M2 and three boots: E1 and E2 on the first; the E2 default arm and E2'
  groups 0, 1, 2 and 4 on the second; groups=3 and the final A arm on the third.
- The PERF counters gave no data. The TLB size comes from field names that
  m1n1 marks as guesses.
- The fault logs are rate-limited, so only the first failed process has
  addresses.
- E2' writes on a live, attached DART with TTBR valid. It does not test the
  write at DART init (TTBR cleared), which is the only order macOS uses.
- The run did not decode what any of the tunable fields do.

## Files

| file | content |
|---|---|
| `logs/dart-check.txt` | `dart_compare.py` over the six reads: E1 r0, r0b, r1, r20, E2 after apply (readB), and the default boot after the reboot (rPost) |
| `logs/e1-r0.log`, `logs/e2-readB.log`, `logs/post-rPost.log` | the raw probe lines of three reads |
| `logs/e1-console.log` | the E1 window (four probe loads, the 1-CALL and 20-CALL encoder runs) |
| `logs/e2-apply.log` | the 38 RMWs: value read, value written, readback |
| `logs/e2-console.log`, `logs/b-console.log`, `logs/b-gate-add.log` | the E2 window, the B arm with the fault lines, the failed gate |
| `logs/a-before-*`, `logs/default-post-*` | per arm: checks, encoder blocks, prog_020 and prog_006 blocks |
| `logs/post-console.log` | the window on the default boot after the reboot |
| `logs/e2q-analysis.txt` | `e2q_analyze.py` over `e2q-runs/`: per group, the A arm, the group arm, drops, drift, verdict |
| `logs/e2q-reads.txt` | each E2' read against E1 r0: words changed, ADT words applied per DART |
| `logs/e2q-apply-undo.log` | every E2' apply and undo (stream state, flushes, RMWs), from the kernel journal |
| `logs/e2q-g3-fault.txt` | the 0x20c apply, the DART fault, the CALL timeout, the undo |
| `e2q-runs/` | per E2' arm: console, encoder blocks, prog_020 and prog_006 blocks (and the failed gate logs) |
| `scripts/` | `lib.sh`, `e1-window.sh`, `e2-window.sh`, `post-window.sh`, `e2q-window.sh`, `e2q-run.sh`, `ab-turn.sh` (the AfBridgeRun harness), `dart_compare.py`, `e2q_analyze.py` |
| `SHA256SUMS` | sha256 of every file here |
