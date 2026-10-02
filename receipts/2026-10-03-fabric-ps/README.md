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
