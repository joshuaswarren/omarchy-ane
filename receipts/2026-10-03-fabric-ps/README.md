# T6021 fabric-ps 5->6 (ABA, one boot) and DSID=14 (second boot) — protocol

Pre-registered 2026-10-02T20:57Z, before any change on the M2 Max (T6021, j414c, linux-asahi 7.1.13-3-1-ARCH).
Results are appended below after the run.

## Why

The same M2 runs the compute-bound probe P6' at 1.180 ms under macOS 27 and 5.465 ms under Linux (ratio 3.74),
P7 (2x32 MiB fp16 add) 2.49x, the Parakeet encoder 2.84x. Two register differences were captured: fabric-ps
0x28e20c000 reads 0x666 on macOS (idle and under ANE load) and 0x555 on Linux; the ANE TM DSID word 0x285c2046c
carries dsid 14 on macOS (0x3880) and 0 on Linux (0x80). A first DSID test used the 13.5-derived value 9 and moved
nothing (+0.12%).

## Register layout (source)

- fabric-ps is the `apple,t6020-pmgr-misc` reg "fabric-ps" (arch/arm64/boot/dts/apple/t602x-die0.dtsi).
  drivers/soc/apple/apple-pmgr-misc.c defines only `APPLE_CLKGEN_PSTATE_DESIRED GENMASK(3, 0)` at offset 0 and
  writes it as read-modify-write on suspend/resume.
- Bits [11:4] have no source for a clkgen word (it is not in the pmgr ps-regs list, so the device PS layout with
  ACTUAL [7:4] does not carry over). Both observed values repeat DESIRED in all three nibbles.
- So the lab module writes DESIRED [3:0] only (the kernel's own RMW) and reads the whole word back:
  success = 0x666 within 1 s; anything else is an abort (revert, reboot).

## Lab module ane/t6021/probes/ane_fabric_ps_set.c

j414c/t6021 only; one `ioremap_np` of the 4-byte word, nothing else mapped; init reads and refuses unless the word
is exactly 0x555; `go` param accepts 6 (only from 0x555) or 5 (only after this module raised it); every write is
logged before it happens, then the word is polled every 10 ms for 1 s. No module_exit; a reboot is the only undo.
Build (ALARM chroot, linux-asahi 7.1.13-3-1-ARCH headers, gcc 16.1.1): ane_fabric_ps_set.ko sha256
bc42e7b3fb38c79d20b4dbed5b471077d525becf8050070f2c92eb2fe1ce1411; the same build reproduces ane_dcs_ps_probe.ko
73f36b1c and ane_dsid_tm_probe.ko 24bbaadb byte-identical.

## Experiment 1 (fabric-ps), one boot, no ANE module change

Arms A1 -> B -> A2. Each arm: encoder 20 blocks x 16 calls (every block bit-exact, fp16 sha fca96f13), P7 20x16
(rel L2 <= 1e-3), P6' 20x16 (output bitwise equal to the earlier Linux P6' output), matvec 2048x5120 gate + 20x16.
Between A1 and B: `go=6`; between B and A2: `go=5`. fabric-ps sampled with the read-only probe after each write
and during an extra encoder block in B.

Decision (min-of-min per program): EFFECT if |B/A1 - 1| >= 3% on any program; NO EFFECT if all < 1%; 1-3%
unresolved. A2 within 1% of A1 = reversible; A1/A2 drift > 1% flags that program.

## Experiment 2 (DSID=14), second boot

Module 584e7ac2 (agent/ane-dsid-probe, fw_dsid_set param) with `options ane_t6021 fw_dsid_set=14`. Expect dmesg
`fw DSID_SET dsid=14 result=0` and the TM word 0x00003880 (bits [17:10] = 14). Same four programs; reference =
mean of Experiment 1 A1/A2. Word 14 with |C/A - 1| < 1% everywhere => "DSID tagging alone has no effect (MCC side
missing)"; >= 3% on any program => EFFECT. Confounder: different module binary and boot than Experiment 1.

## Restore

Start module back, options file removed, reboot to the stock default; verify module hash/srcversion, bound, ESP
boot.bin unchanged, no lab modules, dmesg clean, one bit-exact encoder block.

## Results (2026-10-02 21:47-22:43Z)

Start state: linux-asahi 7.1.13-3-1-ARCH, hand module ane_t6021.ko af2cee6c (0.4.0-main-b6ef8f1). Every arm:
20 blocks x 16 calls per program, each block idle-gated; every block of every arm produced the same outputs
(encoder fp16 fca96f13 bit-exact, P7 d95bd62b, P6' 103f96b2 = the earlier Linux P6' output, matvec gate PASS +
ddc66dc8); dmesg clean; no fault. Per-block data: `results/arm-*.tsv`; ratios: `results/analysis-output.txt`.

### Experiment 1: fabric-ps 5 -> 6 -> 5 (one boot)

The write (`results/exp1-fabric-ps-dmesg.txt`): `0x00000555 -> 0x00000556`, the word read `0x80000666` after
50 ms and `0x00000666` after 61 ms; it read 0x666 in all 70 samples while raised (~4.7 min, including during an
encoder block); the revert returned `0x00000555` the same way.

| program (ms, min-of-min) | A1 (5) | B (6) | A2 (5) | B/A1 |
|---|---|---|---|---|
| Parakeet encoder | 254.201 | 254.772 | 254.222 | +0.22% |
| P7 2x32 MiB add | 4.124 | 4.104 | 4.111 | -0.49% |
| P6' 64x conv1x1 | 5.415 | 5.446 | 5.474 | +0.57% |
| matvec 2048x5120 | 2.240 | 2.186 | 2.275 | -2.41% |

No program moved by 3% or more. Matvec sits in the 1-3% band, but its A1/A2 differ by 1.6% and the DSID boot
(fabric back at 5) reads 2.2% faster too, so that deviation is not tied to the fabric state. By the
pre-registered rule the result is "small/unresolved" (matvec only). The macOS/Linux ratios on this chip are 2.84
(encoder), 2.49 (P7) and 3.74 (P6'), so the fabric pstate is not a material part of that gap.

### Experiment 2: fw_dsid_set=14 (second boot, module 584e7ac2)

dmesg: `fw MCACHE_SIZE_GET reply 0x300000 result=0`, `fw DSID_SET dsid=14 result=0`. The TM word 0x285c2046c
read `0x00003880` (dsid bits [17:10] = 14, other bits 0x80 unchanged) idle, during an encoder block and after.
Against the mean of A1/A2: encoder +0.07%, P7 -0.11%, P6' +0.23%, matvec -2.24%. The value macOS uses lands and
changes nothing beyond the matvec band, as dsid 9 did before: DSID tagging alone has no effect on Linux.

### Restore

Hand module af2cee6c back, options file removed, reboot to the stock default: module hash/srcversion verified and
bound, ESP boot.bin unchanged, no lab modules, dmesg clean, 0 failed units, encoder smoke bit-exact (min 254.315 ms).
