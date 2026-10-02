# T6021 gap window result (2026-10-02): the ANE computes 3.7x slower under Linux

The compute-bound probe P6' runs in 1.180 ms under macOS 27.0 (26A428) and in 4.415 ms under Linux
(5.465 ms raw), on the same M2 Max and the same day. Both systems produce bitwise-identical output for
it. By the rule registered before the window (ratio >= 2.0), this supports H1: under Linux the ANE runs
at a low compute operating point. The activation-stream probe P7 (2.49x) and the whole Parakeet encoder
(2.84x) are slower too, but less than the compute probe. Under macOS the fabric perf-state word reads
one step higher (0x666 against 0x555 under Linux), and the ANE SLC data-set id is 14 (Linux 0).

## Timings

Linux "corr" subtracts the 1.05 ms per-call settle from the minimum (GapRank's r_p). macOS times are
min-of-min / median-of-medians over 20 blocks x 20 calls, each block one in-process compile
(`_ANEInMemoryModel`, PERFSTATS=1). The macOS load average was 18.9-27.4 throughout (post-boot
indexing; the 10-minute load gate expired). Load can only lengthen a macOS time, so every ratio here
is a lower bound.

| probe | Linux min raw / corr (ms) | macOS min / med (ms) | ratio corr | ratio raw | output |
|---|---|---|---|---|---|
| P6' (64x conv1x1 512ch, 8.59e9 MAC, input x0.8) | 5.465 / 4.415 | 1.180 / 1.307 | **3.741** | 4.631 | identical on both OSes; not the CPU golden |
| P6 (same program, original input) | 5.468 / 4.418 | 1.178 / 1.306 | 3.750 | 4.642 | identical inf signature on both OSes |
| P7 (fp16 add, 2 x 32 MiB in, 32 MiB out) | 4.120 / 3.070 | 1.233 / 1.278 | 2.490 | 3.341 | valid on both (rel L2 2.7e-4) |
| Parakeet whole encoder | 254.260 / 253.210 | 89.138 / 89.232 | 2.841 | 2.852 | bit-exact on both (fca96f13) |

Rates: P6' is 7.28 TMAC/s under macOS against 1.95 (corr) / 1.57 (raw) under Linux. P7 moves 96 MiB
at 81.6 GB/s under macOS against 32.8 / 24.4 GB/s under Linux.

Linux sources: P6' was measured in this window (stock lab boot, module af2cee6c, 20 x 16 calls, each
block idle-gated at load1 < 0.5 and PSI 0.00). P6, P7 and the encoder are the same-day GapWinA blocks
under the same boot conditions ([linux-e1.json](linux-e1.json)). P6' and P6 agree within 0.05% on
Linux, so the Linux timing does not depend on the input values.

## Decision (rules registered before the window)

- r(P6') = 3.74 >= 2.0: **H1 supported**, which is the "ANE compute clock / operating point is low
  under Linux" branch. The result holds with the raw minimum too (4.63). The data-path branch, which
  needs r(P6') <= 1.15, is excluded.
- The P7-vs-P6' split (threshold 1.5x) is borderline. The corrected ratios give 3.741 / 2.490 = 1.502,
  which points at compute; the raw ratios give 1.386, which reads as uniform. This split decides
  nothing.

Output validity: the P6/P6' MIL embeds its weight through a BLOBFILE that both compilers read as the
wrong constant, so neither output matches the CPU fp16 reference (P6Fix diagnosis). The macOS P6'
output is bitwise identical to the Linux P6' output: all 262,144 fp16 lanes, with 257,646 inf lanes on
each. Both systems compute the same function on the same data, so P6' is a like-for-like timing probe.
Identical output does not prove an identical task schedule, though. The Linux program is the Studio
h14 cross-compile; the macOS program is macOS 27's in-process compile.

## Register words (ANERegDump, approved frozen kext 932d3b9b, already loaded; no approval needed)

| word | macOS idle | macOS mid-loop (2 of 2 attempts) | Linux (GapWinA/B) |
|---|---|---|---|
| fabric-ps 0x28e20c000 | 0x00000666 | 0x00000666 | 0x00000555 |
| dcs-ps 0x28e20c400 | 0x00000999 | 0x00000999 | 0x00000999 |
| DSID 0x285c2046c (field bits[17:10]) | gated (islands down) | 0x00003880 (field 14) | 0x00000080 (field 0) |

The mid-loop attempts both passed the gate on the first poll: ane_sys/ane_cpu read 0x1f0003ff and the
six compute islands read 0x3ff. Every range read `ok`. The gate is the set this M2 passed on
2026-09-25; ane_sys_mpm 0x4000 stays off under macOS. Raw values: [words.json](words.json).

## ioreg (iBoot ADT, live), [ioreg-nodes.txt](ioreg-nodes.txt)

- `ane0`: `function-mcc_dataset` = mcc phandle 0xa3 + 'M$DS', `clock-ids` 0x13e-0x141, `pre-loaded` 1,
  `segment-ranges` as in the findings doc section 3.
- `mcc`: `dsid_mgmt` 1, `mcache-pmp` 1, `dcs-enable-thermal-loop` 1, `ptd-ranges` <0xe, 0x12>.
- `iop-pmp-nub` (firmware t6020pmp): ANE bandwidth DVFS inputs `ane0-{fast,slow}-{,af-,afr-}bw-threshold`
  and their `-dvfs-filter` values (fast 0xe0; slow 0x1f, slow-af 0), `mcache-stream`, and the `dvfs-domain` list
  (DCS, FAB, AFR, SOC0, ...).
- `pmgr`: `perf-domains` names an ANE domain with id 8, and `voltage-states8` holds 7 pairs:
  600, 852, 1104, 1356, 1596, 1848 and 2100 (x1e6) at 593-906 mV. INFERENCE: this is the ANE DVFS table.
  Its top-to-bottom ratio is 3.5, close to r(P6') = 3.74. `ane-dpe` 1, `soc-dpe` 1.
- `dart-ane0`: dart,t8110, `sid` <0, 0xf>, `dapf-instance-0`; its first window is the pmgr PS block
  0x28e084000-0x28e084033.

## perfStats and powermetrics

- perfStats: `_ANERequest` accepted the mutable dict on all 80 blocks, but every block returned it
  empty (`{}`). Through this path, macOS 27.0 exposes no NE compute, nominal or throttle counters.
- `powermetrics --samplers cpu_power` under the P6/P7 loop showed ANE Power at 9,046-9,148 mW in 7
  samples (the P6 phases) and 1,668-2,687 mW in 3. The `ane_power` sampler printed only a header.
  See [powermetrics-cpu_power.txt](powermetrics-cpu_power.txt).

## How it ran

1. Linux pre-flight (read-only lab-state snapshot), then P6' 20 x 16 on Linux in one gpu-turn ticket.
2. BootNext macOS (`asahi-bless -n --set-boot-macos -y`, default unchanged) and reboot. macOS answered
   in 50 s with the expected build 26A428.
3. Bundle at this branch's a956454: the timings and powermetrics ran first, and their artifacts were
   fetched with hashes before the register phases. The final fetch verified 379/379 files, and the
   remote scratch was removed.
4. `sudo reboot` from macOS. Linux came back in 127 s. Afterwards the lab state was identical to the
   pre-flight snapshot apart from the boot identity: ESP boot.bin 62ba3010, module af2cee6c bound,
   BootNext consumed, dmesg clean. One encoder block ran bit-exact (fca96f13).

Bundle fixes made before the boot (commit a956454): the block counter never matched the compact JSON
`"rc":0`, so every block overwrote block-01; the P6' arm was staged but never run; regdump would have
built an unapproved kext; the gate could never pass on macOS; and rc_all always ended 0. One fix came
after the window: `stamp` parsed `usec` as the boot time, so the `uptime_s` values in this capture are
invalid and have been removed from [macos-blocks/](macos-blocks/). The `ts` and `load` values are
correct.

## Not verified

- The live ANE perf state on either OS, and what `voltage-states8` means (inference above).
- Whether the cross-compiled and the native P6 programs run the same task schedule. The cheapest
  discriminator: run the macOS-native P6 HWX under Linux, the NativeVsCross method.
- The P6/P6' output against the CPU golden; that needs the BLOBFILE const fixed in the probe MIL.
- macOS timings on a quiet machine (load stayed 18.9-27.4).

## Files

`analysis-output.txt` (analyze_gap_window.py on the real capture), `linux-e1.json`,
`linux-p6prime-blocks.tsv`, `macos-blocks/{p6,p6prime,p7,pk}.tsv`, `words.json`, `ioreg-nodes.txt`,
`powermetrics-cpu_power.txt`. The raw capture (firmware and kernel-handoff dumps included) stays in
private evidence.
