# 2026-10-01: ANE coverage for every Apple SoC (static)

Static work only. No Mac was booted, and no module was loaded. The values come
from Apple's ADTs in the macOS 13.5 (22G74) and 27.0 (26A428) IPSWs, the
linux-asahi 7.1.13 device trees, and the overlays that ran on hardware.

## Result

| SoC | Support | ADT evidence (13.5 / 27.0 IPSW) | Overlay | Driver compatible | Gate |
| --- | --- | --- | --- | --- | --- |
| T8103 (M1) | tested | `ane` j274 j293 j313 j456 j457 | `t8103-ane.dts` (unchanged) | `ane.ko` `apple,t8103-ane` | enabled |
| T6000 (M1 Pro) | untested-overlay | `ane0` j314s j316s = T6001 `ane0` | `t6000-ane.dts` (new) | `ane.ko` `apple,t6000-ane` (existing entry) | opt-in `ane-t6000` |
| T6001 (M1 Max) | tested | `ane0` j314c j316c j375c (+ `ane1`) | `t6001-ane.dts` (same `.dtbo`) | `ane.ko` `apple,t6000-ane` | enabled |
| T6002 (M1 Ultra) | untested-overlay, die 0 | `ane0` j375d = T6001 `ane0`; `ane1..3` | `t6002-ane.dts` (new, die 0) | `ane.ko` `apple,t6000-ane` (existing entry) | opt-in `ane-t6002` |
| T8112 (M2) | unsupported | `ane` j413 j415 j473 j493 | none | none | — |
| T6020 (M2 Pro) | unsupported | `ane0` j414s j416s j474s = T6021 `ane0` | `t6020-ane.dts` (new, not installed) | none | state `disabled`, key `ane-t6020` |
| T6021 (M2 Max) | tested | `ane0` j414c j416c j475c | `t6021-ane.dts` (same `.dtbo`) | `ane_t6021` `apple,t6021-ane` | opt-in `ane-t6021` |
| T6022 (M2 Ultra) | unsupported | `ane0` j180d j475d = T6021 `ane0`; `ane1` | `t6022-ane.dts` (new, die 0, not installed) | none | state `disabled`, key `ane-t6022` |
| T8122, T6030, T6031, T6032, T6034 (M3) | unsupported | 27.0 only: `iop,ascwrap-v6` | none | none | — |
| T8132, T6040, T6041 (M4) | unsupported | 27.0 only: `ane,t8020` | none | none | — |
| T8142, T6050, T8140 (M5 and j700) | unsupported | 27.0 only: `ane,t8132exclave` | none | none | — |
| T8152 (j873g) | unsupported | 27.0 only: two `iop-ane,ascwrap-v8` | none | none | — |

No driver gets a new compatible. `ane.ko` already matches `apple,t6000-ane`,
the compatible that linux-asahi uses for the shared T600x die-0 blocks (the
ADT DARTs are `dart,t6000` on all three SoCs), so T6000 and T6002 need only
the opt-in overlays. `ane_t6021` gets no T6020 or T6022 entry (see "No data").

Every SoC has an ANE: each board of each SoC in the two IPSWs has an `ane*`
node in `/arm-io` and an ANE firmware in its BuildManifest.
The generation comes from Apple's firmware file names
([firmware-13.5.txt](firmware-13.5.txt), [firmware-27.0.txt](firmware-27.0.txt)):
`h13` T8103, `t600x` T6000/1/2, `h14` T8112, `t602x` T6020/1/2, `h15` T8122,
`t603x` T6030/1/2/4, `h16` T8132, `t604x` T6040/1, `h17` T8140, T8142 and
T6050, `h18` T8152. T6020, T6021 and the T6022 die 0 load the same file,
`t602x_ane0_fw_selene_rc4x` (the image `ane_t6021` pins).

## Sources of the new overlay values

T600x die 0 (`packaging/dt/t600x-ane.dtsi`, used by T6000, T6001, T6002):

| Value | Source |
| --- | --- |
| all nodes and properties | the T6001 overlay that bound `ane.ko` and ran exact fp16 programs (lab `ane/t6001-j316c-set-domains.dts`, packaged `t6001-ane.dts`); its `.dtbo` bytes do not change ([dtbo.txt](dtbo.txt)) |
| AIC `/soc/interrupt-controller@28e100000` | linux-asahi 7.1.13 `t600x-die0.dtsi:17`; in t6000-j314s, t6000-j316s, t6002-j375d |
| `ane_sys` `power-controller@268` | `t600x-pmgr.dtsi:374`; ADT pmgr `ANE_SYS` 0x28e080268 on j314s j316s j375d |
| `ane_sys_cpu` `power-controller@2c8` | `t600x-pmgr.dtsi:412`; ADT pmgr `ANE_SYS_CPU` 0x28e0802c8, parent `ANE_SYS` |
| SET states `c000..c030`, pmgr window | T6001 overlay; inside ADT `ane0` reg[1] 0x28e080000+0xc02c on all three SoCs |
| DARTs 0x285800000, 0x285810000, 0x285820000, IRQ 771 | ADT `dart-ane0` reg[0..2] and interrupt 771 on all three SoCs |
| engine 0x285c04000+0x24000 | ADT `ane0` reg[0] 0x284000000 + the H13 engine offset 0x1c04000 (proven on T8103: 0x26a000000 to 0x26bc04000, and on T6001) |
| ANE IRQ 770 | ADT `ane0` interrupt 770 on all three SoCs |
| `ane.ko` SET base 0x28e08c000 | `ane_drv.c` `ane_soc_t6000`; pmgr + 0xc000, inside ADT `ane0` reg[1] on all three |

T602x die 0 (`packaging/dt/t602x-ane.dtsi`, used by T6020, T6021, T6022):

| Value | Source |
| --- | --- |
| all nodes and properties | the T6021 overlay that `ane_t6021` ran on (stock kernel, three boots; disk boot); its `.dtbo` bytes do not change |
| engine 0x284000000+0x2000000, pmgr 0x28e080000+0x4034, SET 0x28e08c000+0x4000, IRQ 884 | ADT `ane0` reg[0..2] and interrupt 884, same on j414s j416s j474s j180d j475d as on j414c |
| DARTs 0x285800000, 0x285810000, 0x285820000, IRQ 885 | ADT `dart-ane0`, same on all T602x boards |
| power states 0x260, 0x2c8 (pmp), 0x2e0, 0x4000..0x4030 | `t602x-pmgr.dtsi:332, 451, 479, 568-622`; ADT pmgr `ANE_SYS`, `ANE_CPU`, `ANE_SYS_MPM`..`ANE_SET4` same addresses on all boards |
| AIC `/soc/interrupt-controller@28e100000` | `t602x-die0.dtsi:16`; in every t6020/t6021/t6022 board DTB |
| mailbox 0x285408000, send-empty line 1833 | T6021 lab overlay v3; no node in any T602x ADT names line 1833 (highest named: 1832) |
| T6020/T6022 compatible | `apple,t6020-ane`, `apple,t6022-ane`: one per SoC, so no driver binds them before it has their data |

T6002 and T6022 describe die 0 only. `ane.ko` takes the SET block from the
per-compatible descriptor (die-0 base), and `ane_t6021` compiles in die-0
addresses (IPI 0x285844000, pmgr words 0x28e084000, TD word 0x285c20458). A
die-1 node would use die-0 registers.

## No data (no overlay or no driver), and what is needed

- T8112: ADT has `ane` reg 0x26a000000+0x2000000, 0x23b700000+0x18000,
  0x23b724000+0x4000, IRQ 520, `dart-ane` 0x26b800000/0x26b810000/0x26b820000
  IRQ 521, pmgr `ANE_SYS` 0x23b7004a8. Needed: the ANE power states inside
  0x23b724000 (pmgr lists no ANE_CPU or SET states), the mailbox address and
  interrupts, the DART power domains, and a driver for `h14_ane_fw_bia_j4xx`
  (image pin, segment layout, where iBoot preloads it).
- T6020, T6022 die 0: `ane_t6021` maps the firmware where iBoot preloaded it
  on the T6021 j414c: SEG0 0x10000848000+0xc4000, SEG1 0x10001400000+0x438000
  (`ane_t6021_fwload.c`). The IPSW ADT has no `segment-ranges` on `ane0`:
  iBoot adds it at boot. Needed: `/arm-io/ane0` `segment-ranges` from a T6020
  and a T6022 on the 13.5 stub (m1n1 ADT dump, or `ioreg -l -p IODeviceTree`
  in macOS). With equal values, `ane_t6021` needs only the compatible; with
  other values, a per-SoC data struct. Then the overlay state goes to opt-in.
- T6022 die 1, T6002 die 1: a driver that takes its registers from the node.
- `ane1` of T6001/T6002 (0x508000000, IRQ 797) and `ane3`: in the ADT and
  in the BuildManifest (`Ap,ANE1`), not described; no driver data.
- M3 and later: a driver for each firmware generation and a compiler
  backend. The 27.0 ADT nodes are in [adt-27.0.txt](adt-27.0.txt).

## How

- `ipsw_ane.py fetch` reads only BuildManifest.plist and the DeviceTree
  members of each IPSW with HTTP range requests (the reader of
  `omarchy-ane-firmware-fetch`), and unwraps the LZFSE ADTs.
  `ipsw_ane.py firmware` and `adt` (m1n1 `proxyclient/m1n1/adt.py` at
  AsahiLinux/m1n1 a878095) wrote the `.txt` files here. A second fetch gave
  byte-identical ADTs and BuildManifests (25 files of 13.5, 61 of 27.0).
- `tools/asahi-dtbs` builds the linux-asahi board DTBs from tag
  asahi-7.1.13-3 (94fb2334) with the kernel's own dtc. All 19 that the
  overlays select, and all 41 Apple M-series DTBs, equal
  linux-asahi-7.1.13.asahi3-2 byte for byte ([dtbs.txt](dtbs.txt)).
- [tests.txt](tests.txt): `test_ane_dt` ok, `test_ane_m2` ok,
  `test_ane_overlays` ok: 8 overlays, 22 board applications, each with the
  skip (kernel node enabled) and re-apply (kernel nodes disabled) cases. Both
  negative controls fail as they must: dtc/fdtoverlay 1.6.1, and fdtoverlay
  1.7.2 that loads the system libfdt.so.1 1.6.1.
- [dtbo.txt](dtbo.txt): the T8103, T6001, T6021 and U-Boot `.dtbo` files are
  byte-identical before and after (dtc 1.7.2, and dtc 1.6.1).
- [ane-objects.txt](ane-objects.txt): the four `ane.ko` objects are
  byte-identical before and after (only a comment changed in `ane_drv.c`).
  `ane_t6021` is not changed.

## Install directory

The packaging lead confirmed the omacom/omarchy-mac#677 directory (head
5dd5cad6): overlays install to
`/usr/share/omarchy-platform/dtb-overlays/PREFIX/NAME.dtbo`; the opt-in file
stays `/etc/omarchy-platform/dtb-overlays.opt-in`. `OVERLAY_DIR` in
`omarchy-ane-dt` is the one place that names it (build-dtbo reads it,
omarchy-ane-m2-enable imports it, and `test_ane_dt.py` checks the hook
Target).

Decision: `omarchy-ane-dt apply`, `update-m1n1-dtbs` and both
`90-omarchy-ane-dt` hooks stay. The README supports Arch Linux ARM
(asahi-alarm) installs, which have no omarchy-mac-boot at all, so on them
`omarchy-ane-dt` is the only thing that applies the overlays. The lab M2 Max
is such an install: its disk boot used copies that `omarchy-ane-dt apply`
wrote ([t6021-disk-boot](../2026-10-01-t6021-disk-boot/README.md)), and
`apply` writes copies only where no omarchy-mac-boot is installed. Inference:
its overlays sit in `/usr/lib` from a hand `build-dtbo` run, because that
receipt names `build-dtbo` and no package. Not checked on the laptop.

The old directory is not read. `apply` refuses while `.dtbo` files are in
`/usr/lib/omarchy-platform/dtb-overlays`, so a tool update without a move
cannot drop the ANE node and the U-Boot input fix from the lab M2's next
`boot.bin`. [migration.txt](migration.txt) runs the move on a fake root
with the package j414c DTB and both T6021 keys: old main gives copy
`c31a54c3…`; the new tool refuses and keeps it; after `build-dtbo /`, removal
of the old directory, and `apply`, the copy is current, with the same bytes.
The check goes when no install has the old directory.
