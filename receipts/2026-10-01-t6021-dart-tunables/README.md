# T6021: the ANE DART tunables on the M2, read and applied (2026-10-01)

Status: E1 done. E2 stopped on DART translation faults, so there is no
timing verdict. The probe is on main and is read only by default.

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

The hypothesis "the DART tunables cost time" is **not tested**: with the
macOS values applied the way this run applied them, the ANE does not compute
correctly.

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
(`hw/dart8110.py`, marked "completely guessed, unverified"; Linux
`apple-dart.c` has no PERF register). All of these read 0 on all three DARTs
before and after 21 encoder CALLs (`logs/dart-check.txt`). So under Linux the
counters do not count. 0x700 reads 0 [INFERENCE: counting needs an enable
bit that no one sets]. E1 is read only, so it did not write 0x700, and the
miss count per CALL stays unknown. The receipt's two TLB models are not
separated by this run.

[INFERENCE from the m1n1 PARAMS field names] TLB_SET_COUNT (PARAMS_0 [11:0])
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
   rate-limit burst of `dev_err_ratelimited` (`apple-dart.c:1292`), so the
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
  FLUSH_SID, no STT/CTC flush bits) does not clear: a stale entry fails one
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

## Next discriminators (not run)

In order of risk, each with the same stop rule:

1. Apply the full set live, then a FLUSH_ALL (`TLB_CMD` op 0) on each bulk
   DART, then one gate. Passing gates mean the faults came from stale
   translation state, and the timing A/B can run.
2. If 1 still faults: bisect by group (0x20c alone, 0x220-0x224, the SID
   words), one group per boot, each followed by a flush.
3. Apply the words where macOS does, in `apple-dart` before TTBR and
   ENABLE_STREAMS, from the DT tunables. That is the upstream-shaped place,
   and it needs a kernel build; m1n1 would pass the ADT values in the DT.

## Commands

    # build (on the M2, nice 19)
    make -C /usr/lib/modules/7.1.13-3-1-ARCH/build M=$PWD/ane/t6021/probes W=1 modules
    # E1 window: probe x2, encoder --repeat 1, probe, encoder --repeat 20, probe
    gpu-turn -m 10 -- e1-window.sh OUT
    # E2 window: A-before, apply=1, read, B, (write-back apply=2, read, A-after)
    gpu-turn -m 25 -- e2-window.sh OUT
    # after the reboot: read probe, one default arm
    gpu-turn -m 15 -- post-window.sh OUT
    python3 scripts/dart_compare.py ../2026-10-01-t6021-macos-vs-linux-mmio/dart-tunables.tsv LOG...

The probe that ran is commit `2f943fe` (the source on main). `scripts/lib.sh`
holds the shared steps: the probe load under `/var/tmp/ane-run.lock` (it
refuses while an ane-run runs), the encoder run with its golden check, and
the stop checks.

`apply=2 orig_brd=… orig_bwr=…` writes the given values back with the same
RMW. This run never used it.

## Limits

- One M2. E1 and E2 ran on one boot, and the default arm ran on the next.
- The PERF counters gave no data. The TLB size comes from field names that
  m1n1 marks as guesses.
- The fault log is rate-limited to 10 lines, so only the first failed process
  has addresses.
- E2 applied the words live, without a flush. It does not test the macOS
  order (write at DART init).
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
| `scripts/` | `lib.sh`, `e1-window.sh`, `e2-window.sh`, `post-window.sh`, `ab-turn.sh` (the AfBridgeRun harness), `dart_compare.py` |
| `SHA256SUMS` | sha256 of every file here |
