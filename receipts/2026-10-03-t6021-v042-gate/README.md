# omarchy-ane v0.4.2 candidate on T6021 (M2 Max): release gate and the `dyn_pg` arms

Date: 2026-10-03, 04:50-07:04Z, plus an arm-0 session 08:30-09:39Z. Device: the M2 Max
lab laptop (T6021, j414c), stock linux-asahi `7.1.13-3-1-ARCH`, stock GRUB default, lab
m1n1 boot.bin `62ba3010` (never written). This is the first hardware run of the `dyn_pg`
protocol in
`receipts/2026-10-03-t6021-dynpg/README.md`.

Verdicts:

- **Gate A (default parameters, what ships): PASS.**
- **Arm B (`dyn_pg=1`): the firmware accepts the command but does not gate.** Every
  correctness and timing check passes, and the seven ANE power-state words read ACTUAL
  0xf before and after jobs, so there is no idle-power or latency effect to measure.
- **Arm C (`dyn_pg=1 boot_prevent_nap=0`): REFUSED at probe.** The firmware boots, then
  does not answer CONFIG_GET; the driver refuses the device. Not retried.
- **Arm 0 (driver never loaded): 251 mW below the held driver** at idle (pooled; 227 mW
  against same-session held blocks, 275 mW against the gate run's), far above the boot-to-boot
  noise seen for either arm (28 and 48 mW). This is the ceiling for any ANE power-down work.

## Module under test

`main` at **5a457a3**, built out of tree with the `dkms.conf` make lines
(`ANE_VERSION=0.4.2-main-5a457a3 CONFIG_DEBUG_INFO_BTF_MODULES=`) in an Arch Linux ARM
chroot against the `7.1.13-3-1-ARCH` headers:

| module | sha256 | srcversion | used |
|---|---|---|---|
| `ane_t6021.ko` | `4f6c939b4a8ea6780a0edb565e9e0542806596a90580174d55d97ad662c08cb6` | `59494CBC56F28ED8D1122C6` | installed in `updates/`, booted 6 times |
| `ane.ko` | `5d6bfa2454c372dba337c42c382d6414eb6f159ae2f455c98faeb83ccbd759b5` | `B94A022ECB6F0FF27D17BB8` | built for the record, not installed |

vermagic `7.1.13-3-1-ARCH SMP preempt mod_unload aarch64`. The `ane_t6021` srcversion
equals the 9aa98f5 host build in the dyn_pg receipt (same driver sources). Runner:
`tools/ane-run` built on the device from the same archive,
`73c1efad348c9fac67b27afcd81de150a3accefe422b59b4d30b16fcef489833`.

Every arm was its own boot of this one file. Test options used a one-shot modprobe.d
`install` line that deletes itself and syncs before the load, so a reset falls back to
the defaults. The module was never unloaded or reloaded. Every ANE call ran under
`flock /var/tmp/ane-run.lock timeout 120`. Timed and power steps ran at uptime >= 360 s,
gated on load1 < 0.5 and cpu PSI some avg10 = 0.00, recorded per block.

| boot | options | state |
|---|---|---|
| A | none | bound |
| S0 | `stats=0` (ane_stats item 4) | bound |
| B | `dyn_pg=1` | bound |
| C | `dyn_pg=1 boot_prevent_nap=0` | probe failed -110, unbound |
| A-end | none | bound (end state) |

## Gate A (default parameters)

| check | result | evidence |
|---|---|---|
| boot, bind | PASS | `run returned 0 (booted=1 fw_alive=1)`, `chman: table VALIDATED (mismatch mask 0x0)`, `loaded ane_t6021 0.4.2-main-5a457a3`; bound, `/dev/accel/accel0`; `omarchy-ane-check` ready |
| log level | PASS | see below |
| modinfo | PASS | `license` 1 line (`Dual MIT/GPL`), `description` 1 line (`Apple Neural Engine (T6021/M2) installed module`) |
| H14 smoke | PASS | `omarchy-ane-smoke` 20/20 bit-exact, every hash `94041b7c...`, errors 0, on boots A, S0, B and A-end |
| gate ops | PASS | `gate.sh` add, mul, relu, add-scalar, mul-scalar, real-div-scalar, clip-low, clip-high, matvec 2048x5120: GATE PASS on boots A, S0 and B |
| whole encoder | PASS | 20 processes x 16 calls: min-of-min **254.306 ms**, median-of-medians **254.515 ms**, 20/20 golden-exact, fp16 sha256 `fca96f1355485ec3...`; v0.4.1 gate: 254.295 / 254.4375 (+0.004 % / +0.030 %) |
| ane_stats item 1 | PASS | `ane_stats` 0444, `sudo -u nobody cat` gives `busy_ns 0` / `jobs 0` before any client; debugfs `ane_t6021/ane_timeline` present; with `stats=0` neither file nor the debugfs directory exists |
| ane_stats item 2 | PASS | coreglass: idle `ane_busy` 0.0 (68 ticks), encoder 0.993 (506 ticks), 3.893 jobs/s |
| ane_stats item 3 | PASS | 200 PROCEDURE_CALLs: jobs +200 exact; busy 50,902.3 ms <= process wall 51,536.7 ms (0.988); 200 timeline lines in the ticket range |
| ane_stats item 4 | PASS | stats=0 vs stats=1 min-of-min 254.349 vs 254.306 ms (+0.017 %), median-of-medians 254.509 vs 254.515 (-0.002 %) |

Every ane line in `dmesg -x` at the default level (`loglevel=3` on the command line) on boot A:

```
info  platform 284000000.ane: Adding to iommu group 0
warn  ane_t6021: loading out-of-tree module taints kernel.
info  LAB init resource[0x84] bit0=1: prevent nap
warn  boot: module PINNED until reboot (started CPU; wedged-pin)
info  boot: run returned 0 (booted=1 fw_alive=1) — DONE observed — handshake complete; scratch_result=...
info  BOOT-PHASE sequence returned 0000000000000000 (cpu_started=1 fw_alive=1 booted=1) CPU_STATUS=0x2c
info  chman: table VALIDATED (mismatch mask 0x0)
info  loaded ane_t6021 0.4.2-main-5a457a3 (DRM major 2 minor 0; ABI 2; legacy_only=1 chman_ok=1 booted=1; state HELD; BO cap 12288 MiB)
```

No `ANERD`, `ANEWR` or `ps probe` line, no emerg/alert/crit/err line. The two warnings are
the kernel's taint notice and `ane_t6021_boot.c`'s `dev_warn` for the started CPU. One
`BOOT-PHASE` line stays at info (`ane_t6021_rtclient_main.c`, the sequence summary); the
own-memory, fwalias and CONFIG_GET lines are `dev_dbg` now and absent, as the source says.

## Arm B: `dyn_pg=1`

The command went through: `dyn_pg: firmware dynamic power gating on
(SET_DYNAMIC_POWERGATE = 1)` at 4.44 s, after the ChMan line and before the device
registered. The legacy exchange returns 0 only when the firmware sets the done bit,
echoes opcode 0x2d and length 0x0c, and writes status 0, so the firmware returned status
0. The driver reads no other reply word, and the default legacy mode has no firmware
console reader.

| check | arm B | arm A |
|---|---|---|
| `ane_pg_state` before any job | all seven words `0x000003ff` | same |
| `ane_pg_state` 30 s after 83 calls, and every 10 s through 3 idle blocks | all `0x000003ff` | same |
| smoke, 9 gates | PASS | PASS |
| encoder min-of-min / median-of-medians | 254.318 / 254.5085 ms (+0.005 % vs A) | 254.306 / 254.515 |
| first call after 5 s idle, median (n=20) | 1.525 ms | 1.549 ms |
| back-to-back calls, median | 1.512 ms | 1.513 ms |
| jobs vs calls | exact (+83, +320, +40) | exact |

ACTUAL stayed 0xf in `ane_td`, `ane_base` and `ane_set1`-`4`: the firmware did not gate
its compute islands. This is the dyn_pg receipt's "firmware did not switch" branch. Its
decode says `setDynamicPowerGate(1)` skips `SwitchDynamicPowerGate` when the firmware's
+0x1a2 flag is set; Linux cannot see that flag. Other causes the data does not exclude:
a missing prerequisite command, or a gating condition this workload never meets. No other
command was sent to tell these apart. With no gating, arm B equals arm A for idle power,
and the power table below shows no arm effect larger than the session drift.

## Arm C: `dyn_pg=1 boot_prevent_nap=0`

```
info  boot: run returned 0 (booted=1 fw_alive=1) — DONE observed — handshake complete; ...
info  BOOT-PHASE sequence returned 0000000000000000 (cpu_started=1 fw_alive=1 booted=1) CPU_STATUS=0x2c
info  chman: table VALIDATED (mismatch mask 0x0)
info  LEGACY timeout ch=1 io=00000000fbffc000
err   install: CONFIG_GET failed (-110) — refusing to register DRM device (ioctls would run on an unproven ring)
err   probe with driver ane_t6021 failed with error -110
```

Without the prevent-nap bit (the `LAB init resource[0x84]` line is absent, as expected),
the firmware finishes its boot handshake but does not answer the first legacy exchange
within 3000 ms. The 0x2d command never ran, because CONFIG_GET comes first. No
`/dev/accel`, no `ane_stats`, no `ane_pg_state`. This fits the dyn_pg receipt's open risk
for arm C: the ASC CPU naps and the host doorbell does not wake it (inference; the
`ane_cpu` power word is not readable here). Arm C cannot run on this driver until the
wake path for a napping ASC exists. The refused state was left alone and the box
rebooted to the defaults.

## Idle power

Each block: 300 s at 1 Hz with the module loaded and held, no ANE job, AC online, battery
Full at 100 %, backlight unchanged (400), load1 <= 0.29 and cpu PSI 0.00 at start and end.
Rails: the `macsmc_hwmon` power inputs, read by name as coreglass's `sampler.hwmon()`
reads them. The statistic is the block median (the signal is spiky; single samples reach
25 W).

`pm_genpd_summary` showed `ane_sys`, `ane_cpu`, `ane_sys_mpm`, `ane_td`, `ane_base` and
`ane_set1`-`4` `on` on every boot (A, S0, B, refused C, A-end), with runtime status
`active`: the driver holds its runtime-PM reference, and genpd cannot see firmware
gating, which is why `ane_pg_state` exists.

| block | boot | fans | Total System Power median (mW) |
|---|---|---|---|
| A1 | A | on all block (2317 rpm) | 14998.4 |
| A2 | A | on 59 % | 14949.6 |
| A3 | A | off | 14766.0 |
| B1 | B | on 59 % | 14886.3 |
| B2 | B | off | 14722.1 |
| B3 | B | off | 14717.1 |
| Cr1 | C (refused) | off | 14669.8 |
| Cr2 | C (refused) | off | 14663.4 |
| Cr3 | C (refused) | off | 14659.9 |
| Aend1 | A-end | off | 14750.5 |
| Aend2 | A-end | off | 14757.2 |

Fan state moves the total by about 200 mW (A2: 14978.5 with the fans on, 14754.3 off), so
the comparison uses only blocks in which the fans never ran. Means of block medians, with
a 95 % bootstrap interval over blocks:

| difference | Total System Power | blocks |
|---|---|---|
| B − A (A and A-end pooled) | −38.3 mW [−46.4, −30.6] | 2 vs 3 |
| refused C − A | −93.5 mW [−102.0, −85.1] | 3 vs 3 |
| refused C − B | −55.2 mW [−61.0, −49.4] | 3 vs 2 |

These intervals cover block-to-block noise inside one boot, not boot-to-boot variation,
and each arm had one boot. The only same-arm pair of boots (A at 05:18Z, A-end at 06:53Z)
agrees within 16 mW (14766.0 vs 14750.5 / 14757.2). Readings:

- B is 38 mW below A although its compute islands never gated (ACTUAL 0xf throughout). The
  data does not say where those 38 mW come from: one boot cannot separate a small effect
  of the 0x2d command from boot-to-boot variation larger than the A pair showed.
- The refused C state (firmware booted without the prevent-nap bit, no host traffic) is
  94 mW below A. That is the size of what a napping ASC could save (inference; the state
  is not a working driver, and `ane_cpu`'s power word was not read).
- The Heatpipe rail moves the other way between some of these states; its meaning on this
  SMC is not verified, so it is reported in the artifacts but not used.

## Arm 0: driver not loaded (08:30-09:39Z)

The idle-power ceiling for any power-down design: the same module file installed, but
`ane_t6021` kept from loading for one boot by the same self-deleting one-shot file,
`install ane_t6021 /usr/bin/rm -f <file> && /usr/bin/sync && /usr/bin/true`. The
initramfs carries no ane module, so udev's autoload on the root file system reads that
line; nothing loads and the device is never probed. No GRUB or command-line change was
needed. Two such boots (Z1, Z2), then the default boot (held, "As" blocks, same session).

Each arm-0 boot was checked at boot and at the start and end of every block: no
`ane_t6021` in `lsmod`, no `/sys/module/ane_t6021`, no `/dev/accel`, no driver link on
`284000000.ane`, the one-shot file gone, runtime status `unsupported`. The only ane line
in dmesg is `platform 284000000.ane: Adding to iommu group 0`.

Genpd with the driver never loaded (both Z boots): `ane_set1`-`4`, `ane_base`, `ane_td` and
`ane_sys_mpm` `off-0`; `ane_cpu` and `ane_sys` `on`. The kernel logs `PM: genpd: Disabling
unused power domains` at 0.64 s, and later the eight pmgr power controllers report
`sync_state() pending due to 284000000.ane`. With the driver loaded all nine are `on`.

Same sampler, statistic and quiet gates as above; every block below had the fans off for
all 300 samples.

| block | boot | Total System Power median (mW) |
|---|---|---|
| Z1a | Z1 (arm 0) | 14502.6 |
| Z1b | Z1 (arm 0) | 14494.8 |
| Z1c | Z1 (arm 0) | 14486.5 |
| Z2a | Z2 (arm 0) | 14471.2 |
| Z2b | Z2 (arm 0) | 14461.2 |
| As1 | held, default | 14692.0 |
| As2 | held, default | 14715.9 |
| As3 | held, default | 14721.9 |

| difference (means of block medians, 95 % bootstrap over blocks) | Total System Power |
|---|---|
| held − arm 0, same session (As vs Z) | **+226.7 mW** [+206.8, +245.6] |
| held − arm 0, gate-run held blocks (A3, Aend1, Aend2 vs Z) | +274.6 mW [+260.0, +290.1] |
| held − arm 0, all six held blocks | +250.7 mW [+225.4, +274.8] |
| arm 0 boot to boot (Z1 − Z2) | +28.4 mW |
| held, session to session (As − gate-run held) | −48.0 mW |
| refused C − arm 0 | +181.1 mW |

The block intervals do not cover boot-to-boot noise. The measured boot and session
spreads (28 and 48 mW) are each under a quarter of the smallest held − arm-0 estimate.
The sessions drifted downward over time (Z1 > Z2, As < gate-run held), and the held
blocks came last, so drift works against the difference rather than creating it.

Reading: holding the driver costs about 0.23-0.27 W at idle on this machine. The refused
C state (firmware booted, all nine domains on per genpd, no prevent-nap) sits 181 mW above
arm 0. INFERENCE: most of the gap comes with the powered compute islands and the running
firmware, and only part of it (the 94 mW refused-C step) is reachable by letting the ASC nap.

The arm-0 session ended on the default boot 3d3b1e2e with the same checks as the end
state below: candidate `4f6c939b...` bound with default parameters, smoke 20/20, a 4-call
encoder block bit-exact (median 254.394 ms), jobs +24 exact, no error line, and a lab-state
diff against 08:30Z of identity, load, the `accel0` mtime and the check's own ticket only.

## End state

The gate session left the device on boot A-end, and the arm-0 session left it on boot
3d3b1e2e in the same state: the candidate as the lab module with default
parameters: `updates/ane_t6021.ko` `4f6c939b...`, version `0.4.2-main-5a457a3`, `dyn_pg`
N, `boot_prevent_nap` Y, `stats` Y, bound, `/dev/accel/accel0`, no test file in
`/etc/modprobe.d/`, boot.bin `62ba3010`. After 360 s of uptime: smoke 20/20 bit-exact, a
4-call whole-encoder block bit-exact (median 254.490 ms), jobs +24 exact, no error line.
A lab-state snapshot from before the window differs only in the module file, its
version string, the boot identity and load line, the `accel0` mtime, and the GPU-lock
ticket of the check itself. The previous lab module (`af2cee6c...`) is kept as a backup.

## Files

`results/`: the boot records of all five boots (state, `dmesg -x` ane lines, check), the
arm B `ane_pg_state` read, per-block encoder tables and summaries for A, S0 and B, the
three-process encoder runs and the item-3 summary, both latency runs, every power block
summary, and the end-state console with its lab-state diff. `results/arm0/`: the three
arm-0-session boot records, every arm-0 and same-session held power block summary with
its genpd snapshot, and that session's end-state console and lab-state diff.

## Incidents

- 05:28-05:39Z: the control host lost ssh to the device (`connection unexpectedly
  closed`, then connect timeouts). The device kept running on the same boot. The S0
  timing run was attached to that ssh session; it stopped after 18 of 20 blocks when the
  session died, and was rerun in full. Every later step ran detached with a log file.
- Three later "no answer" alarms (05:42-05:47Z, 05:51-05:57Z, 06:09-06:14Z) were a bug
  in my poller: it counted grep's "no match yet" exit status as an ssh failure. Fixed;
  the device answered throughout.

## Not covered

- The six island gates (they need the `/var/tmp/inst` layout); 9 of the dyn_pg receipt's
  16 gates ran.
- Arm C's power, latency and encoder rows (no device).
- The receipt's bootstrap CI rule for B−A and C−B as a pass test: with no gating in B and
  no device in C there is no arm effect to test; the intervals above are reported as
  measured, not as a verdict.
- `ane.ko` on any device (record build only); the DKMS install path.
