# 2026-11-18: ane_h18 fixediova rebuild and add reproduction on the M6 (T8152)

Lane w7Y. Machine: the M6 Mac mini (T8152, SoC `t8152`), kernel
`7.1.12-aurora-m6usb-ARCH` (boot `ec43c8e5`, 2026-11-18). Build host: the
x86 build container. Every hash below is a full `sha256sum` prefix, first 16
hex chars, then the last 4.

## What was rebuilt, from where

The source in `ane/h18/` on this branch is the fixed-IOVA module source from
the 2026-11-03 work tree, copied verbatim except for one documented addition
(`prog_out_dump_bytes`, below). The 2026-11-03 tree itself is preserved
elsewhere; nothing on this branch rewrites its behavior.

Built with the repo's documented kernel-build flow on the x86 build container
against the `m6-r19-match-20261031` kernel tree, `ARCH=arm64 LLVM=1`:

```
make -C <kernel-tree> O=<out-dir> M=ane/h18 ARCH=arm64 LLVM=1 modules
```

- module `ane_h18.ko` sha `f2fe5d2f1967ef0a...` (vermagic
  `7.1.12-aurora-m6usb-ARCH SMP preempt mod_unload aarch64`)
- program pack `ane_h18_prog-add2.bin` sha `2b587bda770ac841...` (84 336 B,
  6 sections, 3 buffers, inputs embedded), packed on the build host by
  `tools/ane-prog-pack` (sha `e8b7918f`, built with the upstream stub-`drm.h`
  recipe from `tools/test_ane_prog_pack.sh`) from `fixtures/h14-anec/add/program-0.anec`
  with `--in 5=a.bin --in 6=b.bin` (a, b: 32 768-byte fp16 patterns,
  deterministic, documented in the pack log)

## One source addition (documented, measurement only)

`ane_h18_main.c`: new module param `prog_out_dump_bytes` (uint, 0444, default
0). When nonzero, after the call the module hexdumps the first N bytes of
every output buffer through the existing `ane_h18_prog_dump` (16 bytes per
line). Nothing else changed. This is what produced the `out buffer` lines in
the run log below.

## The run (2026-11-18 17:57 UTC, boot `ec43c8e5`)

Per-boot sequence, all writes through the module's own debugfs controls:

1. `insmod ane_h18.ko optin=t8152 stage=boot confirm_power=1 confirm_boot=1
   map_fw=1 owned_fw=1 fw_path=h18_ane0_fw_kirkland_j8xx.payload.bin
   handshake=1 hs_cfg_size=0x380000 hs_wait_ms=2000 hs_chan=1 hs_cmds=1
   hs_start=1 hs_prog=1 prog_fw=ane_h18_prog-add2.bin
   prog_iova_bufs=0x30040000 prog_iova_text=0x3000c000 read_scratch=1
   read_dart=1 dart_inst=15 alias_mbox=1 rtkit_hello=1 rtkit_start=1
   rtkit_stop=1 prog_out_dump_bytes=4096`
2. `echo apply > /sys/kernel/debug/ane_h18_dt` — applies the boot overlay
   (`ane-t8152-boot` opt-in); the ANE0 platform device appears; the driver
   probes and reports `RESULT stage=boot verdict=READY
   reason=bound-awaiting-start-stop`.
3. `echo start-stop > /sys/kernel/debug/ane_h18_boot` — runs the whole
   ladder.

### Result: the same behavior the 2026-11-03 receipt records

| Step | 2026-11-03 receipt | This run |
|---|---|---|
| firmware | RTKit 3514.0.15 (Aug 8 2026 build), owned copy staged, two segments mapped in the default DMA domain | same (`owned firmware staged bytes=0x370000`, `iommu_map` of the read and read+write segments) |
| handshake | SCRATCH7 READY, wake answered, table address returned | `handshake t+2ms SCRATCH0=0xffbb0000` (new table address), table `TERMINAL` magic, all 8 channels parsed |
| memory requests | SHAREDMALLOC answered on the IO channel | FWMALLOC surfaces granted (ordinals 1 to 4+ logged) |
| commands | PRINT_ENABLE, CONFIG_GET (tick `0x16e3600`), START processed, RESOURCE_INFO_GET | identical (`CONFIG_GET reply u32 at +8 = 0x16e3600`; firmware log shows `EVT#3: START PROCESSED`) |
| LOAD_PROGRAM | answered, acknowledgement 0 | `LOAD_PROGRAM program id 0x0` |
| CREATE_PROCESS | answered, acknowledgement 0 | `CREATE_PROCESS process id 0x0` |
| PROCEDURE_CALL | acknowledgement 0, one IO_T2H event state 0, no finish event | `PROCEDURE_CALL gave no finish event after 2000 ms, 1 IO_T2H events` |
| program outcome | sequence ends, no output produced | `sequence result -110` (ETIMEDOUT) |
| teardown | core stopped, lifecycle retained until reboot | `final cpu_status=0xe stopped=1`; verdict `FAIL reason=handshake-channel-step-failed`; lifecycle retained |
| DART | read-only reads before LOAD_PROGRAM and after the call | same (`dart_inst=15`, four instances read both times) |

New evidence this run adds:

- input buffers verified after the call: `buffer 5 (input, 0x8000 bytes):
  8192 nonzero words`, first words `40003c00 44004200 46004500 48004700`
  (the packed a-pattern); `buffer 6 (input, 0x8000 bytes): 8192 nonzero
  words`, first `38000000 3e003c00 41004000 43004200` (the packed b-pattern).
  The inputs reached the ANE buffers intact.
- output buffer measured, not assumed: `buffer 4 (output, 0x8000 bytes):
  0 nonzero words` plus the 4096-byte `out buffer` hexdump — all zeros. The
  procedure never finished, so nothing was written. The zero output is the
  expected consequence of the missing finish event, now shown byte-exact.
- firmware-side log captured for the first time (56 records): RTKit
  3514.0.15, `VM mode: dynamic`, `CANEController() done`, heap sizes, and the
  `START PROCESSED` event.

### Fixed-IOVA layout note (deviation from the param doc, recorded)

`prog_iova_bufs=0x30000000` (the value in the param description) collides
with `prog_iova_text=0x3000c000` for this pack: each add-pack buffer is
0x8000 bytes, so buffer id 6 lands at 0x30008000 and its second 16 KiB page
is 0x3000c000 — the __TEXT alias the module has already mapped. The apply
refuses with `fixed IOVA 0x3000c000 for PROG buf 6 is already mapped`
(-EBUSY). This run uses base `0x30040000` instead; the __TEXT address is the
only compile-time address the HWX carries, buffers are plain descriptor
addresses, and the aliases are host-chosen. Recorded here and in the param
comment so the next run does not rediscover it.

### Files

- `post-insmod.dmesg`, `post-run.dmesg` — full kernel log, before the apply
  and after the whole ladder
- `ane_h18_prog-add2.bin` — the exact pack the run loaded (sha above)
