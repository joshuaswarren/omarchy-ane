# 2026-10-03-ane-h18: H18 b0, what a Linux driver has and lacks (T8152 M6, T8150 A19 Pro)

This b0 record uses only data that was already on the lab host. No IPSW member or kernelcache was fetched or opened. No firmware was decrypted or disassembled, and no machine was touched. The firmware facts are hashes, header fields, section names, record tags and identifier names. Each fact is marked **MEASURED** (read in this run from the cited file) or **INFERENCE** (a reading of measured facts that no source confirms). Repository paths are at omarchy-ane `16cfa87` unless a commit is given.

## Decision

**T8152 (Mac mini M6): the `ane_t6021` family, as a new `ascwrap-v8` variant.** The `ane_t6021` family boots the ANE ASC firmware from Linux, then talks RTKit/RTBuddy and CSNE to it. The H18 images are that kind of firmware. They carry the same RTKit release string, the same 97 `CSNE_CMD_*` names, and every `_rtk_patchbay` record tag of the macOS 27.0 H14 Selene image (the image family `ane_t6021` runs). T8152 is not the H13 host-TM family of `ane.ko`, because the firmware holds the TM and engine drivers (`CSneTMDrvH18g`, `CAneEngineExeLoopH18g`). It is not the Exclave family, because Apple binds it with `AppleASCWrapV8ANE` and no `IOExclaveProxy`. The variant changes the ASC position, the DART generation, the firmware container and the power-domain source, so most of the per-SoC table is still empty (see "b1 design").

**T8150 (iPhone 17 Pro, A19 Pro): the Exclave family.** Its ANE is `ane,t8132exclave`, which Apple binds to `H11ANEIn` with `IOExclaveProxy=true`. No public Linux boot path for an iPhone exists, so no driver and no b1 apply to it.

**b1 stays a plan.** A firmware-boot smoke module needs the 28 entries of the table below. One is a lab choice, 8 have a measured value (one of them lacks the interrupt names), 6 have only an inferred value, and 13 have no local value.

## Inputs

| Input | Path | SHA-256 |
| --- | --- | --- |
| H18 ANE0 firmware member (macOS 27.0, 26A428) | `/tmp/h131/26A428__MacBookPro17,1/Firmware/ane/h18_ane0_fw_kirkland_j8xx.im4p` | `3a1bb26439c28a146546810de8096717ada8b8eb8052156330c5b16d021293b8` |
| H18 ANE1 firmware member | same directory, `h18_ane1_fw_kirkland_j8xx.im4p` | `1571c3da969e76fbf013d25b84c43f39eae151bf6408b8cce3b6ea53a986e988` |
| H14 Selene member (27.0 comparator) | same directory, `t602x_ane0_fw_selene_rc4x.im4p` | `ad9151af4e732575a67ce007d385413b3d55a8eecffc562085678daa5bac151a` |
| H17 Hyperion member (27.0 comparator) | same directory, `h17_ane0_fw_hyperion_j71y.im4p` | `ac48eb953c68752d0b4f69ac830adf5ad60019325a48bf1ee72997cf97b9c5a8` |
| 27.0 ADT summary (all Mac boards) | notebook `artifacts/AneAllSoc/every-soc/receipt/adt-27.0.txt` | `04fe38b8ee009700f02e2c01e83bf608afab5e843f5878a83f162973a1ffdb84` |
| Predecessor artifacts | notebook `artifacts/AneGenH18/ane-gen-h18/` (8 files, `SHA256SUMS` checks OK) | see that file |
| T8152 / T8150 records | `data/ane-soc/t8152.json`, `data/ane-soc/t8150.json` at `16cfa87` (equal to the predecessor artifact copies) | `ccd6e802…`, `1809ba5d…` |
| aurora-wip T8152 board DT | aurora-silicon/linux `aurora-wip` 9845b1b9a313, `arch/arm64/boot/dts/apple/t8152-j873g.dts` (blob `6b5e1403`) | `9f56c85ced3ba443f7b62f31c92d284aa25f82ee8f2f9e86bf5ab0f2be904e00` |
| Linux DART driver | omarchy-linux 57f8f6deaa3a and aurora-ane-wt bc41726b524d, `drivers/iommu/apple-dart.c` | `f48a05cf…`, `8978b46a…` |

The four firmware member hashes equal the predecessor's IPSW member hashes: `data/ane-soc/t8152.json` S4, S5 and S7 for ANE0, ANE1 and Selene, and `data/ane-soc/t6050.json` F1 for Hyperion. Three notebook scripts made the measurements: `h18_fw_names.py` (IM4P fields, Mach-O header, section names, `_rtk_patchbay` tags and lengths, keyword identifier names; no values, no addresses), `h18_layout.py` (engine-relative arithmetic on the ADT tuples) and `irq_pairs.py` (ANE/DART interrupt pairing over the ADT summary). Their outputs are `fw-names.json`, `layout.txt` and `irq_pairs.txt` in notebook `artifacts/AneH18Driver2/h18-b0/`, listed in its `SHA256SUMS`.

## T8152: what is known

### Firmware (MEASURED, `fw-names.json`)

- Both members are IM4P with four DER fields: `IM4P`, a type tag, version `1`, and one OCTET STRING. There is no KBAG. The type tag is `anef` for ANE0 and `ane1` for ANE1. The payloads are Mach-O (`cffaedfe`), CPU type `0x100000c`, subtype `0x80000002`, `MH_PRELOAD`, flags `0x200001`, entry PC 0.
- Each image has seven load commands (`sizeofcmds` 0xb38): three `LC_SEGMENT_64` (`__TEXT` vm 0 size 0xc4000, `__DATA` vm 0xc4000 size 0x2ac000, empty `__DATA_CONST` vm 0x370000), `LC_SYMTAB` with **0 symbols**, `LC_DYSYMTAB`, `LC_UUID`, `LC_UNIXTHREAD`. The 27.0 Selene and Hyperion images have the same seven-command shape.
- `__TEXT` sections: `__text`, `__cstring`, `__const`, `ce_env` (0x4000), `text_env`, `__constructor`, `__init_offsets`. Selene has no `ce_env`; Hyperion (H17) has it. `__DATA` sections are the Selene set: `_rtk_power`, `_rtk_patchbay` (0x282), `_rtk_tunables` (0x7e0; Selene 0x5b0), `_rtk_boot`, `_rtk_page_tables`, `_fwinfo`, `_rtk_mtab` and others.
- `_rtk_patchbay` parses completely (642 of 642 bytes) as 45 records `{tag[4], u32 len, value}`, the record format of `receipts/2026-10-01-t8112-ane/README.md:78`. Tags in memory byte order: `BECA SECA 1IEC 2IEC GKTS otcd oscd oecd DILS SRSA GFCM SSSC qF8v LRSD SVSD LCSD ZSTR BPTP TNGI oeNS SEBC ECAP BVTP sP1T BtpG StpG BlpP SPTP SZSD LZSD S2xG SNUT ZNUT OTTR LLTR RIrW _COS RCOS dApC dArW fVED GLFp ABOI ZSOI ARcM`. ANE0 and ANE1 have the same list.
- Against the 27.0 Selene image (40 records), H18 keeps all 40 tags and adds five: `BECA`, `SECA`, `1IEC`, `2IEC`, `RIrW`. Hyperion (H17, 42 records) has `BECA` and `SECA`, so `1IEC`, `2IEC` and `RIrW` are new in H18. The five records that `ane_t6021` writes on T6021 (`GKTS`, `_COS`, `RCOS`, `dApC`, `dArW`; `ane/t6021/ane_fw_validate.h:52-56`) are all present.
- RTKit: `RTKSTACKRTKit_release-3514.0.15.release` in all three 27.0 images. The `CSNE_CMD_*` name sets of H18, Selene and Hyperion are equal (97 names).
- Power control: the class is `CPowerControlServiceAneH18g` (`PowerControl_H18g`). H18 has `InitPMUBaseAddress`, which Selene and Hyperion do not have, and a log string marks `SetPMUBaseAddress` as deprecated.
- Mailbox and endpoint names in the image: `Mailbox`, `MailboxPool`, `pMailbox`, `Doorbell`, `DoorBellReg`, `DoorBellBit`, `ChMan`, `ChManHost`, `ChManTarget`, `Endpoint`, `EndPointUnset`, and the commands `CSNE_CMD_IPC_ENDPOINT_SET`, `_SET2`, `_UNSET`, `_UNSET2`, `_TYPE_DATA_CHAINING`, `CSNE_CMD_SET_SNE_PMU_BASE`, `_BASE2`. These are firmware-side names. The image does not name the host RTBuddy endpoints or their numbers.
- H18-only source files against Selene: `CAneEngineExeLoopH18g`, `CSneCEDrv`, `CSneCEDrvH18g`, `CSneItqDrvH18g`, `CSneItqDrvHx`, `CSneMCWDrv`, `CSneMCWDrvH15`, `CSneTMDrvH18g`, `CPowerControlServiceAneH18g`.

### ADT layout (MEASURED offsets, `layout.txt`; ADT values from `adt-27.0.txt:311-316`)

| Window | `ane` | `ane1` | Engine-relative |
| --- | --- | --- | --- |
| reg[3] engine | 0x418000000+0x2000000 | 0x480000000+0x2000000 | +0 |
| reg[0] | 0x419600000+0x74000 | 0x481600000+0x74000 | +0x1600000 |
| reg[1] | 0x419050000+0x4000 | 0x481050000+0x4000 | +0x1050000 |
| reg[2], reg[4..9] | 0x316000000+0x1c000, 0x300700000+0x1c000, 0x300724000+0x4000, 0x3003c0000+0x40000, 0x211000000+0xff4000, 0x3082c8000+0x4000, 0x301200000+0x8000 | the same | outside the engine |
| `dart-ane`, `dart-ane1` (`dart,gen3`) | 0x419800000, …820000, …840000, …860000 (each +0xc000), 0x419810000+0x4000 | the same at 0x4818… | +0x1800000, +0x1820000, +0x1840000, +0x1860000, +0x1810000 |
| Interrupts | 311 271 788 801 765; DART 789 | 312 272 836 813 813; DART 837 | — |

- In the 27.0 summary, each of the 13 Mac SoCs with an ANE row has an ANE line n and a DART line n+1 (`irq_pairs.txt`: `socs_ok 13`, `exceptions 0`), for example T8110 520/521 (`adt-27.0.txt:220-221`), T6020 884/885 (:42-43), T8122 524/525 (:229-230), T8132 629/630 (:243-244), T8140 655/656 (:282-283) and T8142 751/752 (:288-289). On T8152 the pairs are 788/789 and 836/837 (MEASURED). INFERENCE: 788 and 836 are the engine lines; the other four lines of each node are the ASC lines. The ADT summary gives no interrupt names.
- T8150's ANE engine (0x480000000) and DART (0x481800000…) are at the addresses of T8152 `ane1` and `dart-ane1` (`layout.txt`). T8150 also has the T8152 windows at 0x300700000 (size 0x18000 against 0x1c000), 0x300724000, 0x3003c0000, 0x211000000 and 0x3082c8000 (`data/ane-soc/t8150.json` `ane.reg` against `t8152.json` `ane_reg`; MEASURED). INFERENCE: A19 and M6 share one address map, and M6 adds a second ANE.
- Power: the `ane` clock-gates resolve to `DCS2` and `DCS3`, the `ane1` clock-gates to `AFIMCCNI1` and `AFINS0`, and the main pmgr table has no power-state device with `ANE` in its name (`adt-27.0.txt:313-316`). In the same summary T8122 and T8142 list `ANE_SYS`, `ANE_CPU` and related devices, and `t8150.json` `pmgr.ane_domains` lists them for T8150. INFERENCE: the T8152 ANE power states are in a place that the summary script does not read, or have other names. The ADT summary cannot tell which.

### ASC wrapper and host side

- ADT compatible `iop-ane,ascwrap-v8`; Apple's personality `AppleASCWrapV8ANE` in `com.apple.driver.AppleA7IOP-ASCWrap-v8` (predecessor `h18-firmware-kext.txt` line 8; MEASURED there, not re-read here). T8122 (M3) is `iop,ascwrap-v6` with reg[0] at engine+0x1400000, size 0x6c000; T8152 has reg[0] at engine+0x1600000, size 0x74000 (`layout.txt`, MEASURED).
- `ane/t6021/ane_t6021.h:84-88` records that the K14 kext names CPU-control address 0x1600044 for its "h16g/h17/h18g variant block", and 0x1400044 for h14g. INFERENCE: the v8 ASC wrapper is reg[0], with CPU_CONTROL at engine+0x1600044 and CPU_STATUS at +0x1600048.
- T6021 has the ASC mailbox at wrapper+0x8000 (live read, `ane_t6021.h:121-147`), and every ASC mailbox in aurora's T8122 tree is at its ASC base+0x8000 (`t8122.dtsi:833,899,995,1059` at aurora bc41726b524d). INFERENCE: the T8152 mailboxes are at 0x419608000 and 0x481608000. On T6021, engine+0x1050000 is RVBAR (`ane_t6021.h:100-109`). INFERENCE: T8152 reg[1] holds RVBAR.
- The T6021 MBI block (SCRATCH at engine+0x1840048, doorbell at +0x1844000, `ane_t6021.h:195-201`) falls inside T8152 `dart-ane` reg[2] (0x419840000+0xc000) (`layout.txt`, MEASURED). So the T6021 MBI offsets do not carry over, and the H18 MBI position is unknown.
- `apple-dart.c` matches only `apple,t8103-dart`, `apple,t8103-usb4-dart`, `apple,t8110-dart` and `apple,t6000-dart` (omarchy-linux 57f8f6deaa3a :1628-1631; aurora-ane-wt bc41726b524d :2361-2364; MEASURED). No Linux driver claims a `dart,gen3` model.
- The aurora-wip T8152 DT (blob `6b5e1403`) has an AIC3 node with label `aic` (line 220), cpufreq, UART and framebuffer. It has no ANE, DART, pmgr or mailbox node, and its header says peripheral power management stays with the boot firmware (line 7) (MEASURED).
- BuildManifest flags: ANE0 is `IsLoadedByiBoot=true`, ANE1 is `false` (`data/ane-soc/t8152.json` `firmware.buildmanifest_flags`). INFERENCE: Linux must stage the ANE1 image itself; the driver-owned copy mode of `ane/t6021/ane_t6021_fwload.c:21-32` does that.

## T8152 against the `ane_t6021` per-SoC table

`struct ane_t602x_soc` (`ane/t6021/ane_t6021.h:736-763`) and the layout constants of `ane_t6021.h` are the table that a T8152 variant fills.

| # | Entry | T6021 value (source) | T8152 value | Status |
| --- | --- | --- | --- | --- |
| 1 | DT compatible | `apple,t6021-ane` | lab choice, for example `apple,t8152-ane` | n/a |
| 2 | Engine window | 0x284000000+0x2000000 | 0x418000000 / 0x480000000, +0x2000000 | MEASURED |
| 3 | ASC wrapper base | engine+0x1400000 (`ane_t6021.h:118-119`) | engine+0x1600000, size 0x74000 | MEASURED offset, INFERENCE role |
| 4 | CPU_CONTROL (RUN bit 4) | +0x1400044 (`:107`) | +0x1600044 | INFERENCE |
| 5 | CPU_STATUS | +0x1400048 (`:108`) | +0x1600048 | INFERENCE |
| 6 | RVBAR | +0x1050000 (`:109`) | reg[1] +0x1050000 | MEASURED offset, INFERENCE role |
| 7 | ASC mailbox | +0x1408000 (`:141-147`) | +0x1608000 | INFERENCE |
| 8 | MBI SCRATCH / doorbell | +0x1840048 / +0x1844000 (`:195-201`) | unknown; the T6021 offsets fall in a DART window | missing |
| 9 | Safe-read map | fatal 0x1854000..0x1c04000 (`:158-163`) | unknown | missing |
| 10 | Interrupts | ANE 884, DART 885 | 311 271 788 801 765 / DART 789 (ane1: 312 272 836 813 813 / 837) | MEASURED numbers, names missing |
| 11 | DART windows | dart,t8110, 4×0x4000 | dart,gen3, 4×0xc000 + 0x4000 | MEASURED |
| 12 | DART register model in Linux | `apple,t8110-dart` | no `dart,gen3` variant | missing |
| 13 | DART SID, VM base, page size | live DT | not in the IPSW ADT (`t8152.json` `dart_sid`) | missing |
| 14 | Power domains and order | eight, `ane_t6021.h:42-47` | none named in the ADT summary | missing |
| 15 | `ps_cpu_off`, `pwgate_off` | 0x2e0, 0 (`ane_t6021_fwload.c:209-214`) | unknown | missing |
| 16 | `pmu_pa`, `ps_off` | 0x28e084000 | unknown; H18 marks `SetPMUBaseAddress` deprecated | missing |
| 17 | `RTK_soc` (`_COS`) | 0x6021 | 0x8152 candidate; on T602x iBoot ORs in fuse bits (`receipts/2026-10-01-t8112-optin/README.md:68-71`) | INFERENCE |
| 18 | `RTK_soc_revision` (`RCOS`) | 0x11 | unknown | missing |
| 19 | `RTK_cpu_physical_address` (`dApC`) | 0x285000000 (engine+0x1000000) | unknown | missing |
| 20 | `RTK_cpu_wrapper_physical_address` (`dArW`) | 0x285400000 (wrapper base) | 0x419600000 / 0x481600000 (reg[0]) | INFERENCE |
| 21 | New records `BECA SECA 1IEC 2IEC RIrW` | not present | values unknown | missing |
| 22 | ASC tunables block | 24 records (`ane_fw_validate.h:281-316`) | unknown | missing |
| 23 | Firmware image pin | 13.5 Selene `a9c4b771…` | 26A428 payloads `830505bc…` (ANE0), `5d3ed4c7…` (ANE1); the version the owner's boot stub preloads is unknown | MEASURED for 26A428 |
| 24 | Image validator | exactly 5 load commands, 2 segments, symbol-located patches (`ane_fw_validate.h:38-42,52-63`) | 7 load commands, 3 segments, 0 symbols: patches must be found by walking `_rtk_patchbay` | MEASURED |
| 25 | Firmware placement (`segment-ranges`) and entry IOVA | `ane_t6021_fwload.c:122-127` | not in the IPSW ADT | missing |
| 26 | Transport | 13.5 legacy ChMan, no RTKit HELLO (`docs/t6021-ane-bringup-findings.md:1120-1131`) | 27.x: HELLO first, then CSNE (`docs/t6021-ane-bringup-findings.md:809-815`, from the 27.0 kext) | INFERENCE |
| 27 | ANE1 firmware source | n/a | not iBoot-loaded | MEASURED flag |
| 28 | Base DT nodes for the overlay | `packaging/dt/t6021-ane.dts` | aurora-wip has none | missing |

Summary: 1 lab choice (1), 8 MEASURED (2, 3, 6, 10, 11, 23, 24, 27; row 10 lacks the interrupt names), 6 INFERENCE (4, 5, 7, 17, 20, 26), 13 missing (8, 9, 12, 13, 14, 15, 16, 18, 19, 21, 22, 25, 28).

## T8150 (A19 Pro): what is known

All values are in `data/ane-soc/t8150.json` (sources S1, S2, S6, S7 from the iOS 27.0.1 24A446 iPhone18,1 IPSW; the predecessor read them and this run did not re-read the members). One ANE node `ane,t8132exclave`, `ane-type` 768, six reg windows from engine 0x480000000, IRQs 607 and 620. DART `dart,t8110` at 0x481800000 with SID list, VM base 0x10000000000, VM size 0x30000000000, page size 16384. pmgr `ANE_SYS` 0x300700210, `ANE_CPU` 0x30070c000, `ANE_TDBASE` 0x30070c008, `ANE_MPM` 0x30070c010. Firmware `h18_ane_fw_apollo_v5x.im4p` (not encrypted). Host personality `H11ANEIn`, `IOExclaveProxy=true`, `exclave-edk-service=com.apple.service.ANEExclave_EDK`.

Missing for Linux: everything at run time. The ANE belongs to the exclave on iOS (INFERENCE from `IOExclaveProxy` and the EDK service name), and no public method boots Linux on an A19 iPhone. No owner input changes this, and `omarchy-ane-probe` cannot run there. This run changes nothing in `t8150.json`.

## Missing inputs, and what the owner of an M6 Mac can supply

### What `omarchy-ane-probe` gives today

`omarchy-ane-probe` is a Linux tool. It reads `/proc/device-tree`, sysfs and three fixed commands (`tools/omarchy-ane-probe:4-13`, `docs/ane-probe.md:20-41`). It does not read the Apple ADT. On an M6 that boots the current aurora-wip T8152 DT, it reports `soc`, `board` and `compatible`, an empty `ane_nodes` list (the DT has no ANE node), the kernel, and `dmesg` lines that name `ascwrap` or `dart-ane`. Run it as a normal user and send the output:

```sh
omarchy-ane-probe --pretty --max-kib 0
```

That output confirms the board (j873g), the SoC and the kernel. It cannot supply any entry of the table above. Its `soc_table` section cannot check T8152 either: it compares only `boards` and the `ane`, `dart` and `mailbox` objects of the data file (`tools/omarchy-ane-probe:397-417`), while `t8152.json` keeps its ANE and DART facts under flat keys (`ane_compatible`, `ane_reg`, `dart_regs`, …), and the probe's board string is `apple,j873g` (`:445`) against the table's `j873g`. So `soc_table` becomes the DT acceptance check for b1 only after the record moves to that shape and the board formats agree (b1 prerequisite).

### What the owner can supply from macOS (read-only, no sudo, no network)

The IODeviceTree plane in macOS shows the ADT as iBoot passed it, including `segment-ranges` (on T6021, macOS 27 `ioreg` and the m1n1 ADT give the same value: `tools/t8112-kit/README.md:13`). The T8112 collector `tools/t8112-kit/collect-macos.sh:36-38` saves only the first node of each name, so it misses `ane1` and `dart-ane1`. On the M6, in macOS:

```sh
for n in ane ane1 dart-ane dart-ane1 pmgr; do
  ioreg -a -p IODeviceTree -r -n "$n" > "ioreg-$n.plist"
done
ioreg -p IODeviceTree -r -n arm-io -d 1 |
  grep -E '"(compatible|chip-revision|fuse-revision|soc-generation)" =' > arm-io.txt
ioreg -p IODeviceTree -r -n chosen -d 1 |
  grep -E '"(chip-id|board-id|firmware-version|system-firmware-version)" =' > chosen.txt
ioreg -p IOService -r -n ane -d 4 > iosvc-ane.txt
ioreg -p IOService -r -n ane1 -d 4 > iosvc-ane1.txt
sw_vers > sw_vers.txt
shasum -a 256 -- * > SHA256SUMS
```

| Missing entry | Where it comes from | Owner can supply |
| --- | --- | --- |
| 25 firmware placement, entry IOVA | `segment-ranges` in `ioreg-ane.plist`, `ioreg-ane1.plist` | yes |
| 13 DART SID, VM base, page size | `ioreg-dart-ane*.plist` | yes, if macOS shows them |
| 14, 15, 16 power states, ps words | `ioreg-pmgr.plist` device and ps-register tables | yes (names, addresses); order needs more |
| 10 interrupt names | `interrupt-names` or the mailbox child of `ioreg-ane*.plist`, if present | maybe |
| 18 `RCOS` candidate | `arm-io.txt` `chip-revision` (a candidate only: `tools/t8112-kit/README.md:11`) | yes, as a candidate |
| 23 firmware pin | `chosen.txt` `firmware-version`, `sw_vers.txt`: the macOS version whose ANE image iBoot preloads | yes |
| Host endpoint names | `iosvc-ane*.txt`: the RTBuddy endpoint nubs under `AppleASCWrapV8ANE` (T6021 shows `ANEEndpoint1..5`, `ane_t6021.h:179-180`) | yes |

### What needs more than the owner's macOS

- **Entries 18, 19, 20, 21, 22 (the values iBoot writes into the image).** On T6021 the preload differs from the file only in the fields that iBoot patches (`ane_fw_validate.h:243-250`). The lab got them from preload captures through the m1n1 proxy (`tools/t8112-kit/README.md`, Route B). No local data shows an m1n1 or other loader that can read the ANE preload on T8152. Prerequisite: a T8152 loader with proxy mode, then one capture per boot stub version.
- **Entries 4, 5, 7, 8, 9 (ASC and MBI register map, safe reads).** The T6021 values came from the 13.5 kext and live reads. For T8152 the matching sources are the `AppleA7IOP-ASCWrap-v8` and ANE kexts, in the public `kernelcache.release.mac18g` (`t8152.json` S6) and in the owner's macOS. This run did not read them. Until a source exists, a driver must read no T8152 register outside a cited list.
- **Entry 12 (`dart,gen3`).** A Linux `apple-dart` variant needs the gen3 register model. The source is the same kernelcache or a description from the `apple-dart` maintainers.
- **Entry 14 order and entry 28.** A base DT with pmgr power-state nodes for the ANE islands, and ANE, DART and mailbox nodes. aurora-wip has none today.
- **An M6 compile oracle.** The H18 HWX target id, CPU subtype and ISA for T8152 are unknown (`t8152.json` `hwx_generation`).

## b1 design (plan, not code)

b1 is an opt-in module that boots the T8152 ANE0 firmware and stops at the RTKit HELLO. It runs no model. It is not written, because 13 of its 28 entries have no local value and 6 more rest on inference.

- **Files.** New files only: `ane/h18/ane_h18.c` and a Makefile; a data-only overlay change stays separate. The module does not touch `ane_t6021.ko`, so the proven T6021 path cannot change. It reuses `ane/t6021/ane_fw_validate.h` for the SHA-256 pin and gains one function that finds patch records by walking `_rtk_patchbay` instead of `LC_SYMTAB`.
- **Binding.** Compatible `apple,t8152-ane` (a lab choice). No `MODULE_DEVICE_TABLE`, so udev never loads it. It binds only with `ane_h18.experimental=1` and an `okay` node from a base DT (entry 28). The data-only overlay stays disabled, and `omarchy-ane-dt` never applies it.
- **Sequence.** Each step runs only after its table entries have a source; the module refuses to probe while any entry is null. (1) Raise the ANE power domains in the sourced order through genpd and check ACTUAL=0xf before any engine read, as the T6021 probes do (`ane/t6021/probes/ane_afbridge_probe.c:11-14`). (2) Attach `dart-ane` through `apple-dart` with a gen3 variant, map the PMU page if the firmware needs it, and map the driver-owned image copy. (3) Write the iBoot patch records and the ASC tunables block. (4) Program RVBAR, set RUN in CPU_CONTROL, and start `apple_rtkit` on the reg[0]+0x8000 mailbox. (5) Wait at most 1000 ms for HELLO, then log the result and stop. After the CPU starts, the module holds every surface until reboot (the wedged-pin rule of `ane/t6021/ane_t6021_boot.c:465-506`).
- **Pass/fail.** Pass: dmesg shows HELLO and the endpoint map within 1000 ms, no DART fault, and `omarchy-ane-probe` reports the node bound. Fail: any timeout or fault; the machine reboots to clear it. Only the owner runs it, on their own M6. The lab has no M6.
- **ANE1.** Out of b1 scope. It needs its own staged image (not iBoot-loaded) and the same table for the `ane1` windows.

## Validation

- `python3 tools/validate_ane_soc.py data/ane-soc/t8152.json data/ane-soc/t8150.json` on this branch: `ok t8152 state=data-only sources=10 leaves=10` and `ok t8150 state=data-only sources=7 leaves=7`, exit 0. `python3 tools/gen_coverage_table.py --check`: `gen_coverage_table: ok`, exit 0.
- The notebook entry `entries/AneH18Driver2/20261003T005509Z-*-h18-b0-local-only.md` records the commands, times and hashes.

## Limits

- Section names, record tags and identifier names show that a feature exists. They do not show how the firmware uses it. Equal CSNE command names do not prove equal command semantics.
- The ADT summary is a script output (`adt-27.0.txt`), not the full ADT. Absent pmgr names can be a limit of that script.
- The kext and T8150 facts come from the predecessor run; this run did not re-read the kernelcache or the iOS members.
