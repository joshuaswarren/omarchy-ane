# 2026-10-02 — GapWinB: DSID arms A/B/B2 on the T6021 M2 (fw_dsid_set A/B + TM readback)

Result of the pre-registered A/B in `receipts/2026-10-02-ane-dsid/README.md` ("Pre-registered
A/B protocol"), executed by worker GapWinB (notebook entry
`entries/GapWinB/20261002T155006Z-jw14m2-linux-dsid-arms.md`, never committed). Same module
binary for all arms (sha256 584e7ac2b06069b5c4aeda3994dfc1b8b703ed182479fb7556e734b77373551c,
branch `agent/ane-dsid-probe` @ 90b800c/b435a9d), parameter set per arm by
`/etc/modprobe.d/ane-dsid-test.conf` (removed at restore). One boot per arm; whole encoder
20 blocks x 16 calls per arm (separate `qwen_prog_run` process per block; every block gated
load1 < 0.5 AND PSI cpu some avg10 = 0.00 AND settled boot >= 25 min; every block golden-checked
fp16 sha256 fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063, bit-exact);
ane-run gates add/mul/relu/matvec-2048x5120 per arm; `ane_dsid_tm_probe.ko` (24bbaadb...)
readback of TM word 0x285c2046c after timing, under its PS guard.

## Decision (pre-registered rules, applied to measured data)

**DSID tagging alone has no effect (MCC side not programmed).**

- Arm A (defaults off): min-of-min 254.115 ms, median-of-medians 254.277 ms raw
  (settle-corrected 253.065 / 253.227 ms at 1.05 ms). Same-day anchor vs the pre-window
  baseline (254.26 / 254.4115): drift -0.057% / -0.053% — inside the <= 1% validity band;
  arm A is a valid baseline.
- Arm B (`fw_dsid_set=9`): the firmware sequence COMPLETED — dmesg: `fw MCACHE_SIZE_GET reply
  0x300000 result=0`, `fw DSID_SET dsid=9 result=0` (both result=0, fired at boot during
  driver probe, before any program load). TM word 0x285c2046c read **0x00002480**
  (= baseline 0x80 | 9<<10; dsid bits [17:10] = 9, no other bits moved) at idle, during an
  encoder block, and after. Encoder min-of-min 254.421 / median 254.5655 ms raw
  (corrected 253.371 / 253.5155): **+0.120% / +0.113% vs arm A — unchanged within the
  pre-registered +-1% band** (3% effect line would need 261.7 ms min; even the 1% "changed"
  line is 256.7 ms). 20/20 blocks bit-exact, all gates GATE PASS.
- Arm B2 (`fw_dsid_set=9 fw_dsid_defaults=1`): **INVALID for timing per the pre-registered
  abort branch.** The sequence aborted at the second exchange: `fw MCACHE_SIZE_GET reply
  0x300000 result=0` (4.47 s after boot), then `LEGACY timeout ch=1` and
  `fw ANE_DEFAULT_SETTING_SET result=-110` (7.47 s = the 3000 ms exchange timeout), then
  `install: boot command failed (-110) — refusing to register DRM device` and
  `probe with driver ane_t6021 failed with error -110`. No /dev/accel node existed; DSID_SET
  was never sent; timing/gates/probe skipped (a run against a nonexistent device node is not
  evidence). No wedge, no fault lines, clean reboot available.

So: the command **lands** (word reads 9, exactly 0x80 | 0x2400) and **changes nothing**
(+-0.12%). Under the pre-registered decision table that is the "word reads 9, time unchanged"
branch: DSID tagging of the TM word alone has no measurable whole-encoder effect on Linux —
consistent with the decode-thread caveat that the M$DS/PTD side (the consumer that would act
on data-stream ids) is never programmed on Linux.

## Bonus finding for the decode thread

The 0x2e ANE_DEFAULT_SETTING_SET replication (bytes identical to what macOS 13.5 sends this
same firmware: {regId=4,0x33},{regId=3,0xe}) **does not complete under Linux** — no reply
within the 3 s exchange timeout — while the 0x26 immediately before it succeeds. This closes
AneDsidBuild's "Not verified" item live: the arm-B2 boot is the test. Whether the firmware
ASSERT-stops replying or silently ignores the command is not distinguishable from this log
alone. The 0x25 DSID_SET path itself is proven working (arm B).

## Run record

- armA-20261002T162816Z: PASS (20/20 bit-exact, gates 4/4, word 0x80 field 0, sequence lines 0)
- armB-20261002T170344Z: PASS (20/20 bit-exact, gates 4/4, word 0x2480 field 9, sequence
  result=0 x2)
- armB2-20261002T175000Z: INVALID (sequence aborted at 0x2e, device refused, timing skipped)
- Runner: `tools/ane-run` 140,376 B built on the M2 from a hash-verified git archive of
  origin/main daa7447 (archive sha256 feae35255a6bee980f592a7eae1dbd6d1508f12e10c780edd78d9cf9676f2e6f,
  byte-identical to the archive the pre-window baseline used).
- Restore: hand module af2cee6c reinstalled + options file removed 17:24:04Z (sha + srcversion
  6783D1DE1FAE8D064F84FD0 verified). The restore REBOOT was delayed ~90 min by the
  Release0715 lane's KokoroDefault daemon holding the M2 GPU lock out-of-band (the queued
  reboot ticket correctly never fired under it and was cancelled); it landed at 18:52:35Z
  (95 s). Post-restore lab-state verification PASS: stock kernel 7.1.13-3-1-ARCH, hand module
  af2cee6c bound (srcversion 6783D1DE1FAE8D064F84FD0), ESP boot.bin 62ba3010, zero probe
  modules, no dsid options file, dmesg BAD count 0, 0 failed units, cpufreq originals intact,
  labstate-pre/post diff = boot id + uptime + mtimes only, and one 4-call whole-encoder
  smoke block on the restored module bit-exact (golden max_abs=0 relL2=0 exact=1, rc 0).

## Artifacts

Notebook artifacts: `~/.local/share/apple-silicon-lab/artifacts/GapWinB/` (runs/, SHA256SUMS).
This receipt: `receipts/2026-10-02-ane-dsid/results/` on branch `agent/ane-dsid-arms`.
