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
- Rounding for mul and the scalar ops, and the matvec accumulation order, are
  checked against references (see Stage 1-4) but not derived from Apple
  documentation.
- Autoload was tested on three boots. A hang at autoload would loop on
  every boot; the recovery is the kernel argument `module_blacklist=ane_t6021`.
- The module cannot be unloaded after the firmware starts. Reboot only.

## Stage 1-4 ops through the installed path (boot e77abe94)

`gate.sh OUTDIR OP` (4 seeded trials, then a second process with
`--repeat 3`), module `ff85a989...` autoloaded, libane at commit 742a728.
The C section builder is byte-identical to the Python builder on all nine
fixture ANECs (`make check`, host-only). On the device, first run of each
program:

| op | shape | result |
|---|---|---|
| add | [1,512,1,1] | 512/512 lanes exact, padding zero: PASS |
| mul | [1,512,1,1] | 512/512 exact: PASS |
| relu | [1,512,1,1] | 512/512 exact: PASS |
| add-scalar (0.5) | [1,512,1,1] | 512/512 exact: PASS |
| mul-scalar (0.5) | [1,512,1,1] | 512/512 exact: PASS |
| real-div-scalar (0.5) | [1,512,1,1], 2 tasks | 512/512 exact: PASS |
| clip-low (maximum 0.5) | [1,512,1,1] | 512/512 exact: PASS |
| clip-high (minimum 0.5) | [1,512,1,1] | 512/512 exact: PASS |
| matvec | x [1,256] x W [256,256], 2 tasks, 128 KiB weights | 256/256 within 2 ulp, max 1.000 ulp (224 to 256 exact per trial): PASS |

Artifacts: notebook `Main/installed-ops-e77abe94/` with SHA256SUMS.
The inferred kernel-base refs {1,2} (real-div, matvec, clip) worked on the
first device run. The matvec reference accumulates in fp32 and rounds once;
the device differs by at most 1 ulp on some lanes, so accumulation order
or width on the device is not the reference's.

## Qwen-size matvec (boot 7f0e8312, module `91f16eed...`)

Driver BO cap raised (1 GiB per BO, 2 GiB counted total). Dense random
inputs (M rows of K halves), weights fp16 [N,K], 4 trials + reopen/repeat
per case, all PASS:

| case | tasks | weights | device error vs fp64-exact |
|---|---|---|---|
| M=1 K=1536 N=1536 | 2 | 4.5 MiB | max 0.217 cond-units |
| M=8 K=2048 N=2048 | 2 | 8 MiB | max 0.218 cond-units |
| M=1 K=2048 N=5120 | 2 | 20 MiB | max 0.187 cond-units |

Cond-unit = |device - exact| / (2^-11 * sum |a_k w_k|). The ulp of the
result is the wrong scale under cancellation: the same runs show up to
23 ulp on lanes whose result is near zero. Bit-exact lanes: 50 to 100 %
per trial. The pass threshold (4 cond-units) is an assumption chosen
above the measured maximum, not derived. The first matvec runs with the
sparse 256-value input (K=1536 with 256 nonzero values) were a weak test
and are superseded by these. The directory `gate-7f0e8312-matvec` in the
artifact holds a run without `--weights` (expected FAIL).

## BO lifetime (boot a32ca689, module `d7417708...`)

Before: after the firmware ran, the driver never freed a BO and kept its
bytes counted. One session of 20 MiB matvec runs and island runs reached
the 2 GiB cap (`BO_INIT failed for 2310144 bytes`). Now: BOs are
reference-counted (handle + each user mapping); a BO whose IOVA the
firmware never saw is freed at the last put; a section BO of a LOAD that
was sent to the firmware, and an io BO used in a CALL, stay held (the
firmware never sees a freed IOVA); PROG_LOAD of identical sections reuses
the cached program, so the new section BOs are freed. Test: 150 runs of
the 2048x5120 matvec (20 MiB weights, about 3 GiB of BO traffic), all
PASS, no BO_INIT failure, no IOMMU fault in dmesg, memory used 4.9 GB and
stable. Not tested: `free_io_bos=1` (off by default), free while mapped.
Open ceiling: io BOs of every process stay held until reboot.
