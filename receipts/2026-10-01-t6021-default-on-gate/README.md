# T6021 default-on gate: reserved mode, own memory and the packaged boot chain on the M2 Max (2026-10-01)

Status: passed. The own-memory default merged as PR #49 (`b6ef8f1`). The test fix that #47 needed merged as PR #51
(`69796ae`).

## Result

Four boots of one M2 Max (T6021, j414c, linux-asahi `7.1.13-3-1-ARCH`) ran the omarchy-ane package as a packaged
install puts it on disk. Every boot started the ANE firmware, passed all 16 gates and gave the bit-exact Parakeet
encoder. The own-memory mode gives byte-identical outputs to the reserved mode, at the same speed, and it runs with
the stock m1n1 1.6.1 that has no ANE reservation.

| Boot | Module | m1n1 stage 2 | Firmware copy | Encoder median, 20 calls | Gates | Mailbox IRQs |
| --- | --- | --- | --- | --- | --- | --- |
| A | main `73da8f8` (`fw_alias_reserved=1`, old default) | lab `v1.6.1-pdtrace` | iBoot preload, reserved windows | 254.457 ms | 16/16 PASS | 0/0 |
| B | same, one-shot `fw_alias_reserved=0` | lab | own memory | 254.497 ms | 16/16 PASS | 0/0 |
| P | main `b6ef8f1` (own memory is the default) | stock `v1.6.1` (package `m1n1 1.6.1-1`) | own memory | 254.318 ms | 16/16 PASS | 0/0 |
| final | main `b6ef8f1` | lab | own memory | 254.365 ms | 16/16 PASS | 0/0 |

The release module 0.4.0 gave 254.457 ms with the same script on the boot before A. In every boot the encoder output
had golden max_abs 0 and fp16 sha256 `fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063`. The
63 gate output files, the Qwen prog_020 output and the encoder output are byte-identical between A, B and P.

## What ran

- Module builds on the M2 (native gcc, nice 19): main `73da8f8` `e77c3c75…` (= the #47 driver; README-only commits
  after it), main `b6ef8f1` `af2cee6c…` (0.4.0-main-b6ef8f1, the final module). `ane-run` built from both trees is
  `f687a7ab…`, the 0.4.0 release binary.
- Package install as the omarchy-pkgs change list describes it (`scripts/install-pkg.sh`): the module,
  `omarchy-ane-dt`, `omarchy-ane-check`, `omarchy-ane-firmware-fetch`, `update-m1n1-dtbs`, the hooks
  `90-omarchy-ane-dt.hook`, `90-omarchy-ane-dt-remove.hook` and `90-omarchy-ane-firmware.hook`, and
  `packaging/build-dtbo /` (overlays in `/usr/share/omarchy-platform/dtb-overlays`). The hand-installed overlays in
  the old directory were removed, and `ane-t6021` was removed from the opt-in file. `uboot-serial-stdin-t6021` stays:
  this laptop needs it for U-Boot. Then the commands that the hooks run: `omarchy-ane-firmware-fetch --hook` ("already
  installed and matches the pin") and `omarchy-ane-dt apply` ("copy is current").
- The device tree copy did not change (`c31a54c3…`, scratch-root prediction and real apply): the T6021 overlay without
  the opt-in key gives the same tree, because fdtoverlay does not copy the overlay root key into the board tree.
- Boot B used a one-shot modprobe line that deletes its own file before the load, so a reset falls back to the
  default (`scripts/oneshot-b.sh`).
- Boot P used the boot.bin of the packaged flow (`scripts/stage-p.sh`): the packaged `update-m1n1` with only the line
  that `omarchy-ane-dt apply` adds to `/etc/default/update-m1n1`, so m1n1 and U-Boot are the package files
  (`pacman -Qkk` 0 altered; both equal the cached package members). Compared with the lab boot.bin `62ba3010…`
  (`logs/esp/`): the m1n1 part differs (stock 1,114,112 B `9ad08653…` against lab 1,179,648 B `6fdf8bba…`), all 110
  device trees are identical (j414c `c31a54c3…`), U-Boot `b8d52d16…` is identical, and the config tail is empty.
  After P the lab boot.bin went back on the ESP, because the lab needs its 60 s proxy window for recovery.

## Kernel lines

Boot A (reserved mode) has `fwalias: reserved SEG0/SEGi at entry 0x10000000000 (10000000000+c4000
100000c4000+438000, preloaded placement)` and no `refusing` line, so the new reservation guard passes with the lab
m1n1. Its ane_t6021 lines equal those of the 0.4.0 release boot except two text-only lines (the #47 fwload message and
the version string; `logs/A/trace-vs-release-0.4.0.txt`).

Boots B, P and final have these lines, and they are the expected lines for the release gate (default own memory):

```
fwload: apple/ane/t602x_ane0_fw_selene_rc4x.macho PRELOAD validated + DART-mapped: entry 0x0, iova 0x00000000ff800000 size 0x700000
fwload: own memory: iBoot patches replayed (soc 0x6021 rev 0x11 DATA 0x100000c4000 cpu 0x285000000 wrapper 0x285400000)
fwalias: entry 0x10000000000 <- 448 dart pages aliased from fw 0x00000000ff800000 (first <PA, changes per boot>, roundtrip OK)
pmu: DART map 0x28e084000 (IOVA == PA, 0x4000 bytes): 0
BOOT-PHASE P4 pollA READY observed
BOOT-PHASE P7 pollB DONE observed
boot: run returned 0 (booted=1 fw_alive=1) ... scratch_result=00000000ff7a0000 ...
chman: table VALIDATED (mismatch mask 0x0)
LEGACY CONFIG_GET words 00000003 016e3600 result=0 (DMA remains held)
```

No `fwalias: reserved` line, no `owned heap` line and no `refusing` line; `/sys/module/ane_t6021/parameters/fw_alias_reserved`
reads `N`. With timestamps and numbers masked, the ane_t6021 lines of B differ from A only in these two fwalias
lines, and P and final equal B (`logs/*/trace-*.txt`). Firmware READY comes at 4.2-4.3 s and DONE at 4.4 s in all four
boots. Boot P has no `ane-firmware@` node under `/reserved-memory` and reports m1n1 stage 2 `v1.6.1`
(`logs/P/chosen.txt`).

## Pass criteria

Written in the private notebook before each boot. Each boot: one boot with no reset, 0 failed units;
`omarchy-ane-check` ready with no `UNTESTED` line; `omarchy-ane-dt status` `source=overlay`; gates add, mul, relu,
add-scalar, mul-scalar, real-div-scalar, clip-low, clip-high, matvec 2048x5120 and the islands island-c-pv,
island-a-kt, island-a-attn-p1, island-b-select-runtime, island-b-select-constfill, rms-c2048-gamma (3 seeds each)
PASS; encoder bit-exact with a median of 20 calls within 254.5 ms ± 0.5 % (B and P also within 0.5 % of A); prog_020
t15 rel L2 0.00117 ± 1e-5 (all boots 0.001174227); 60 s four-worker burst with 0 failures (11,318 to 13,028 runs);
no new bad kernel line; mailbox IRQ 53/54 counts 0 and 0. All met.

## Host checks

- The negative case of the reservation guard (reserved mode without covering no-map nodes refuses) was not run on
  the M2. `tools/test_t6021_alias_rollback.py` compiles the real guard against fake device trees. After #47 it did not
  compile; with the fix in #51 it passes on main, and a mutant with one page of slack at the window end fails it
  (`logs/host/`).
- On the flip commit `b619b4a`: `test_ane_m2`, `test_ane_dt`, `test_ane_firmware_fetch` ok, `h14_boot_regression`
  127/127; a native W=1 build has the same six warnings as main.

## Limits

- One M2 Max, one boot per configuration. The encoder medians differ by less than 0.1 %, inside the boot-to-boot
  spread of earlier runs.
- Boot P is the packaged boot chain, but the root file system is the lab system (the same kernel and packages, plus
  lab files that the ANE path does not read).
- The release module is built by DKMS from the release tag; the module here was built from main with the kernel
  headers. The release gate on the release commit is a separate run.

## Files

- `logs/A`, `logs/B`, `logs/P`, `logs/final`: ane_t6021 kernel lines, window console, `omarchy-ane-check` output,
  encoder and prog_020 logs, trace comparisons. `logs/P` also has the boot.bin staging, install and restore logs.
- `logs/esp`: parse and part-by-part comparison of the two boot.bin files.
- `logs/prep`: package install, device tree prediction, the release-module script check. `logs/host`: host tests.
- `scripts`: the device scripts as run; in the copies here only the usage comments name no host.
- `SHA256SUMS`.
