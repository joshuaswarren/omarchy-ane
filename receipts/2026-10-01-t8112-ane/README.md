# T8112 (M2) ANE: facts from Apple data, overlay, driver plan

2026-10-01. Static work on an x86_64 host. No Mac was used: no boot, no module
load, no MMIO. Sources are Apple hardware tables for macOS 13.5 and 27.0, the
linux-asahi 7.1.13 device tree sources and m1n1 a878095.

## Result

- The T8112 ANE is the same kind of ANE as the T6021 ANE: an ASC that runs
  RTKit firmware which macOS starts. In macOS 13.5, `ane-type` 0x80 (T8112) maps to internal type 0x70, generation 5.
  `ane-type` 0xa0 (T6021) maps to type 0x80, also generation 5. M1 is
  generation 4. `ANE_Init` has no type branch: RVBAR, CPU_CONTROL and the
  SCRATCH registers sit at the same engine offsets on both SoCs. The DART is
  `dart,t8110` with a `vm-base`, as on T6021. The firmware is RTKit
  (RTKit-2062.141.1, the same release as the T6021 selene image), and its
  segment layout matches selene's. **The driver family is `ane_t6021`, not `ane.ko`.**
- All register windows, interrupts, DARTs and the seven ANE power states
  come from Apple data (table below). They are in the new overlay
  `packaging/dt/t8112-ane.dts`. The overlay is not installed (state
  `disabled`, key `ane-t8112`), because no driver binds `apple,t8112-ane`.
  It applies to all four linux-asahi 7.1.13 T8112 board trees.
- `ane_t6021` now runs the T602x firmware from its own memory and replays
  iBoot's patches (receipts/2026-10-01-t602x-independent), so a T8112 entry
  needs no preload address. It still needs two iBoot values that Apple's
  IPSW data does not give (the chip revision and the ASC tunables block),
  the firmware entry IOVA, and a mailbox send-empty interrupt. The driver is
  unchanged (see "Driver plan").

## Sources

| Source | Identity |
| --- | --- |
| macOS 13.5 | build 22G74 |
| macOS 27.0 | build 26A428 |
| ADTs, 13.5 | `DeviceTree.j413ap.im4p` e6596bf8…, `j415ap` dfa5617f…, `j473ap` e9031a89…, `j493ap` eec90132… (LZFSE payloads aba575de…, 6fdc87af…, 921d6872…, 0007354f…) |
| ADTs, 27.0 | `j413ap` fb961bfd…, `j415ap` 998d630c…, `j473ap` 8d446d69…, `j493ap` 11664390… |
| ANE firmware, 13.5 | `Firmware/ane/h14_ane_fw_bia_j4xx.im4p`, 4938577 B, sha256 d0e0984c9542b671…; payload 4938552 B, **sha256 af587dfa96b1e01d2b2e0f9f776e5dbefebb44c7968b86fff7d2b19c4cbef0dd** |
| ANE firmware, 27.0 | same member, 1654809 B, sha256 365c53b3…; payload 1654784 B, sha256 5fd9330dff31812e… (another layout: `__DATA` 0x614000, chained fixups) |
| Control | the same extractor gives `t602x_ane0_fw_selene_rc4x` 13.5 payload a9c4b771…, the `ane_t6021` pin |
| Linux | AsahiLinux/linux tag `asahi-7.1.13-3` (94fb2334): `t8112.dtsi`, `t8112-pmgr.dtsi`. `tools/asahi-dtbs` builds `t8112-j413/j415/j473/j493.dtb` (8e6c9d4a…, 8afa377f…, a18edba7…, 5000f81c…), and these are byte-identical to the linux-asahi-7.1.13.asahi3-2 package DTBs |
| m1n1 | AsahiLinux/m1n1 a878095: `proxyclient/m1n1/adt.py` (ADT parser), `src/kboot.c`, `src/dapf.c`, `src/tunables_static.c` |

The four T8112 boards have the same ANE, DART and pmgr ANE data in both IPSWs.
Only phandles and the `function-mcc_dataset` index differ.

## Derived values

| Value | T8112 | Source | T6021 (comparison) |
| --- | --- | --- | --- |
| ANE node | `/arm-io/ane`, `compatible` `ane,t8020`, `ane-type` 128 | ADT | `/arm-io/ane0`, `ane-type` 160 |
| Engine window | 0x26a000000 + 0x2000000 | reference run behaviour | 0x284000000 + 0x2000000 |
| pmgr window | 0x23b700000 + 0x18000 | reference run behaviour | 0x28e080000 + 0x4034 |
| Third window | 0x23b724000 + 0x4000 | reference run behaviour | 0x28e08c000 + 0x4000 (PWGATE at +0x2dc) |
| ANE interrupt | AIC 520 | ADT `ane` interrupts | 884 (the mailbox recv-not-empty line, seen live) |
| Internal type | 0x70, generation 5 | macOS behaviour | 0x80, generation 5 |
| DART | `dart,t8110`, page 16 KiB; reg LLT 0x26b800000, BRD 0x26b810000, BWR 0x26b820000, DAPF 0x26b804000 (each 0x4000) | ADT `dart-ane` reg and `instance` (DART 0 LLT, DART 1 BRD, DART 2 BWR, DAPF 0 LLT) | same layout at 0x285800000 |
| DART interrupt | AIC 521 | ADT | 885 |
| DART stream | 0 | ADT `sid` = {0, 15}, `bypass-15`, `sid-count` 16 | same `sid` |
| DART IOVA range | `vm-base` 0x800000000, `vm-size` 0x7ffff0000, so `apple,dma-range = <0x8 0x0 0x7 0xffff0000>` | ADT; ADT `dart-dcp` has the same two values and `t8112.dtsi` writes them as this `apple,dma-range` (dcp_dart) | `vm-base` 1 TiB |
| DART power | gate `ANE-SYS-DART`, parent `ANE_SYS` | ADT `clock-gates`/`power-gates`, pmgr device parents | parent `ANE_SYS` |
| `ANE_SYS` | pmgr 0x23b7004a8, parent `MMX`; linux-asahi `ps_ane_sys: power-controller@4a8` | ADT pmgr device id 136 | 0x28e080260 |
| ANE power states | 0x23b70c000 `ANE_MPM`, 0x23b70c008 `ANE_SYS_CPU`, 0x23b70c010 `ANE_TD`, 0x23b70c018 `ANE_BASE`, 0x23b70c020/028/030/038 `ANE_SET1..4` | reference run behaviour | `ANE_CPU` 0x2e0, `ANE_SYS_MPM`/`TD`/`BASE`/`SET1..4` 0x4000..0x4030 |
| Power-on | PWGATE 0x23b7248b8 ← 0, wait for bits 29:28 = 0; 0x23b70c008 ← 0x1000000f; then 0xf into 0xc010, 0xc018, 0xc020, 0xc028, 0xc030, 0xc038, in that order; poll until each word reads ACTUAL = TARGET = 0xf | reference run behaviour | reg2+0x2dc &= ~0x30000000; 0x2e0 ← 0x1000000f; then 0x4008.. |
| Power-off | 0xc038, 0xc030, …, 0xc010, then 0xc008 ← 0x300; PWGATE ← 0x30000000, wait until set | reference run behaviour | 0x4030..0x4008, 0x2e0 ← 0; reg2+0x2dc \|= 0x30000000 |
| ASC registers | RVBAR 0x26b050000 (engine +0x1050000); CPU_CONTROL 0x26b400044 (+0x1400044, written 0 then 0x10); CPU_STATUS +0x1400048; SCRATCH0..7 +0x1840048..+0x1840064 | reference run behaviour | the same offsets from 0x284000000 |
| ANE IRQ status/ack | engine +0x184c000 read, +0x1850000 written | reference run behaviour | same |
| Mailbox | 0x26b408000 (ASC +0x8000) | the ASC base above plus the ASC mailbox v4 layout, as on T6021 (0x285408000, `ane/t6021/ane_t6021.h`). [INFERENCE: the same ASC wrapper; no ADT node names an ANE mailbox on either SoC] | 0x285408000 |
| DAPF ranges | 0x22b45c000+4, 0x23b70c000..0x23b70c03b, 0x23b7004a8+4; 27.0 adds 0x206468000+4 | ADT `dart-ane` `dapf-instance-0` | 0x28e084000..+0x33, 0x28e080260+4, 0x38545c000+4 |
| Firmware | `h14_ane_fw_bia_j4xx` | firmware list `ANE` of j413ap, j415ap, j473ap, j493ap (13.5 and 27.0) | `t602x_ane0_fw_selene_rc4x` |
| Firmware layout (13.5) | MH_PRELOAD, 5 load commands (0x960 bytes), flags 0x1, entry 0; `__TEXT` vm 0 size 0xb4000, file 0x4000; `__DATA` vm 0xb4000 size 0x438000, file 0xb8000, filesize 0x3e8000; `__text` 0x7627c bytes | payload header | `__TEXT` 0xc4000, `__DATA` 0x438000, same `__text` size |
| Firmware patch records | `_rtk_patchbay` records `GKTS` (0xba688), `_COS` RTK_soc (0xba7f3), `RCOS` RTK_soc_revision (0xba7ff), `dApC` RTK_cpu_physical_address (0xba80b), `dArW` RTK_cpu_wrapper_physical_address (0xba81b): all unset in the file. `__rtk_platform_asc_tunables_block` at 0xccbb8. The TEXT header has the selene layout: patchbay vm and size at 0x422c, DATA vm at 0x4234, the DATA-base u64 that boot stage fills at 0x423c | payload LC_SYMTAB `__rtk_patch_*`; header bytes 0x4200..0x4290 | the same records, +0x101c0 (0xca848…); tunables 0xdcd78; DATA base 0x423c |

Notes on the rows above (macOS behaviour on the 13.5 image; not re-checked on a running M2):

- `ane-type` 0x70 parts use the PWGATE register at +0x6dc of the third window, not +0x8b8.
- The PWGATE write and the `0xc008` ← 0xf step depend on bit 1 of the ANE power-management setting, whose default is 3. With bit 1 clear there is no PWGATE write, and `0xc008` is written with 0xf.
- The third window is mapped only for internal type 0x70 or higher with that bit set. PWGATE in that window is at +0x8b8.
- ADT pmgr device `ANE_CPU` (id 209) is 0x23b70c008, flag `no_ps`, parent `ANE_SYS`. pmgr `ps-regs[13]` is 0x23b70c000. The other ANE power islands have no ADT entry.

The 13.5 image is the T8112 analog of the pinned T6021 image: the same IPSW,
the same RTKit release, and the stub version that `omarchy-ane-firmware-fetch`
maps to an IPSW. The 27.0 image has another layout and would need its own pin.

## Missing data

| Item | Why the IPSW lacks it | How to get it | Needs a T8112? |
| --- | --- | --- | --- |
| iBoot patch values: `RCOS` (chip revision) and the ASC tunables block | iBoot takes them from its own tables (T6021: 0x11 and 24 entries, receipts/2026-10-01-t602x-independent) | one preload capture on a T8112 with `tools/t8112-kit/collect-m1n1.py` (m1n1 proxy) | yes |
| Firmware entry IOVA (RVBAR at handoff, engine +0x1050000) | boot stage latches it at boot; the IOVA is in the ane `segment-ranges`, which no platform device tree ADT has. Likely `vm-base` 0x800000000 [INFERENCE: T6021 latched its `vm-base` 0x10000000000] | the `segment-ranges` TEXT remap, which the reference run `ioreg` also shows (the collection script in `tools/t8112-kit`), or the m1n1 kit | yes |
| The pmgr page the firmware writes | answered from the image: bia `SetPMUBaseAddress` stores 0x23b70c010 (`ANE_TD`), so the page is 0x23b70c000 (receipts/2026-10-01-t8112-kit) | — | no |
| Mailbox send-empty interrupt | Apple data names none (T6021 uses unused AIC2 line 1833, a lab choice) | a lab choice of an unused AIC line, or a driver that does not need the mailbox (the 13.5 legacy transport never starts it) | no |

## Overlay

`packaging/dt/t8112-ane.dts`, state `disabled` in `packaging/dt/overlays`,
key `omarchy,opt-in = "ane-t8112"`, skip key `apple,t8112-ane`:

- the three DARTs (`apple,t8112-dart`, `apple,t8110-dart`), IRQ 521,
  `apple,dma-range`, power domain `ps_ane_sys`;
- the seven ANE power states 0xc008..0xc038 (`apple,t8112-pmgr-pwrstate`) as
  a chain `ane_sys_cpu` ← `ps_ane_sys`, `ane_td` ← `ane_sys_cpu`, `ane_base` ←
  `ane_td`, `ane_set1` ← `ane_base`, …, `ane_set4` ← `ane_set3`. The chain makes
  genpd use the macOS order in both directions. Apple data gives a parent
  only for 0xc008. `ANE_MPM` (0xc000) is left out, because the reference run never
  writes it;
- `ane@26a000000`: reg `engine`/`pmgr`/`set`, IRQ 520 `ane`, the three
  DARTs with stream 0, the seven power domains.

Left out because Apple data does not complete them: the mailbox node
(send-empty interrupt), `mboxes`, and the firmware `memory-region`.

Test (`tools/asahi-dtbs`, then `tools/test_ane_overlays.py` with the kernel
dtc 1.7.2-g53373d13), in [tests.txt](tests.txt):
`t8112-ane.dts (disabled): 4 boards: t8112-j413 t8112-j415 t8112-j473
t8112-j493`, and `ok (9 overlays, 26 applications)`. The applied j413 tree
resolves IRQs 520 and 521 to the AIC (phandle 0xf, `apple,t8112-aic`), the
DART power domain to `power-controller@4a8`, and the ANE power domains to
0xc008..0xc038 in order. `t8112-ane.dtbo` (kernel dtc, `-@`) sha256
c0edc029…; overlaid DTBs j413 3f4dc05a…, j415 aa2b4484…, j473 2a00fd5e…,
j493 6212879a….

## Driver plan

The driver is not changed: the chip revision, the ASC tunables and the entry
IOVA are not derived. With the own-memory path of
receipts/2026-10-01-t602x-independent, the physical placement is not
needed. To add T8112, `ane_t6021` needs a per-SoC data entry for
`apple,t8112-ane` with:

| Field | T8112 | T6021 now |
| --- | --- | --- |
| firmware file, pin, size | `apple/ane/h14_ane_fw_bia_j4xx.macho`, af587dfa…, 0x4b5b38 | selene, a9c4b771…, 0x4c5b28 |
| segments (`ane_fw_validate.h`) | `{0, 0xb4000, 0x4000, 0xb4000}`, `{0xb4000, 0x438000, 0xb8000, 0x3e8000}` | `0xc4000`, `0x438000` |
| patch-record and tunables VMs | the bia VMs in "Derived values" | selene VMs |
| `RTK_soc` | 0x8112 | 0x6021 |
| `RTK_cpu_physical_address`, `RTK_cpu_wrapper_physical_address` | 0x26b000000, 0x26b400000 (engine + 0x1000000 and + 0x1400000, the rule `ane_t6021` uses) | 0x285000000, 0x285400000 |
| `RTK_soc_revision`, ASC tunables | from a T8112 capture | T6021 capture constants |
| firmware entry IOVA | from a T8112 read (likely 0x800000000) | 0x10000000000 |
| pmgr page mapped for the firmware | 0x23b70c000 (bia stores PMU base 0x23b70c010, receipts/2026-10-01-t8112-kit) | 0x28e084000 |
| PWGATE | reg[2] +0x8b8: write 0 / 0x30000000 | reg[2] +0x2dc: RMW of bits 29:28 |

The engine offsets (RVBAR, CPU_CONTROL, SCRATCH, IRQ status and ack) need no
per-SoC field. `omarchy-ane-firmware-fetch` has the T8112 row (IPSW member
`Firmware/ane/h14_ane_fw_bia_j4xx.im4p`, the pin above). Estimate: about 150
to 250 lines across `ane_t6021_fwload.c`, `ane_fw_validate.h` and the
of_match table, plus host-test cases. A T8112 machine must supply the items
in "Missing data" before the first boot test; `tools/t8112-kit` collects
them and prints the diffs.

## Files

- [tests.txt](tests.txt): the test and DTB build output.
