# H16 (T8132 M4, T6040 M4 Pro, T6041 M4 Max) ANE: b0 table and b1 opt-in module

2026-10-03. Static work on an x86_64 host (omp-studio-local) plus
niced module builds in the macstudio ALARM chroot and against the
aurora tree. No Apple-silicon hardware ran anything in this receipt:
no boot, no module load, no MMIO. Sources are Apple IPSW members read
by HTTP range, extracted kernelcaches and iBoot payloads, and the
aurora `josh/ane-driver-aurora` tree.

## Verdict

- **b0 (per-SoC table): complete** for T8132, T6040 and T6041, every
  value cited below. The tier question is settled: **T6040 = Apple M4
  Pro, T6041 = Apple M4 Max** (MEASURED, `/product product-soc-name`).
- **b1 (opt-in module): built, not run.** `ane/h16/ane_h16.ko` builds
  W=1-clean against both required trees and refuses to probe without
  its opt-in key. It implements the firmware-boot smoke up to
  SCRATCH7-wake plus a polled RTKit HELLO/EPMAP/STARTEP. The CSNE
  commands (CONFIG_GET / PLATFORM_INFO / BUILDINFO) are NOT implemented:
  the 27.0 host contract for them is not derived (below). This is an
  experimental tool for the first M4 Linux owner, not ANE support.
- Driver family: a new module (`ane_h16`) with an `ane_t6021`-like
  contract. The 27.0 `AppleH16ANEInterface` ANE_Init is the same
  scratch + channel-manager path the T6021 team proved for the 13.5
  selene image, on an H16-specific register map.

## Sources

| Source | Identity |
| --- | --- |
| IPSW | macOS 27.0 (26A428) `UniversalMac_27.0_26A428_Restore.ipsw` (URL pinned by `packaging/omarchy-ane-firmware-fetch`) |
| BuildManifest | sha256 `22949db9…` (fetch.log) |
| ADTs | 12 board payloads; this receipt uses j604ap `fa30e1c7…`, j614sap `7f0ace88…`, j614cap `d9125210…` (full list in artifacts/AneH16Driver logs) |
| Kernelcaches | `kernelcache.release.mac16g` decompressed `803a5408…` (T8132 boards), `kernelcache.release.mac16j` decompressed `42544337…` (T6040/T6041 boards) |
| Kexts | `AppleH16ANEInterface` 10.19.2 (mac16g `99e16f1e…`, mac16j `976b4d88…`), `AppleT8132ANEHAL` `dc394980…`, `AppleT6041ANEHAL` `f5cfad15…` |
| Firmware | `h16_ane_fw_leto_j7x.im4p` (T8132 boards) payload `4b330c5f…`; `t604x_ane_fw_aether_brvx.im4p` (T6040/T6041 boards) payload `8659271a…`; both 1,556,480 B raw Mach-O, unencrypted (no KBAG) |
| iBoot | `iBoot.j604.RELEASE.im4p` payload `a53b4706…`, `iBoot.j614c.RELEASE.im4p` payload `538c53c4…`, `iBoot.j614s.RELEASE.im4p` payload `5d33fed4…` (mBoot-20457.1.29, LZFSE, unencrypted) |
| Linux | `~/src/omarchy-linux` at `josh/ane-driver-aurora` (f227145f50e4): `t8132.dtsi`, `t8132-pmgr.dtsi`, `drivers/soc/apple/rtkit.c`, `drivers/soc/apple/mailbox.c` |
| Tools | `ipsw` 3.1.724 (`kernel extract`, `kernel kexts -j`), `pyimg4`, capstone 5.0.7, `dtc`/`fdtoverlay`; scripts under `artifacts/AneH16Driver/h16-b0/scripts/` (private notebook) |

Chip map (BuildManifest `ApChipID` + ADT `/product product-soc-name`):

| SoC | Boards | Tier | Kernelcache | ANE firmware |
| --- | --- | --- | --- | --- |
| 0x8132 | j604 (Mac16,1), j623, j624, j713, j715, j773g | Apple M4 | mac16g | h16 leto |
| 0x6040 | j614s (Mac16,8), j616s (Mac16,7), j773s (Mac16,11) | **Apple M4 Pro** | mac16j | t604x aether |
| 0x6041 | j575c (Mac16,9), j614c (Mac16,6), j616c (Mac16,5) | **Apple M4 Max** | mac16j | t604x aether |

Tier discriminators, all MEASURED from the ADT payloads: T6040 boards
read `/product product-soc-name = "Apple M4 Pro"`, `gpu,t6040`, aic
`#main-cpus` 12, mcc `amcc-count` 4, no `apr1`; T6041 boards read
`"Apple M4 Max"`, `gpu,t6041`, `#main-cpus` 16, `amcc-count` 8, two
`apr` ProRes engines. The two ADTs' `ane0` and `dart-ane0` nodes are
identical apart from phandles (diff in artifacts logs); the kext has
one row (ane-type 0x110) and one HAL (`AppleT6041ANEHAL`) for both.

## b0 per-SoC table

"kext" = `AppleH16ANEInterface` 10.19.2 disassembly (mac16g), where
`ANEHWDeviceConfig::initializeANESoCConfig` at 0xfffffe0009616180
switches on the ADT `ane-type`. Calibration: its ane-type 0xa0 (T6021)
row reproduces the known T6021 map (SCRATCH0 0x1840048, CPU_CONTROL
0x1400044, doorbell 0x1844000, IRQ 0x184c000/0x1850000), so the field
decode is anchored on hardware-proven values. T8132 = ane-type 0x100
(type 0xc0, generation 7, "h16g", HAL AppleT8132ANEHAL); T6040/T6041 =
ane-type 0x110 (type 0xd0, generation 7, "h16g", HAL
AppleT6041ANEHAL, `AppleH16CamIn` IPC).

| Value | T8132 | T6040/T6041 | Source |
| --- | --- | --- | --- |
| Engine window | 0x500000000+0x2000000 | 0x484000000+0x2000000 | ADT ane reg[0] |
| pmgr window | 0x380700000+0x18000 | 0x502280000+0x18000 | ADT ane reg[1] |
| ANE ps words | ANE_SYS +0x570, MPM/CPU/TD/BASE +0xc000/8/10/18 | ANE_SYS +0x3c0, MPM/CPU/TD/BASE +0xc000/8/10/18 | ADT pmgr (t8132 also aurora t8132-pmgr.dtsi) |
| Power-gates | ANE-SYS-V, ANE-SYS-V-PMP (both no_ps) | + ANE-SYS-DART (no_ps, on dart-ane0) | ADT clock-gates/power-gates |
| CPU_CONTROL | engine+0x1600044 (0 then 0x10) | same | kext dev+0x4a0 |
| CPU_STATUS | engine+0x1600048 | same | kext dev+0x498 |
| RVBAR | engine+0x1050000 (written only if bit0 clear) | same | kext ANE_Init read64OneShot/write64 imm |
| SCRATCH0..7 | engine+0x1880020..+0x188003c | same | kext dev+0x438 const table |
| MBI doorbell | engine+0x1884000 | same | kext dev+0x468/0x498 |
| IRQ status/ack | engine+0x188c000 / +0x1890000 | same | kext dev+0x438+0x50 table |
| ASC core PA | 0x501000000 | 0x485000000 | iBoot coproc entry id 4, +0x20 |
| ASC wrapper PA | 0x501600000 | 0x485600000 | iBoot coproc entry id 4, +8 |
| ASC mailbox | wrapper+0x8000: a2i/i2a ctrl +0x110/0x114, a2i send +0x800/+0x808, i2a recv +0x830/+0x838 | same | iBoot coproc entry register table (ASC v4) |
| Interrupts | 629, 642 | 1054 | ADT ane interrupts |
| DART (LLT/BRD/BWR) | 0x501800000/0x501820000/0x501840000, each +0xc000 | 0x485800000/0x485820000/0x485840000 | ADT dart-ane reg + instance |
| DAPF | 0x501810000+0x4000 | 0x485810000+0x4000 | ADT dart-ane reg[3] |
| DART facts | dart,t8110; sid 0,10,11,15; bypass-10; sid-count 16; page 16 KiB; vm-base 0x10000000000; vm-size 0x30000000000; dart-options 0x25; flush-by-dva; IRQ 630/1055 | same | ADT dart-ane |
| MPM streams | iommu-parent mapper-ane (stream 0) + mapper-ane-mpm (stream 11) | same | ADT ane iommu-parent (new this pass: the MPM mapper) |
| Firmware | leto payload 4b330c5f… | aether payload 8659271a… | IPSW + BuildManifest `ANE` |
| Firmware load | iBoot: IsLoadedByiBoot true, IsFUDFirmware true, Personalize true | same | BuildManifest |
| Wake word | SCRATCH7 == 0x08042006 after CPU_CONTROL 0→0x10 | same | kext ANE_Init 27.0 |
| RTKit | 3514.0.15; `__TEXT` vm 0xc0000, `__DATA` vm 0xc0000+0x2ac000; `_rtk_patchbay` 43 records; `_rtk_tunables` 0x6a0 B | same | firmware payload |
| HW tunable tables | `ANEH16Donan/Brava/Coll_{PerfCtr,TLimit}` | `ANEH16Brava_*` (values not extracted) | kext symbols |

iBoot patchbay writer list (H16, from `iboot_ane.py` on j604/j614c;
sites in the artifacts logs): `BECA GKTS DILS SSSC qF8v ECAP BVTP
sP1T BtpG BlpP S2xG SNUT OTTR` plus the generic RTKit loader's
`SOC_ SOCR dApC dArW STKG McRA RTTO DEVf pFLG PRNG ECID NONC`.
`SNUT` (tunables pointer, firmware value 0xd7c70 = the section itself)
is written by iBoot but the tunable VALUES are filled at runtime — the
set tables are empty in the iBoot image (AneH17Driver's finding for
j700, checked here for j604/j614c), so the ASC tunable contents are not
derivable statically. The module therefore copies the patchbay and
tunables bytes from the iBoot preload instead of replaying values.

## b1 record

`ane/h16/`: `ane_h16_main.c`, `ane_h16_soc.c`, `ane_h16.h`, `Makefile`,
`t8132-ane-experimental.dts`, `README-bringup.md`. Behavior:

- No `MODULE_DEVICE_TABLE` (no autoload); probe returns -EPERM unless
  `optin=<soc>`; not in `make all`, `dkms.conf` or the package.
- `stage=status`: power-domains up, wait until ACTUAL (bits 7:4) == 0xf
  on all five ANE ps words and re-read all five, then log
  RVBAR/CPU_STATUS/SCRATCH/mbox controls. Correction 2026-10-03: the
  first merged version (afd5e62) only waited for ACTUAL nonzero
  (`v & 0xf0`); agent/ane-h16-ps-fix makes the code match this gate.
- `stage=boot`: pin the payload by SHA-256, parse the boot ADT from the
  reserved-memory phram `adt` region, read `segment-ranges`, diff the
  iBoot preload against the file (only patchbay/tunables and
  single-word iBoot patches accepted; anything else refuses), stage the
  image in a DART-mapped coherent buffer at the ADT IOVAs, RVBAR latch
  check or fold, SCRATCH7=0, CPU_CONTROL 0→0x10, poll SCRATCH7 for
  0x08042006, then a polled (IRQ-less) RTKit HELLO/EPMAP/STARTEP when
  `hello_wait_ms=1000`. No CSNE, no inference, no DRM node; remove
  refuses after the CPU is released.
- Builds: `josh/ane-driver-aurora` tree (f227145f50e4), W=1, rc=0,
  zero warnings, `ane_h16.ko` sha256 `086d2263…` vermagic
  7.1.12-ARCH+; M2 3-1 tree (macstudio ALARM chroot, niced), W=1,
  rc=0, `ane_h16.ko` sha256 `09f2337e…` vermagic 7.1.13-3-1-ARCH, no
  modinfo alias (no autoload). The overlay compiles with dtc 1.7.2 and
  applies with fdtoverlay to an aurora `t8132-j604.dtb` (phandles
  resolve; artifacts log `overlay-apply`).

### Missing facts (why CSNE is not implemented)

1. The 27.0 ANE_Init init-structure layout (the 0x174-byte header the
   13.5 kext published via SCRATCH0/1) is not decoded for 27.0.
2. The 27.0 channel-description table and ring layout (ChMan) is not
   decoded; the T6021 one is 13.5-specific.
3. CSNE opcode numbers for PLATFORM_INFO/BUILDINFO on 27.0 firmware
   are not extracted (CONFIG_GET 0x03 is, from the T6001 kext RE).
4. The ASC tunable block contents (iBootData-derived) are not
   derivable statically (above).
5. Whether iBoot preloads the ANE firmware for a Linux boot on M4 is
   unknown; the module refuses when the live ADT has no
   segment-ranges, and its preload diff refuses anything that is not
   the pinned image.

## Estimate to first boot (agent-days, plus tester runs)

b0 was the bulk and is done. Remaining to a promoted driver: decode
items 1-3 above (comparable to the T6021 W-series passes), then first
hardware runs. Honest range: 4-8 agent-days of static RE plus 2-6
tester runs to know whether the SCRATCH7 wake word arrives on M4; the
ChMan/CSNE stage after that is the same size again.

## Not verified

No H16 hardware ran. No module load, no binding, no MMIO, no firmware
start. The register map is kext- and iBoot-derived, calibrated against
the T6021 row, not measured. The tier mapping is from Apple's own
ADT strings, not from marketing pages. `ane_h16` is experimental until
an M4 owner produces the stage=status log.
