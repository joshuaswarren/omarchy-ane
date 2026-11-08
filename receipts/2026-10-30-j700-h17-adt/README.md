# J700 H17 stage 0: live boot ADT

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`.
Repository baseline `063ae8e`; aurora device-tree source
`68df87370abac4e227936552dd9be2252972e551`. The running tree has
`/soc/ane@400000000` and three `iommu@4018*` nodes, all `disabled`;
`omarchy-ane-probe` reported `reachability.verdict=not-exposed` and
`driver_loaded=false`. No ANE MMIO, power-domain request or task submission
occurred in this receipt.

`make -C ane/h17` against the running kernel's headers exited 0. GCC 16
reported warnings in kernel headers because that kernel was built with
Clang 18; there were no source warnings from `ane_h17_adt_probe.c` in the
final build. `make -C ane/h15 check` exited 0 (`test: ane_h15_adt — ok`).

The first stage 0 attempt found a 64-byte `segment-ranges` property but
refused it because the H15 helper preserves the flags in the upper 32
bits of the TEXT size word. The corrected T8140 probe splits the flag
and byte count. Its built module SHA-256 was
`3f76396d38a3c03f2ae6f73a727474908040b960825ed692c7e5cbc354738f08`
for the loaded build. The next `insmod ... optin=1`
exited 0 and printed:

```text
ane_h17_adt_probe segment-word[0]=0x10000774000
ane_h17_adt_probe segment-word[1]=0x0
ane_h17_adt_probe segment-word[2]=0x10000000000
ane_h17_adt_probe segment-word[3]=0x1000c0000
ane_h17_adt_probe segment-word[4]=0x100019ac000
ane_h17_adt_probe segment-word[5]=0xc0000
ane_h17_adt_probe segment-word[6]=0x100000c0000
ane_h17_adt_probe segment-word[7]=0x2ac000
ane_h17_adt_probe TEXT flags=0x1 DATA flags=0x0
ane_h17_adt_probe TEXT phys=0x10000774000 iova=0x10000000000 size=0xc0000
ane_h17_adt_probe DATA phys=0x100019ac000 iova=0x100000c0000 size=0x2ac000
ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=PASS reason=segments
```

`sudo rmmod ane_h17_adt_probe` exited 0, and `lsmod` showed no probe
module afterward. This establishes that iBoot left ANE segment metadata
in the live ADT. It does not establish that the preload bytes are valid,
that the ANE can power up, or that inference runs.

## Stage 1: reserved-RAM preload read

The first `inspect_preload=1` attempt refused `reason=outside-dram` before
mapping a segment. Linux's `memory@10000000000/reg` begins at
`0x10003438000` on this boot, after the iBoot-reserved firmware area. The
probe was corrected to admit only ADT-named ranges in the first 64 MiB of
the J700 DRAM aperture that end before the first Linux RAM range. The
corrected build had SHA-256
`dc4f00b7a3235a906654182ada4ecaac6b463fe9fc65cebf98fbe3f33c1f6eae`
when loaded. `sudo insmod ... optin=1 inspect_preload=1` exited 0 and
printed:

```text
ane_h17_adt_probe preload segment=TEXT sha256=cda09ec1aee68b88b47edc0eebc810452532a855563fbd650b95e2846c345310 nonzero=1
ane_h17_adt_probe preload segment=DATA sha256=ceea79eadab680c3a849dc93442a8d5bec6d43b5e304fa88673e89969c721f5c nonzero=1
ane_h17_adt_probe RESULT stage=1 soc=t8140 verdict=PASS reason=preload-readable
```

`sudo rmmod ane_h17_adt_probe` exited 0. This proves the two named ranges
hold readable, nonzero bytes on this boot. It does not compare them with
the pinned firmware payload or prove that ANE hardware can run them.

After the empty-segment refusal was added, the final module built without
source warnings (SHA-256
`0cebd48a202ed52689bf29f570212568abfb746512e48447783b4d9c882bbaf0`).
The same stage 0 and stage 1 outputs passed again, with the same two
segment digests. `sudo rmmod` exited 0. An `insmod` without `optin=1`
then refused with `reason=board-or-optin`, and no module remained loaded.

## Stage 2: the host's 26.6.2 payload and preload diff

The host's `asahi,os-fw-version` is 26.6.2. The earlier T8140 pin in
`data/ane-soc/t8140.json` is for macOS 27.0 and must not be used on this
boot. A range request to Apple's restore image for this build (the
address `packaging/omarchy-ane-firmware-fetch` carries) extracted
`Firmware/ane/h17_ane_fw_theia_d9x.im4p`. The ZIP member's CRC
passed. The IM4P is 1,572,889 bytes, SHA-256
`1694f0dd3157dc887e58cdb09b01c874fcf2ec3b5c201f4a08d6564afb7f669c`;
its raw Mach-O payload is 1,572,864 bytes, SHA-256
`32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c`.
This extraction did not download or hash the entire IPSW archive.

The pinned Mach-O has TEXT vm `0+0xc0000`, file `0x4000+0xc0000`, DATA vm
`0xc0000+0x2ac000`, file `0xc4000+0xbc000`. Its `ce_env` section is at vm
`0xb8280+0x4000`; `_rtk_patchbay` at `0xcbdc8+0x261`; and
`_rtk_tunables` at `0xde030+0x6a0`. These are file-layout facts from the
26.6.2 payload, not observed register behaviour. The source records them
in `ane/h17/ane_h17_fw.h` and the fetcher pins this file only for a
26.6.2 stub.

`python3 tools/test_ane_firmware_fetch.py` passed. On this host,
`sudo python3 packaging/omarchy-ane-firmware-fetch` installed the pinned
payload under `/usr/lib/firmware/apple/ane/`, and `--check` passed.
The stage 2 module build was W=1-clean for its own source, SHA-256
`fcfe45553bf650b9070ddcc096c20ef491da56a74126bbe613b8fee620a761b9`.
The `compare_preload=1` run exited 0 and reported:

```text
ane_h17_adt_probe firmware name=apple/ane/h17_ane_fw_theia_d9x.macho bytes=1572864 sha256=32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c pin=PASS
ane_h17_adt_probe comparison segment=TEXT differing_bytes=1730 allowed=1730 forbidden=0 first=0xb8280 last=0xb8ff0 first_forbidden=0x0 file_bytes=0xc0000
ane_h17_adt_probe comparison segment=DATA differing_bytes=336 allowed=336 forbidden=0 first=0xbdf0 last=0x1e650 first_forbidden=0x0 file_bytes=0xbc000
ane_h17_adt_probe RESULT stage=2 soc=t8140 verdict=PASS reason=preload-whitelist
```

`sudo rmmod ane_h17_adt_probe` exited 0. The code and zero-fill matched
the 26.6.2 file outside the three named environment sections. This result
does not prove that the patched values are correct, that ANE power is
available, or that any program executed.

## Before stage 3: running device-tree gate

`sudo python3 ane/h17/preflight.py` exited 0 with `verdict=PASS` on all
11 checks. The script read `/proc/device-tree` and the genpd debugfs
summary only. It found J700; a disabled `apple,t8140-ane` at
`0x400000000+0x2000000` with IRQs 655 and 668; three disabled DARTs at
`0x401800000+0xc000`, `0x401820000+0x20000`, and
`0x401840000+0x20000`; and all four nodes linked to the same ANE_SYS
domain. The domain is enabled in the source DT at PMGR offset `0x290`,
but the live genpd state was `off-0`. The source is aurora-linux
`b6f070c911934c9c377c4be939b883bf32f37acf`, files
`t8140.dtsi` and `t8140-pmgr.dtsi`, hashes D4 and D5 in
`data/ane-soc/t8140.json`. No power request, ANE MMIO or DART access
occurred. This gate is a prerequisite to designing stage 3, not stage 3
itself.

## Stage 3 source and current-boot refusal

`ane/h17/t8140-ane-ps-probe.dts` creates one synthetic ANE_SYS consumer;
it does not change the disabled ANE or DART nodes. The new
`ane_h17_ps_probe.ko` calls Linux PM runtime on that consumer, reads only
the PMGR state word at offset `0x290`, checks ACTUAL bits 7:4 for `0xf`,
then releases the vote. The PMGR semantics and 100 microsecond poll
timeout come from aurora-linux
`b6f070c911934c9c377c4be939b883bf32f37acf`,
`drivers/pmdomain/apple/pmgr-pwrstate.c`. The module itself does not
map ANE or DART registers.

`make -C ane/h17` exited 0 against the running kernel headers; the
source had no W=1 warning (GCC emitted existing kernel-header attribute
warnings). The module SHA-256 is
`062131061c4a521bb06c1a145b0fcf72ee22fb3a4b0f56c0409e2c7eb16c9053`.
The kernel's `dtc -q -@` compiled the overlay (SHA-256
`e7de0de7ec934c44ec88756bc2d399bd83d3728ed4259b6dc7e143bedf7f2d67`),
and `fdtoverlay` merged it with `/sys/firmware/fdt` offline. In that
merged blob the consumer's `power-domains` is `<0xa0>`, the existing
ANE_SYS phandle; the ANE remains `disabled`.

On the unchanged live tree, `sudo insmod ane/h17/ane_h17_ps_probe.ko
optin=1` refused with `error=-22`, `reason=board-optin-or-dt`, because
the synthetic node is absent. The module was not loaded and the live
genpd summary still read `ane_sys off-0`. No power vote or ANE MMIO
occurred. This is a refusal-path result, **not a stage 3 power PASS**.

The host has `omarchy-mac-boot 20261024-1` and its
`/usr/lib/omarchy-mac/boot/dtb-overlays.sh` hook. `dtc 1.8.1` was
installed because the hook requires `dtc`, `fdtoverlay`, and `fdtget`
at version 1.7.1 or newer. Its own `dtb_overlays_build` merged the
opted-in overlay with the installed `t8140-j700.dtb`: the synthetic
consumer and ANE_SYS both held phandle `0xa0`, while the ANE remained
`disabled`. The overlay was installed at
`/usr/lib/omarchy-mac-boot/dtb-overlays/t8140/omarchy-ane-h17-ps-probe.dtbo`,
and the key `ane-h17-ps-probe` was written to
`/etc/omarchy-mac-boot/dtb-overlays.opt-in`.

`sudo update-m1n1` exited 0 and wrote `/boot/efi/m1n1/boot.bin` with
SHA-256 `5da129f0a4edde5c9bc30e0565c140bb167eb5ded739bab25d2fca785d839648`.
Its strings include `omarchy,t8140-ane-ps-probe`. The previous boot
image SHA-256 was
`f4ae3c12689f81cf136802c9ed70a8457f1593e68366cf58126458d5988c44c6`.
This is staged for the **next boot**; the current running tree still lacks
the probe node and ANE_SYS is still `off-0`. No reboot or live power
transition has occurred yet.

`omarchy-apple-silicon-boot-check --boot-chain` initially refused the
manually copied overlay because it was not owned by a package. A local
`omarchy-ane-h17-ps-probe-dt 0.1.0-1` package was built from
`packaging/arch/omarchy-ane-h17-ps-probe-dt/PKGBUILD` and installed;
`pacman -Qo` now names that package for the overlay, and its installed
SHA-256 remains `e7de0de7ec934c44ec88756bc2d399bd83d3728ed4259b6dc7e143bedf7f2d67`.
The package hook rebuilt m1n1. `sudo omarchy-apple-silicon-boot-check
--boot-chain` then exited 0: `running linux-aurora 7.2.11-6-1-ARCH;
installed boot files match`.

The one-time `ane/h17/run-stage3.sh` was exercised on the current boot.
It re-ran firmware `--check`, stages 0, 1, and 2, and the eleven-check
DT preflight successfully, then stopped because the synthetic node is
absent until reboot. It unloaded the ADT probe and made no power vote.

`omarchy-ane-h17-stage3.service` was installed under `/etc/systemd/system`
and enabled. Its one-time marker is
`/var/lib/omarchy-ane/h17-stage3.pending`. On the next boot it removes
that marker before the runner starts. The runner writes a log under
`/var/log/omarchy-ane/` keyed by boot ID, and stops at the first failed
rung. The service had not run at this point.

## Stage 3 live result after reboot

Boot ID `3322fb33-90fb-473e-8eff-5668ac9a9f4c`, still kernel
`7.2.11-6-1-ARCH`. The one-time service exited 0. Its complete log is
`/var/log/omarchy-ane/h17-stage3-3322fb33-90fb-473e-8eff-5668ac9a9f4c.log`,
SHA-256 `72b7af3dbc75f2e666b84f50d3bf30465d035fec4ae25314f9fc32eb2909d899`.
It re-ran the 26.6.2 firmware pin, stages 0, 1, and 2, and all eleven
tree checks before any power request. The boot ADT named the same TEXT
and DATA physical ranges; the TEXT digest remained
`cda09ec1aee68b88b47edc0eebc810452532a855563fbd650b95e2846c345310`.
The DATA digest was
`85c38477bb5921bbf1524ce3daa109ae658b80399f9ade0699dbadd4ac587e71`.
The preload comparison found 1,730 allowed TEXT differences and 335
allowed DATA differences, with zero forbidden differences. The DATA
digest and difference count changed by one byte since the previous boot;
all differences remain inside the three pinned environment sections.

The live power read was:

```text
ane_h17_ps pmgr off=0x290 word=0x23ff target=0xf actual=0xf
ane_h17_ps RESULT stage=3 verdict=PASS reason=ane-sys-on
ane_sys                         off-0                           0
H17_STAGE3_PASS
```

This proves the Linux PMGR provider can turn the ANE_SYS domain on and
off on J700. It does not prove the ANE CPU, DARTs, mailbox, or inference
work. The staged run submitted no task and touched no ANE or DART MMIO.
No ANE, DART, external-abort, or PMGR fault appeared around the stage 3
run. The pending marker was consumed, the modules are unloaded, and the
one-time service was disabled. `omarchy-apple-silicon-boot-check
--boot-chain` still exits 0.
