# omarchy-ane v0.4.1 candidate: T6021 (M2 Max) hardware gate

Date: 2026-10-02, 20:59-21:47Z. Device: M2 Max lab laptop (T6021, j414c), stock
linux-asahi `7.1.13-3-1-ARCH`, stock GRUB default, lab m1n1 boot.bin `62ba3010`
(never written). Verdict: **PASS** on every gate below.

## What was tested

- Candidate: `main` at **53d162c**. Driver, library, smoke tool and fixture bytes
  are equal to 6608721 and bbc41a6 (`git diff bbc41a6..53d162c -- ane libane
  packaging/omarchy-ane-smoke fixtures` is empty; 6608721..53d162c touches only
  `.github/workflows/promotion.yml`, `tools/promote_from_verdict.py` and
  `tools/test_promotion_check.py`).
- Modules built out of tree with the `dkms.conf` make lines
  (`ANE_VERSION=0.4.1-main-53d162c CONFIG_DEBUG_INFO_BTF_MODULES=`) in an
  Arch Linux ARM chroot against the `7.1.13-3-1-ARCH` headers:

  | module | sha256 | srcversion | used |
  |---|---|---|---|
  | `ane_t6021.ko` | `c1b3db66c2f02835013dd5aba4768026fee2385a651e60b4b6c86387e86c9bd3` | `C6BEAF73AB255965AC9AA18` | installed, booted twice |
  | `ane.ko` | `0065abee6e4d19acb616a446c75d97bb38bd0fc05a68eae936da790157728b71` | `389B601E4710B9AFCFAE58C` | release record only, not installed |

  vermagic `7.1.13-3-1-ARCH SMP preempt mod_unload aarch64` for both; both carry
  the `stats` parameter. A release commit that only changes `ANE_VERSION` changes
  the version string, not srcversion (srcversion hashes the sources).
- Runner: `make -C tools ane-run` on the device from the same archive:
  `644a2dd9f62cbae37016c2de98985736fdbd59fad0456945348afd38d1b11580`.
  `packaging/omarchy-ane-smoke` and `packaging/omarchy-ane-check` ran from that
  tree with `packaging/omarchy-ane-run -> ../tools/ane-run` (the packaged layout).

Boot 1 ran the module with its defaults (`stats=1`). Boot 2 ran the same file
with `options ane_t6021 stats=0` in `/etc/modprobe.d/`. The module was never
unloaded or reloaded; every switch was a reboot. Every ANE call ran under
`flock /var/tmp/ane-run.lock timeout 120`.

## Results

| gate | result | evidence |
|---|---|---|
| G1 boot + bind (both boots) | PASS | own-memory replay line, fwalias 448 pages roundtrip OK, READY, DONE, `run returned 0 (booted=1 fw_alive=1)`, `chman: table VALIDATED (mismatch mask 0x0)`, `CONFIG_GET ... result=0`, `loaded ane_t6021 0.4.1-main-53d162c`; 0 `refusing`, 0 error lines, bound, `/dev/accel/accel0`; `omarchy-ane-check` ready |
| G2 H14 smoke | PASS | `omarchy-ane-smoke`: 20/20 bit-exact, every hash `94041b7cc10a66dfd76ecfd469728ad9d66f68b508f4a9744ce41208d6a4de0b`, errors 0 (boot 1: min 1.399 / median 1.464 ms; boot 2: 1.384 / 1.470 ms) |
| G2 gate ops | PASS | `ane/t6021/gate/gate.sh` add, mul, relu, matvec 2048x5120: GATE PASS on both boots |
| G2 whole Parakeet encoder | PASS | 3 processes x 20 calls, golden max_abs 0, fp16 sha256 `fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063` each; median 254.463 / 254.431 / 254.419 ms |
| stats item 1: file and format | PASS | see below |
| stats item 2: coreglass busy rows | PASS | idle `ane_busy` 0.0 (67 ticks), encoder `ane_busy` 0.994 (506 ticks), 3.893 jobs/s |
| stats item 3: consistency | PASS | 200 calls: jobs +200 exact, busy 50,887.9 ms <= process wall 51,566.4 ms (0.987), timeline 200 lines in the ticket range |
| stats item 4: stats=0 vs stats=1 | PASS | min-of-min 254.254 vs 254.295 ms (-0.016 %), median-of-medians 254.4435 vs 254.4375 ms (+0.002 %) |
| G4 restore | PASS | see below |

### Item 1: file and format

Boot 1, uptime 24.7 s, before any ANE client:

```
-r--r--r-- 1 root root /sys/class/accel/accel0/device/ane_stats
$ sudo -u nobody cat /sys/class/accel/accel0/device/ane_stats
busy_ns 0
jobs 0
# /sys/kernel/debug/ane_t6021/ane_timeline (0444), header only:
# ane_timeline: seq submit_ns start_ns end_ns tasks rc tmst (tmst raw tick on ane.ko, 0 = unavailable on ane_t6021)
```

The firmware boot ran its control-plane exchanges (LOAD_PROGRAM, CREATE_PROCESS,
CONFIG_GET) and `jobs` stayed 0: only PROCEDURE_CALL counts
(`ane/t6021/ane_t6021_rtclient_main.c`, the `stats_call` condition). The 20 smoke
calls then read `jobs 20`, `busy_ns 29144338` (1.457 ms per call).

Boot 2 (`stats=0`): the parameter reads `N`; `ane_stats` and the whole
`/sys/kernel/debug/ane_t6021/` directory are absent; the device binds and the
smoke and gates pass as on boot 1.

### Item 3: 200 PROCEDURE_CALLs

One `tools/qwen_prog_run.py --repeat 200` process is one `ane-run` with 200
`ane_exec()` calls, one `DRM_IOCTL_ANE_EXEC` each (`tools/ane-run.c`,
`libane/ane_m2.c`):

```
jobs 428 -> 628 delta 200; busy_ns delta 50887898560 (50887.9 ms, 254.439 ms/job)
process wall 51566.4 ms; busy<=wall True; busy/process_wall 0.9868; busy/(200 x median 254.439) 1.0000
timeline: 256 data lines (ring full); lines with ticket in (428,628] = 200; rc!=0 among them 0
```

Output golden-exact, fp16 sha256 `fca96f13...063`. The 20x16 timing arm on
boot 1 moved `jobs` by exactly 320, and the coreglass step by exactly 200.

### Item 2: coreglass

`coreglass hosts` reported the device ready (load1 0.34, `ane_stats`), then
`coreglass run <m2> --step 'encoder=<one qwen_prog_run --repeat 200>'` (an
ANE-only step, so no GPU lock), 718 samples over 71.9 s:

```
phase       n    sec   ane_busy  ane_jobs_s
idle       67    7.8      0.0       0.0
encoder   506   51.6      0.994     3.893
```

### Item 4: stats cost

Whole encoder, 20 blocks x 16 calls per arm, each block its own process,
golden-checked, gated on load1 < 0.5 and cpu PSI some avg10 = 0.00, from
uptime 360 s on. Both boots ran the same steps in the same order.

| arm | boot | min-of-min ms | median-of-medians ms | blocks bit-exact |
|---|---|---|---|---|
| stats=1 | 1 | 254.295 | 254.4375 | 20/20 |
| stats=0 | 2 | 254.254 | 254.4435 | 20/20 |

The difference (-0.016 % on the minimum, +0.002 % on the median) is inside the
0.3 % band and below the cross-boot spread seen earlier on one module binary
(+0.12 %). Boot order was fixed (stats=1 first), so a boot-to-boot drift and the
stats cost cannot be separated below that spread.

### G4: restore

The lab's own module (sha256 `af2cee6c3962e642d3de485131a118b71852c9d215503757efa626795a9b3b7a`,
srcversion `6783D1DE1FAE8D064F84FD0`) went back into `updates/`, the options file
was removed, and the device rebooted into the stock default. After that boot:
the module file hash and srcversion match, the module is bound with the same
firmware boot lines, boot.bin is still `62ba3010`, no `ane.ko` exists under the
kernel's module tree, `/etc/modprobe.d/` holds only its two lab files, no unit
failed, and a 4-call whole-encoder block is bit-exact (`fca96f13...063`, median
254.430 ms) (`results/restore-console.log`). A lab-state snapshot taken before
the window and after the restore differs in 14 lines: boot id, uptime, load, two
mtimes, the free-space line, and the restore check's own GPU-lock ticket. Every
hash in it (modules, `modules.dep`, boot files, firmware, tools) is unchanged.

## Release commit

The 0.4.1 release landed after this gate (main `9fa12a3`, release merge
`fd3b00c`). `git diff 53d162c..9fa12a3 -- ane libane fixtures
packaging/omarchy-ane-smoke packaging/omarchy-ane-check tools/ane-run.c
tools/qwen_prog_run.py tools/Makefile dkms.conf` changes only a comment in
`dkms.conf` (its `MAKE[0]` lines are unchanged), so the released driver, library,
runner and smoke bytes are the ones tested here.

## Files

`results/`: per-block timing tables and summaries for both arms, the three
encoder runs, the item-3 summary, both smoke JSON files, the gate lines of both
boots (including the first matvec call, which my script invoked with a wrong
argument order: it exited before any ANE call and passed when rerun), the
coreglass phase table, and the boot and restore consoles (lab user name
replaced with `<lab-user>`).

## Not verified here

- `ane.ko` on this device: built for the record, never installed (T6021 uses `ane_t6021`).
- The DKMS/package install path: modules were built and installed by hand, as the
  lab has no DKMS.
- `tmst` stays 0 on T6021 by design (no host time-stamp source).
