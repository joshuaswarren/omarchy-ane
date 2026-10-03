# 2026-10-03-ane-h17: H17 b0, what a Linux driver has and lacks (T8140 Neo, T8142 M5, T6050 M5 Pro/Max)

This b0 record uses only data that was already on the lab host: the NeoAne2 artifacts of 2026-10-02, the `data/ane-soc` records, the 27.0 ADT summary in `receipts/2026-10-01-ane-every-soc`, and the `ane_t6021` sources. No IPSW member, kernelcache, iBoot image or firmware image was fetched or opened in this run, and no machine was touched. Each fact is marked **MEASURED** (read in this run from the cited file, or computed from cited values) or **INFERENCE** (a reading of measured facts that no source confirms). Repository paths are at omarchy-ane `16cfa87`. "Notebook" paths are in the private lab notebook; the facts quoted from them are addresses, sizes, names, flags and digests.

## Decision

**H17 is the `ane_t6021` family: Linux must start the ANE firmware, then speak RTKit/RTBuddy and CSNE to it.** The evidence:

- The macOS 27.0 kernelcaches of all three SoCs bind `ane,t8132exclave` to `H11ANEIn`, the class that binds `ane,t8020` on every M1 and M2 (notebook `NeoAne2/logs/kext-personalities.txt:11-12, 29-30, 49-50`). MEASURED.
- The H17 Neo image has the same 96 `CSNE_CMD_*` names as the 27.0 H14 Selene image, which is the image family `ane_t6021` runs, and the same `RTKit_release-3514.0.15` string (`fw-kext-strings-summary.txt:1, 6`). MEASURED. Equal names do not prove equal semantics.
- The firmware holds the TM and engine drivers (`CSneTMDrvH17`, `CSneCEDrvH17`, `PowerControl_H17`; `fw-kext-strings-summary.txt:5`). So `ane.ko`, which drives the H13 TM from the host, does not fit. MEASURED names; the conclusion is INFERENCE, the same one that `docs/t6021-ane-bringup-findings.md` draws for H14.
- It is not an `ascwrap-v6` IOP: `AppleASCWrapV6` matches only `iop,ascwrap-v6` and `iop,ascwrap-v7` (`kext-personalities.txt:3`), and the H17 ANE node has no such compatible. MEASURED.
- The H17 ADT node has the window layout of the M4 (T8132, H16) `ane,t8020` node: six windows with the same sizes and the DART at the same engine offsets (notebook `AneH17Driver2/logs/layout.txt`; `receipts/2026-10-01-ane-every-soc/adt-27.0.txt:243-244` against `:282-283` and `:288-289`). The H17 node adds the compatible `ane,t8132exclave` and the `exclave-*` properties. MEASURED. INFERENCE: an H17 driver is the H16 variant of `ane_t6021` plus a decision about the exclave.

**The exclave marking is open, not a known lockout.** The ADT marks the ANE `exclave-assigned` on all three SoCs (`adt-h17-ane.txt:76, 220, 382`). MEASURED. The macOS ANE class, an xnu kext, has the strings "Exclave assigned to ANE node (Legacy)", "Exclave mode switch in progress", "Exclave work paused", "Exclave feature supported but disabled by default" and "ANEExclaveProxy service not available. Exclave disabled" (`fw-kext-strings-summary.txt:8`, items 6, 20, 24, 33, 48 of the 60 that the log keeps), and the firmware has `CSNE_CMD_EXCLAVE_MODE_START` and `_STOP` (`:2`). MEASURED strings. INFERENCE: on macOS the xnu kext boots and drives the ANE and lends it to the exclave by a mode switch, so `exclave-assigned` names a second user of the engine, not an owner that keeps xnu out. Only the b1 firmware-boot smoke on a Neo can show whether Linux reaches it.

**b1 stays a plan.** Of the 27 entries in the `ane_t6021` comparison below, 1 is a lab choice, 9 have a measured H17 value, 7 have only an inferred value, and 10 have no local value.

## Inputs

| Input | Path | SHA-256 |
| --- | --- | --- |
| NeoAne2 feasibility report | notebook `artifacts/NeoAne2/feasibility-report.md` | `993bc8d7…` |
| H17 ADT excerpts (j700, j704, j714c; macOS 27.0 26A428) | notebook `artifacts/NeoAne2/logs/adt-h17-ane.txt` | `3f591275…` |
| Kext personalities (mac17p, mac17g, mac17j) | notebook `artifacts/NeoAne2/logs/kext-personalities.txt` | `a1c627f4…` |
| Firmware and kext string summary | notebook `artifacts/NeoAne2/logs/fw-kext-strings-summary.txt` | `99b3e4cb…` |
| IM4P and Mach-O headers (four H17 images) | notebook `artifacts/NeoAne2/logs/im4p-h17.txt` | `d0067c5a…` |
| IM4P headers (27.0 H14, H15, H16 comparators) | notebook `artifacts/NeoAne2/logs/im4p-compare.txt` | `a05a159d…` |
| H17 records | `data/ane-soc/t8140.json`, `t8142.json`, `t6050.json` | `828d26c6…`, `028e4d30…`, `f79c90e4…` (before this branch) |
| H16 comparator record | `data/ane-soc/t8132.json` | `67bc9e5e…` |
| 27.0 ADT summary, all Mac boards | `receipts/2026-10-01-ane-every-soc/adt-27.0.txt` | `04fe38b8…` |
| `ane_t6021` sources | `ane/t6021/ane_t6021.h`, `ane_fw_validate.h`, `ane_t6021_fwload.c`, `packaging/dt/t602x-ane.dtsi` | `e6ec12c9…`, `90f7a87e…`, `6fa255f9…`, `e48344f1…` |
| Layout arithmetic | notebook `artifacts/AneH17Driver2/scripts/h17_layout.py` → `logs/layout.txt` | `1dbdca7d…` → `2ce954f5…` |

`sha256sum -c` of the NeoAne2 `SHA256SUMS` passes for all 18 files (2026-10-03). `h17_layout.py` reads the four `data/ane-soc` records and `adt-h17-ane.txt`. It decodes the `dart-ane` `instance` property, computes every window as an offset from the engine base, and checks the T6021 `ane_t6021.h` offsets against the H17 windows.

## Per-SoC facts

All rows are MEASURED unless the cell says otherwise. `adt` = `adt-h17-ane.txt`, `layout` = `layout.txt`.

| Fact | T8140 (j700, MacBook Neo) | T8142 (j704, M5) | T6050 (j714c, M5 Pro/Max; j775d) | Source |
| --- | --- | --- | --- | --- |
| ADT node | `/arm-io/ane`, `ane,t8132exclave`, ane-type 512 | `/arm-io/ane`, `ane,t8132exclave`, ane-type 528 | `/arm-io/ane0`, `ane,t8132exclave`, ane-type 544, die-id 0. j775d adds `ane1`, `ane,t8020`, die-id 1, at 0x4308000000 | adt:66-68, 210-212, 366-386; `adt-27.0.txt:198` |
| Exclave properties | `exclave-assigned`, `exclave-service` `com.apple.service.ANEExclave` (+`_EDK`), `exclave-reg` | the same | the same on `ane0`; `ane1` not read | adt:71-81, 215-224, 367-382 |
| ASC wrapper version | no `iop`/`ascwrap` compatible and no ASC or mailbox node for the ANE in the ADT; the ANE node is the IOKit provider itself | the same | the same | feasibility-report.md:66-68; kext-personalities.txt:3, 12 |
| ASC wrapper position | INFERENCE: engine+0x1600000. `ane_t6021.h:84-88` records that the K14 kext uses CPU_CONTROL 0x1600044 in its "h16g/h17/h18g variant block" and 0x1400044 for h14g. The kext reads an `ASCWRAP_IDLE_STATUS` register (`fw-kext-strings-summary.txt:11`) | the same | the same | as cited |
| Mailbox and endpoint names | ADT: no ANE mailbox. Other IOPs have exclave mailboxes (`aop-exclave-mailbox`, `iop,secure-rtbuddy-proxy`) and `exclave-endpoint` 77 and 78 on their ioreporting nodes. Kext: `RTBuddyBringup` personalities for role `ANE` and `ANE1`. Firmware: `CSNE_CMD_IPC_ENDPOINT_SET`, `_SET2`, `_UNSET`, `_UNSET2`, `CSNE_CMD_EXCLAVE_MODE_START/STOP`, `CSNE_CMD_SECURE_MODE_*` | ADT and kext: the same. Firmware strings not scanned | ADT and kext: the same. Firmware strings not scanned | adt:6-59, 184-200, 340-356; kext-personalities.txt:9-10, 27-28, 47-48; fw-kext-strings-summary.txt:2 |
| Endpoint numbers | not in local data. T6021 reference: RTBuddy nubs `ANEEndpoint1..5`, EP0 management (`ane_t6021.h:179-180, 191-192`) | the same | the same | — |
| Engine window (reg[0]) | 0x400000000+0x2000000 | 0x420000000+0x2000000 | 0x508000000+0x2000000; j775d `ane1` 0x4308000000+0x2000000 | adt:80, 223, 385; `adt-27.0.txt:198` |
| Other windows | 0x300700000+0x18000 (pmgr; ANE_SYS ps at +0x290), 0x300724000+0x4000, 0x3003c0000+0x30000, 0x211000000+0xff4000, 0x3082c8000+0x4000 | 0x380700000+0x18000, 0x380724000+0x4000, 0x3803c0000+0x40000, 0x211000000+0xff4000, 0x3882c8000+0x4000 | 0x280900000+0x18000, 0x280920000+0x4000, 0x284240000+0x40000, 0x212000000+0xff4000, 0x584000000+0x2000000 (engine+0x7c000000), 0x288374000+0x4000 | adt; layout |
| `exclave-reg` | 0x441c00000+0x88000 = engine+0x41c00000, outside the engine window | 0x461c00000 = engine+0x41c00000 | 0x549c00000 = engine+0x41c00000 | layout |
| Interrupts (ANE; DART) | 655, 668; 656 | 751, 764; 752 | 954, 967; 955 (j775d `ane1` 4058; 4059) | adt:73, 95, 217, 237, 380, 398; `adt-27.0.txt:198-199` |
| Power states with a ps word (offset in reg[1]) | ANE_SYS +0x290 (parent AFISOCNI2). ANE_CPU, ANE-SYS-V, ANE-SYS-V-PMP: `no_ps` | ANE_SYS +0x2e8 (parent AFISOCNI3), ANE_CPU +0xc000, ANE_TDBASE +0xc008, ANE_MPM +0xc010; ANE-SYS-V, -PMP `no_ps` | ANE_MPM +0x3c0, ANE_CPU +0x3c8, ANE_TDBASE +0x3d0 (parent id 219); ANE_SYS_V, _PMP, _DART `no_ps`; no ANE_SYS state named | adt:172-176, 320-326, 487-493; layout |
| DART | `dart,t8110`. `instance` decodes to DARTLLT, DARTBRD, DARTBWR, DAPFLLT at engine+0x1800000 (0xc000), +0x1820000 (0x20000), +0x1840000 (0x20000), +0x1810000 (0x4000) | the same names and offsets; sizes 0xc000, 0xc000, 0xc000, 0x4000 | the same as T8142; DART power gate ANE_SYS_DART (`no_ps`) | adt:84-95, 161, 227-237, 306, 387-398, 481-482; layout |
| DART streams and IOVA | sid 0, 11, 15, 10, 13; exclave-sid 1..7; sid-count 16; vm-base 0x10000000000; vm-size 0x30000000000; page 16384; dart-options 37; bypass-10, bypass-13. The ANE uses `mapper-ane` (stream 0) and `mapper-ane-mpm` (stream 11) | sid 0, 10, 11, 15, 13; the rest the same | sid 0, 10, 11, 13, 15; the rest the same | adt:85-96, 158-162, 228-240, 302-307, 388-400, 477-483; t8140.json `ane.iommus` |
| DAPF ranges inside reg[1] (pmgr) | +0x290..+0x293 (ANE_SYS ps), +0xc000..+0xc013 | +0x2e8..+0x2eb (ANE_SYS ps), +0xc008..+0xc00b (ANE_TDBASE ps) | +0x3a8..+0x3ab, +0x3c0..+0x3d3 (ANE_MPM, ANE_CPU, ANE_TDBASE ps) | layout |
| Firmware | `h17_ane_fw_theia_d9x`, payload `ddc97c20…` | `h17_ane_fw_theia_j73y`, payload `4d6d70a9…` | `h17_ane0_fw_hyperion_j71y`, payload `97f08f91…`; j775d also `h17_ane1_fw_hyperion_j71y` (fourcc `ane1`), payload `7bfc8a97…`, `Ap,ANE1` not iBoot-loaded | im4p-h17.txt:1-44; feasibility-report.md:78-79 |
| Firmware format | IM4P with four DER fields, no KBAG; arm64e Mach-O `MH_PRELOAD`, 7 load commands; `__TEXT` vm 0x0+0xc0000, `__DATA` vm 0xc0000+0x2ac000, empty `__DATA_CONST` | the same | the same (both images) | im4p-h17.txt |
| HAL kext | `AppleT8140ANEHAL` 10.19.3; tunable groups `af_aneaxi2af`, `c1ppt_rc`, `mcw`, `sne_ctrl`, `sne_intc_lock`, `sne_iso_gapf`, `soc`, `soc_dpe_lee`, `soc_dpe_temp`, `sys` (values not extracted) | `AppleT8142ANEHAL` 10.19.3 | `AppleT6050ANEHAL` 10.19.0, and `AppleANELoadBalancer` 10.19.2 | kext-personalities.txt:15-16, 33-34, 42-43, 51-52; fw-kext-strings-summary.txt:9 |

Readings of these facts (INFERENCE):

- T8140 hides three power states from the host pmgr table. Its DAPF range reg[1]+0xc000..+0xc013 covers three ps words, and on T8142 and T8132 the ANE_CPU, ANE_TDBASE/ANE_TD and ANE_MPM words are at reg[1]+0xc000..+0xc010 (layout). The DAPF ranges on all three SoCs cover ANE ps words, so the firmware (`PowerControl_H17`) sets at least part of its own power state through the DART. A host driver may need to raise fewer states than on T6021, or none past ANE_SYS.
- On T6050 the DAPF word reg[1]+0x3a8 is a candidate for the parent state (id 219) of the three named states. The ADT summary gives no name for it.
- The low 20 address bits of reg[5] on T8140 (0x3082c8000) and T8142 (0x3882c8000) match the page of the T8112 eFuse window 0x23d2c8060 (`ane_t6021.h:21-25`). It can be a chip-revision fuse window. Only the low bits match.

## Against the `ane_t6021` per-SoC table

`struct ane_t602x_soc` (`ane/t6021/ane_t6021.h:751-763`) and the layout constants of `ane_t6021.h` are the table that an H17 variant fills. "Missing" means that no local file has the value.

| # | Entry | T6021 value (source) | H17 value | Status |
| --- | --- | --- | --- | --- |
| 1 | DT compatible | `apple,t6021-ane` (`t602x-ane.dtsi`) | lab choice, for example `apple,t8140-ane` | n/a |
| 2 | Engine window | 0x284000000+0x2000000 (`t602x-ane.dtsi:163`) | per SoC above | MEASURED |
| 3 | ASC wrapper base | engine+0x1400000 (`ane_t6021.h:119`) | engine+0x1600000 | INFERENCE |
| 4 | CPU_CONTROL (RUN bit 4) | +0x1400044 (`:107`) | +0x1600044 (`:86-87`) | INFERENCE |
| 5 | CPU_STATUS | +0x1400048 (`:108`) | +0x1600048 | INFERENCE |
| 6 | RVBAR | +0x1050000 (`:109`) | unknown; +0x1050000 is in reg[0] and in no other ADT window | missing |
| 7 | ASC mailbox | wrapper+0x8000 = +0x1408000 (`:121-141`) | +0x1608000 if the mailbox stays at wrapper+0x8000 | INFERENCE |
| 8 | MBI SCRATCH and doorbell | +0x1840048, +0x1844000 (`:195, :201`) | these offsets fall inside the H17 DARTBWR window (layout), so they do not carry over | missing |
| 9 | Safe-read map | no reads in 0x1854000..0x1c04000 (`:162-163`) | unknown; on T8140, +0x1854000 is inside DARTBWR (layout) | missing |
| 10 | Interrupts | ANE 884, DART 885 (`ane_t6021.h:33-36`) | per SoC above; the ADT gives no interrupt names | MEASURED numbers |
| 11 | DART windows | four 0x4000 windows at +0x1800000, +0x1810000, +0x1820000, +0x1804000 (`ane_t6021.h:37-41`) | `dart,t8110`, DARTLLT/DARTBRD/DARTBWR/DAPFLLT at +0x1800000, +0x1820000, +0x1840000, +0x1810000 | MEASURED |
| 12 | DART streams, VM base, page size | from the live DT | in the IPSW ADT (table above) | MEASURED |
| 13 | DART register model in Linux | `apple,t8110-dart` (`t602x-ane.dtsi:121`) | ADT `dart,t8110`, the ADT compatible of the T6020 `dart-ane0` too (`adt-27.0.txt:43`); the three windows have new sizes, and the DAPF instance is new | MEASURED compatible; fit INFERENCE |
| 14 | Power domains and raise order | eight, `ane_t6021.h:42-47` | named above; T8140 has one host ps word. The order is not in local data | missing (order) |
| 15 | `ps_cpu_off`, `pwgate_off` | 0x2e0, 0 (`ane_t6021_fwload.c:209-214`) | T8142 ANE_CPU reg[1]+0xc000; T6050 +0x3c8; T8140 none in the host table; `pwgate_off` unknown | MEASURED offsets for T8142 and T6050; role INFERENCE |
| 16 | `pmu_pa`, `ps_off` | 0x28e084000 (`:212`) | unknown | missing |
| 17 | `RTK_soc` (`_COS`) | 0x6021 | 0x8140, 0x8142, 0x6050 as candidates | INFERENCE |
| 18 | `RTK_soc_revision` (`RCOS`) | 0x11 | unknown | missing |
| 19 | `RTK_cpu_physical_address` (`dApC`) | 0x285000000 = engine+0x1000000 (`ane_t6021.h:118`) | unknown | missing |
| 20 | `RTK_cpu_wrapper_physical_address` (`dArW`) | 0x285400000 = engine+0x1400000 | engine+0x1600000, as entry 3 | INFERENCE |
| 21 | ASC tunables block | 24 records (`ane_fw_validate.h:286-289`) | HAL group names only (table above) | missing |
| 22 | Firmware image pin | 13.5 Selene `a9c4b771…` (`ane_fw_validate.h:66-69`) | 26A428 payload hashes above. The version that the owner's boot stub preloads is unknown | MEASURED for 26A428 |
| 23 | Image validator | exactly 5 load commands and 2 segments (`ane_fw_validate.h:15, 38-41`) | 7 load commands and 3 segments, so the current validator refuses the image | MEASURED |
| 24 | Firmware placement (`segment-ranges`) | pinned, `ane_t6021_fwload.c:122-127` | not in the IPSW ADT; iBoot adds it at boot (t8140.json `ane.segment_ranges`) | missing |
| 25 | Transport | 27.x images: HELLO first, then CSNE (`docs/t6021-ane-bringup-findings.md:809-815`) | the same RTKit release string and CSNE names as the 27.0 H14 image | INFERENCE |
| 26 | Exclave mode | none | `exclave-assigned`, `CSNE_CMD_EXCLAVE_MODE_START/STOP`; the effect on a Linux host is unknown | MEASURED names |
| 27 | Base DT nodes | `packaging/dt/t6021-ane.dts` | aurora-wip has no ANE, ANE DART, mailbox or ANE power domain for T8140 (feasibility-report.md:171-176); `packaging/dt/t8140-ane-dataonly.dts` is disabled and never applied | missing |

Summary: 1 lab choice (1); 9 MEASURED (2, 10, 11, 12, 13, 15, 22, 23, 26); 7 INFERENCE (3, 4, 5, 7, 17, 20, 25); 10 missing (6, 8, 9, 14, 16, 18, 19, 21, 24, 27). Rows 13 and 15 are measured for the ADT side only.

## What a Neo or M5 owner supplies with the probe

`omarchy-ane-probe` now has a `reachability` section (`docs/ane-probe.md`). It reads `/proc/device-tree` and `/sys` only, reads no register, and needs no root. One run on a Neo or an M5 answers whether the Linux device tree gives the ANE to the OS:

```sh
omarchy-ane-probe --pretty --max-kib 0
```

| Question | Probe field | Expected on a stock aurora tree |
| --- | --- | --- |
| Does the Linux DT have an ANE node, and can a driver use it? | `reachability.verdict` (`reachable`, `owned-elsewhere`, `not-exposed`, `unknown`), `reason` | `not-exposed`: no ANE node. INFERENCE from the t8140 community row (content sha256 `5efa0ec0…`, feasibility-report.md:195-202) and aurora-wip (feasibility-report.md:171-176) |
| For each ANE node: status, compatible, windows, IOMMU, power domains, mailboxes, exclave properties, platform device and driver | `reachability.nodes[]` | empty |
| Does the DT copy the ownership property? | `nodes[].exclave_props`; `owned-elsewhere` when a node has `exclave-assigned` | no node |
| What the IPSW ADT says for this SoC | `reachability.soc_table_exclave` (from the installed `data/ane-soc/<soc>.json`) | `exclave-assigned: true`, `exclave-reg` |
| Is the live ADT copy there for a later read? | `reachability.adt_region`, `adt_mtd` | `0x10004834000+0x68000` on the t8140 row (feasibility-report.md:203); `adt_mtd` not recorded in that row |
| Identity | `soc`, `board`, `model`, `kernel` | `t8140`, `apple,j700` |

After a base DT gets ANE, DART and pmgr nodes (entry 27), the same run shows each node's state and compares it with the record (`soc_table`). That is the DT half of the b1 gate.

The probe cannot supply the missing entries 6, 8, 9, 14, 16, 18, 19, 21 and 24. Where each one can come from:

- **Entry 24 (`segment-ranges`), and whether iBoot preloads the ANE image for a non-macOS boot.** The live ADT has both. Two read-only routes exist: the phram `adt` copy that `adt_region` and `adt_mtd` report (a root read of the mtd device, outside the probe), or the IODeviceTree plane in the owner's macOS (`ioreg -a -p IODeviceTree -r -n ane`, and `-n ane0`, `-n ane1`, `-n dart-ane`, `-n dart-ane0` on T6050). The same data tells whether iBoot keeps `exclave-assigned` for a Linux boot.
- **Entry 22, the preloaded version.** `asahi,os-fw-version` is `unknown` on the t8140 row (feasibility-report.md:195-196). The owner's macOS `sw_vers` and the boot stub version answer it.
- **Entry 14 order and entry 16.** The ADT names the states (table above). The raise order and the PMU base need the macOS ANE kext or a live trace. The owner's macOS IORegistry shows the pmgr tables (`ioreg -a -p IODeviceTree -r -n pmgr`) but not the order.
- **Entries 6, 8, 9 (RVBAR, MBI, safe-read map) and the endpoint numbers.** For T6021 these came from the 13.5 kext and live reads (`ane_t6021.h:84-201`). No local source has them for H17. The owner's macOS IOService plane (`ioreg -p IOService -r -n ane -d 4`) shows the RTBuddy endpoint nubs by name.
- **Entries 18, 19, 21 (the values iBoot writes into the image).** On T6021 and T8112 the lab read them from preload captures (`tools/t8112-kit`). No local data shows such a route on a Neo or an M5.

## b1 design (plan, not code)

b1 is an experimental, opt-in module that boots the H17 firmware and stops after the first CSNE replies. It runs no model. It is not written, because 10 of the 27 entries above have no local value and 7 more rest on inference.

- **Files.** New files only: `ane/h17/ane_h17_smoke.c` and a Makefile. The module does not touch `ane_t6021.ko`, so the proven T6021 path cannot change. It reuses the SHA-256 pin of `ane/t6021/ane_fw_validate.h` with a new image description: 7 load commands, 3 segments (entry 23).
- **No autoload.** No `MODULE_DEVICE_TABLE`, so udev never loads it. DKMS does not build it by default, and no package file names it. It binds only with `ane_h17_smoke.experimental=1` to an `okay` node with the lab compatible (entry 1) from a base DT. The data-only overlay stays disabled, and `omarchy-ane-dt` never applies it.
- **Gates before probe.** `omarchy-ane-probe` reports `reachable` for that node; the module refuses `owned-elsewhere`. The module refuses to probe while any entry that a step needs is null.
- **Sequence.** (1) Raise the ANE power states through genpd in the sourced order, and read the ps words until ACTUAL = 0xf before any engine read. (2) Attach the ANE DART through `apple-dart` and map the driver-owned image copy at the `segment-ranges` IOVAs. (3) Write the iBoot patch values and the ASC tunables block. (4) Program RVBAR, set RUN in CPU_CONTROL, and start `apple_rtkit` on the ASC mailbox. (5) Wait at most 1000 ms for HELLO, then EPMAP and STARTEP, then send `CSNE_CMD_CONFIG_GET`, `CSNE_CMD_PLATFORM_INFO` and `CSNE_CMD_BUILDINFO`, log the replies, and stop. It never sends `CSNE_CMD_EXCLAVE_MODE_*`, never maps `exclave-reg`, and never programs the exclave streams 1..7.
- **Pass and fail.** Pass: HELLO, EPMAP and STARTEP, then the three CSNE replies; no DART fault; no SError; a clean next boot. Fail: a timeout or a fault. As on T6021, the module cannot unload after the ANE CPU starts; a reboot clears it, and the next boot runs without it. A hang needs only the power button.
- **Who runs it.** Only the owner, on their own Neo or M5, after a probe run. The lab has no H17 machine.
- **Out of scope.** The j775d `ane1` (die 1, `ane,t8020`, not iBoot-loaded) needs its own staged image and table.

## Data records

`data/ane-soc/t6050.json` `ane1_j775d`: the reason said that `adt-27.0.txt` lists `ane1` "no exclave". That summary prints no exclave property for any node (it shows none for `ane0`, which has `exclave-assigned` in the full ADT), so the claim had no source. The reason now says that the marking of `ane1` is not measured, and gives the `ane1` window, die and interrupts from `adt-27.0.txt:198-199`. No other fact changed. `t8140.json` and `t8142.json` are unchanged.

## Validation

- `python3 tools/validate_ane_soc.py data/ane-soc/t6050.json data/ane-soc/t8140.json data/ane-soc/t8142.json`: three `ok` lines, exit 0. `python3 tools/gen_coverage_table.py --check`: `gen_coverage_table: ok`, exit 0.
- Host gates on this branch: `make -C tools check` exit 0; `make -C tools ane-run`, then `pytest -q tests tools`: 53 passed, 1 skipped. `tools/test_ane_probe.py` runs the probe on a stock M1 tree, the T8103 and T6021 overlay trees, five Neo trees (stock aurora; the data-only overlay; that overlay with the ANE node enabled but its DARTs disabled; all nodes enabled; and then `exclave-assigned` added) and two M5 trees (T8142, and a two-die T6050 j775d). On the stock Neo tree the probe gives `not-exposed`, "the device tree has no ANE node".
- The notebook entry `entries/AneH17Driver2/20261003T012941Z-*-h17-b0-reachability-probe.md` records the commands, times and hashes.

## Limits

- The kext and firmware facts are NeoAne2's extractions (2026-10-02). This run did not re-read the kernelcaches or the images. The string log keeps 60 of the 525 exclave strings.
- `adt-27.0.txt` is a script summary, not the full ADT. It prints no exclave properties.
- Names show that a feature exists, not how it works. Equal CSNE names do not prove equal command semantics.
