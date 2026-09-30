# T6021 (M2 Max) installed path: add through the autoloaded module and libane

Date: 2026-09-29. Host: the M2 Max laptop, Linux 7.1.13-ARCH-polltx.
Branch `agent/m2-installed-path`. Firmware `t602x_ane0_fw_selene_rc4x.macho`,
sha256 `a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`.

## What ran

- Module `ane_t6021` (`ane/t6021/`), installed with `modules_install` and
  `depmod` (`updates/ane_t6021.ko`), loaded by udev through the
  `of:*apple,t6021-ane` alias about 9 s after boot. No module parameters,
  no debugfs, no staged sequence files. DRM accel node `/dev/accel/accel0`,
  driver `ane` 2.0.0 (ABI 2, `ane/src/uapi/drm/ane_accel.h`).
- libane ABI-2 backend (`libane/ane_m2.c`) and `tools/ane-run`: parse the
  ANEC, build the six firmware sections, PROG_LOAD, PROC_CREATE, EXEC.
- Gate: `ane/t6021/gate/gate.sh` (12 seeded trials, then a second process
  with `--repeat 3`), boot `efb71f00`: GATE PASS. Four parallel `ane-run`
  workers, 40 runs each with `--repeat 3`: 160 of 160 pass.
- Evidence: notebook artifacts `Main/installed-path-efb71f00/` (gate log,
  outputs, dmesg, module sha `ff85a989...`), notebook entry
  `20260929T224500Z-...-installed-driver-first-load.md`.

## What the pass means

The fixture add program is nchw [1,512,1,1] with a 64-byte plane stride.
It has 512 valid fp16 lanes (index % 32 == 0). The gate feeds sparse
inputs and checks all 16384 output halfwords: 512 valid lanes exact under
fp16 half-away-from-zero rounding, and the padding zero.

CORRECTION to the lab receipt `first-inference.md` (lab repo): its
"16384/16384 bit-exact" counted 15872 padding zeros in `a`, `b` and `y`.
The proven result is 512 valid lanes, not 16384 independent elements.
Dense random inputs are not a valid test: the engine ignores the padding
lanes and writes zero there.

## Defects found on hardware and fixed

1. mmap of a sub-page BO failed (size compared against the unrounded BO).
2. Firmware program table: 256 entries, never freed. The 257th load
   answered with a protocol error. The driver now shares one program and
   one process between loads with identical sections (SHA-256 key) and
   returns ENOSPC before the 251st distinct program.
3. Firmware malloc table: static 128 entries ran out after ~7 loads.
   Dynamic table, 512 MiB cap.
4. libane returned success after a failed ioctl and crashed on NULL.
5. Completion. The firmware ack, the last-committed-TD word and the TQ
   status words report done about 0.13 ms before the output reaches DRAM.
   Completion is now: TD counter moved and all TQ idle, then a 1 ms settle
   (`call_settle_us`, default 1000).

## Not proven

- On boots 6a42598f, 977903e1 and 55c6230b, about 1 call in 5 returned
  success with an all-zero output. The cause of the boot dependence is not
  known. No failure in 900+ runs on six other boots, also not with a
  powersave governor or a memory-bandwidth load. The 1 ms settle is
  therefore unverified against a failing boot.
- Only the add program has run. Other ops need the generalized C builder.
- Autoload was tested on three boots. A hang at autoload would loop on
  every boot; the recovery is the kernel argument `module_blacklist=ane_t6021`.
- The module cannot be unloaded after the firmware starts. Reboot only.
