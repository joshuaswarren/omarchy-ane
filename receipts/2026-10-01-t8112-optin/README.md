# 2026-10-01 — T8112 (M2) ANE as an UNTESTED opt-in on ane_t6021; per-SoC iBoot data

Static work on an x86_64 host. No Mac was used: no boot, no module load, no
MMIO. All values come from Apple files read by HTTP range, linux-asahi 7.1.13
device trees, m1n1 sources and community rows of the mlx-omarchy dataset.

## Result

- `ane_t6021` binds `apple,t8112-ane`. The overlay `t8112-ane.dts` is opt-in
  (`ane-t8112`), UNTESTED. No T8112 has run it.
- The two values that the T8112 receipt (2026-10-01-t8112-ane) could not get
  from the IPSW now have an Apple source, and no T8112 capture is needed:
  - **`RTK_soc_revision`**: iBoot reads it from two eFuse words at
    0x23d2c8060. The driver does the same read at load time.
  - **ASC tunables**: iBoot's own table for the ANE, 23 records, the same on
    j413, j415, j473 and j493.
- The same method gives two T602x corrections:
  - **T6020** has its own ASC tunables: 13 of the 24 values differ from
    T6021. With the measured revision 0x01 (community rows), this closes D1
    and D2 of receipts/2026-10-01-community-rows.
  - **T6022** has the T6021 tunables (j180d, j475d). Its revision 0x11 is
    measured (community row 90c6b9bf3e03).
- T6021 is unchanged. The staged image, the 449 IOMMU maps and the 17-capture
  replay are byte-identical to main (table below).

| Check | Result |
| --- | --- |
| `tools/h14_boot_regression.c` (gcc -Wall -Wextra) | 127 checks pass; with `--bia` on the bia archive 129; with the selene archive and the 17 pre-Linux T6021 captures 145 (each: 176 to 178 bytes differ unpatched, 0 after replay) |
| Same tool, new checks | T6020 block = the j414s iBoot table, key 0x00; T8112 block = the j413 iBoot table for revisions 0x00, 0x01, 0x10, 0x11, 0x21 (keys 0x00/0x10); 8 fuse vectors decode as the j413 iBoot instructions do; a revision with no key is refused with the image untouched |
| `fwload_trace.py` (T602xIndependent harness: the whole `ane_t6021_fwload.c` in userspace with fake firmware, DMA, IOMMU and MMIO), main vs this branch | T6021 default (reserved alias), T6021 own memory and T6022: the same 449 `iommu_map` calls and the same staged-buffer hash (e6668bfbdf6a46eb default, as in the T602xIndependent receipt). The only output change is one log format: `fwload: %s PRELOAD validated + DART-mapped: entry ...` (was `selene PRELOAD validated ...: 3 segs`). T6020: staged bytes change (revision and tunables), maps unchanged |
| Same harness, T8112 (bia archive, fake RVBAR 0x800000001, fake fuse 0x80000000/0) | 2 non-posted reads of the fuse window, patches replayed, 448 alias pages at 0x800000000, PMU page 0x23b70c000 mapped IOVA = PA, probe 0 |
| `tools/t8112-kit/ingest.py` on the 17 T6021 captures (`test_t8112_kit.py` with Apple data) | ok: each capture's tunables block, revision 0x11, PMU page and entry IOVA equal the driver's T6021 data |
| `tools/test_ane_overlays.py`, the linux-asahi 7.1.13 board DTBs | ok, 9 overlays, 26 applications; `t8112-ane.dts (opt-in)` on j413, j415, j473, j493. j413 result: mailbox interrupts 520 and 1000, `mboxes` = the mailbox phandle, reg-names `engine pmgr set fuse`, no `apple,dma-range`, no `memory-region` |
| `test_ane_firmware_fetch.py`, `test_t8112_kit.py`, `test_ane_dt.py`, `test_ane_m2.py` | ok |
| aarch64 module build, linux-asahi 7.1.13-3-2, W=1 | exit 0; the same 5 warnings as main; aliases `apple,t6020-ane`, `apple,t6021-ane`, `apple,t6022-ane`, `apple,t8112-ane` |
| Device run | NOT RUN. Test plan below |

## Sources

macOS 26.6.2 (25G83) IPSW,
`https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw`,
members `Firmware/all_flash/iBoot.<board>.RELEASE.im4p`. These are
iBootStage2 images with an LZFSE body and no KBAG: no decryption is
necessary (the method of receipts/2026-09-20-iboot-j414c). The 13.5 (22G74)
iBoot2 images (iBoot-8422.141.2, the version that the 13.5 stub runs) have a
KBAG and are encrypted, so they were not used.

| Board | SoC | im4p sha256 | payload sha256 |
| --- | --- | --- | --- |
| j413 | T8112 | 645599ac… | 2483e9e0… |
| j415 | T8112 | 943ae4ed… | bc183c80… |
| j473 | T8112 | 5212093b… | 067b8268… |
| j493 | T8112 | 0454dba4… | 471ba7e0… |
| j414c | T6021 | 4470fcac… (= receipts/2026-09-20-iboot-j414c) | 818990e1… |
| j416c | T6021 | 4d84001a… | c058695e… |
| j414s | T6020 | b464b754… | 5d2f7d1c… |
| j416s | T6020 | e8d0313e… | 39d42488… |
| j180d | T6022 | b4101a51… | ffb40860… |
| j475d | T6022 | eeb7a39c… | 1a88d3ed… |

Disassembly: capstone 5.0.7. Data pointers in these images are VA = base +
file offset, base 0x1006f0a0000 (T602x) and 0x900000000 (T8112).

## How iBoot fills the firmware

All addresses are payload offsets (j414c / j413).

- **RTK patch records.** The patcher writes `_COS` (RTK_soc) and `RCOS`
  (RTK_soc_revision) at 0x14ca5c..0x14ca94 / 0x139dd4..0x139e0c.
  - RTK_soc: T602x 0x469c0 = 0x6020 | (fuse 0x29e2a822c & 3). T8112 0x40268
    = 0x8112.
  - RTK_soc_revision: T602x 0x46c24 reads fuse 0x29e2cd16c/170. T8112
    0x40270 reads fuse 0x23d2c8060/064: bits 2:0 = w0[29:27], bits 6:4 =
    {w1[0], w0[31:30]} (`ubfx`, `extr`, `bfi`).
  So the revision is the chip's eFuse value, not a table constant. The kext
  `ANEMinorVersion` of the community rows (T6020 1, T6021/T6022 17) agrees.
- **Tunables block format.** A firmware has two blocks: type 1 at
  `__rtk_platform_asc_tunables_block`, and type 3 0x2d8 bytes after it. The
  loop at 0x118950 reads an 8-byte header: {type, flags (bit 0: 20-byte
  records), capacity 0x24, count, u32 key}. Type 1 selects set 0x17ad10 /
  0x168078. Type 3 selects set 0x17b0b8 / 0x168340.
- **Entry selection.** A set maps a coprocessor id to entries of 0x58 bytes,
  one entry per chip-revision key, highest key first. The selector (0x6c54 /
  the same code) takes the first entry whose key is not above the chip
  revision. The ANE id is 4: the field +0x210 of the ANE coprocessor
  descriptor (j414c 0x186218 `ANE0`, j413 0x16ff08 `ANE`). iBoot writes the
  selected key into the header and the records without flag bit 30.
- **Check on T6021.** j414c, key 0x10: type 1 gives one record (0x150010),
  and type 3 gives 23 records. Together, in this order, they equal the type-1
  block of the 13.5 T6021 captures byte for byte (header `01 03 24 18 10 00
  00 00`). The captures' type-3 block stays unset. So the 13.5 iBoot wrote
  all 24 records as type 1, and the 25G83 table values equal the 13.5
  values. On T8112 (and T6020) the 25G83 tables are type 1 only, the same
  shape as 13.5 T6021.

| SoC (boards) | Type-1 entry for ANE (id 4) | Type-3 entry | Driver table |
| --- | --- | --- | --- |
| T6021 (j414c, j416c) | 0x150010 only (j414c key 0x11 also has 0x150020) | 23 records, keys 0x11/0x10/0x01/0x00, all equal | `ane_t602x_asc_tunables`: 24 records, keys 0x10/0x01/0x00 (the captures show key 0x10 with revision 0x11, so 13.5 has no 0x11 key) |
| T6022 (j180d, j475d) | the same as j416c | the same as T6021 | `ane_t602x_asc_tunables` |
| T6020 (j414s, j416s) | 24 records, key 0x00 | none | `ane_t6020_asc_tunables`: 13 values differ from T6021 (0x140140..0x1401a8 pairs, 0x14a010) |
| T8112 (j413, j415, j473, j493) | 23 records, keys 0x10 and 0x00, equal | none | `ane_t8112_asc_tunables`: the T6021 offsets without 0x150010 |

## T8112 values (additions to the T8112 receipt)

| Value | T8112 | Source |
| --- | --- | --- |
| RTK_soc | 0x8112 | iBoot j413 0x40268 |
| RTK_soc_revision | eFuse 0x23d2c8060/064, read at load | iBoot j413 0x40270; driver `ane_t8112_fuse_revision()` |
| ASC tunables | 23 records; header key 0x10 for revision ≥ 0x10, else 0x00 | iBoot j413 0x1a09ac and 0x1a0b8c (j415, j473, j493 equal). [INFERENCE: the 13.5 iBoot2 has the same table and keys, as on T6021] |
| pmgr page for the firmware | 0x23b70c000, seven ps words 0xc008..0xc038 | the bia image stores 0x23b70c010 (ANE_TD) in `SetPMUBaseAddress` (receipts/2026-10-01-t8112-kit). It is no longer an inference |
| Probe guard | ANE_SYS_CPU ps word, pmgr window + 0xc008 | ADT pmgr `ANE_CPU` (id 209) |
| PWGATE | "set" window + 0x8b8 must read bits 29:28 = 0; the driver refuses otherwise | kext `EnableANEClocksAndPower`, type 0x70. The T6021 driver never writes its PWGATE either |
| Doorbell | engine + 0x1844000 (now engine-relative for every SoC) | kext `aneInterruptHandler` has no SoC branch |
| Mailbox | 0x26b408000, recv-not-empty 520, send-empty 1000 | ASC + 0x8000, as for every T8112 ASC in t8112.dtsi. **1000 is a lab choice**: no node in the 13.5/27.0 ADTs (286 lines, highest 1151) or the four linux-asahi DTBs (highest 1116) uses it, and it is in the free range 991..1023. Lines 517..519 are free too and may be the ANE mailbox's own lines [INFERENCE, not used] |
| DART range | no `apple,dma-range` | the driver uses a 32-bit DMA mask; with the ADT range (0x800000000..) as aperture no allocation can succeed. T602x has none either |
| Entry IOVA | read from RVBAR at load (0x800000000 expected) | [INFERENCE: ADT `dart-ane` vm-base; the iBoot ANE descriptor +0xc0 holds 0x800000000; the T8112 DCP DART logs the same range on community row 137d85c4c8d8] |

### The fuse read (a new read class for this driver)

- The DT window is the fourth `reg` of the ane node, `fuse` = 0x23d2c8060,
  8 bytes. It is inside ADT `pmgr` reg[36] (0x23d2c8000 + 0x4000), the eFuse
  block that linux-asahi exposes nowhere and no Linux node claims.
- It is safe for these reasons:
  - The block is always on.
  - m1n1 reads its T8112 fuses at 0x23d2c8484 (ATC, `kboot_atc.c`) and
    0x23d2c84b0/0x23d2c84dc (GPU, `kboot_gpu.c`) on every boot, before Linux.
  - iBoot reads the same two words for the same purpose.
  - Fuse reads are standard in Linux (`apple,efuses` nvmem cells on other
    Apple SoCs).
- The driver does two 32-bit reads through `ioremap_np` (non-posted) and
  unmaps at once. It never writes. Without an 8-byte `fuse` window it
  refuses the load: "chip revision unknown, refusing".

## Driver change

- `ane/t6021/ane_fw_validate.h`
  - `struct ane_fw_image` per pinned 13.5 image: `ane_fw_selene` and
    `ane_fw_bia` (name, sha256, size, segments, RTK record addresses,
    tunables address).
  - `struct ane_asc_tunables` per SoC (keys + records, replacing the byte
    array and `ANE_T602X_SOC_REVISION`).
  - `ane_t8112_fuse_revision()`.
  - `ane_fw_apply_boot_patches()` takes the image and the table. It selects
    the key as iBoot does and refuses a revision without one.
- `ane/t6021/ane_t6021.h`, `ane_t6021_fwload.c`
  - `struct ane_t602x_soc` gets `soc_revision`, `revision_fuse`, `fw`,
    `tunables`, `ps_cpu_off`, `pwgate_off`, `pmu_pa`, `ps_off` and
    `trace_td_off`.
  - The four per-SoC objects live in `ane_t6021_fwload.c`.
  - The fuse read happens in `ane_t6021_soc_revision()`.
- `ane/t6021/ane_t6021_rtclient_main.c`
  - The of_match entry for `apple,t8112-ane`.
  - The probe guard uses `ps_cpu_off`. The PWGATE check runs before any
    engine read.
  - The doorbell is engine + 0x1844000.
  - `trace_td` uses the per-SoC words. It records nothing on T8112, because
    the TM TD word is not known there.
- `packaging/dt/t8112-ane.dts` is opt-in. It adds the mailbox, the `fuse`
  window and `mboxes`, and drops `apple,dma-range`.
- The comments of `t6020-ane.dts` and `t6022-ane.dts` now give the measured
  revision and the iBoot tunables.
- `packaging/omarchy-ane-check`: t8112 reports UNTESTED and checks
  `ane_t6021`.
- `packaging/omarchy-ane-firmware-fetch`: docstring update.
- `tools/t8112-kit/ingest.py` compares a result with the driver's per-SoC
  data. It replaces the diff generator, because the driver now has T8112
  data. A capture is still the device check for the inferences above.
- `tools/h14_boot_regression.c`: T6020/T8112 cases, iBoot table oracles, the
  fuse decoder and `--bia`.
- CI runs the regression.

Unchanged, labeled as inference for T8112:

- P-1 writes 12 words with T8103 values into the engine page. On T6021,
  skipping ten of them changed nothing (receipts/2026-10-01-t6021-p1-groups).
- The FWIM size 0x500000 is the T6021 kext value. The bia vmsize 0x4ec000
  fits in it.

## T8112 device test (for the lead to schedule)

Pre-register it in the notebook. One owner.

1. Install the module of this branch and `omarchy-ane-firmware-fetch` (bia,
   af587dfa…). Add `ane-t8112` to `/etc/omarchy-platform/dtb-overlays.opt-in`,
   run `omarchy-ane-dt apply`, then reboot. Record the boot ID and the
   SHA-256 values of the module, the DTB and the firmware.
2. Pass:
   - dmesg has `ane_cpu ACTUAL` with the ON field and `PWGATE = 0x…` with
     bits 29:28 clear.
   - dmesg has `fwload: chip revision 0x… (fuse 0x23d2c8060: …)` and
     `iBoot patches replayed (soc 0x8112 rev 0x… DATA 0x8000b4000 …)`
     (if RVBAR is latched at 0x800000000).
   - dmesg has `fwalias: entry …` and `pmu: DART map 0x23b70c000 …: 0`.
   - The BOOT-PHASE lines (READY, DONE, chman VALIDATED, CONFIG_GET
     result=0) appear, and `gate.sh` passes for add and mul.
   - There is no new DART or ANE fault line.
3. Fail: the module refuses (PWGATE closed, no fuse window), or the
   firmware does not reach READY. Remove `ane-t8112` from the opt-in file,
   reboot, and keep the dmesg.
4. Cheaper first step: `tools/t8112-kit/collect-m1n1.py` on a 13.5 stub,
   then `ingest.py`. It checks the tunables block, the revision and the entry
   IOVA against this data before any module load.

## How

```
gcc -Wall -Wextra -O2 -I ane/t6021 -o h14_boot_regression tools/h14_boot_regression.c
./h14_boot_regression
./h14_boot_regression --bia h14_ane_fw_bia_j4xx.macho
./h14_boot_regression t602x_ane0_fw_selene_rc4x.macho CAPTURE/bytes...
python3 tools/test_t8112_kit.py        # ANE_KIT_* for the Apple-data parts
tools/asahi-dtbs OUT && ANE_DTBS=OUT/dtbs PATH=OUT/bin:$PATH python3 tools/test_ane_overlays.py
make -C <linux-asahi 7.1.13-3-2 build> M=$PWD/ane/t6021 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 modules
```

The iBoot reads (table walker, selector, descriptor dump), the trace
harness variants and their outputs are in the private notebook entry
`entries/T8112Ship/20261001T225831Z-ct-t8112-optin.md`.
