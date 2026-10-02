# 2026-10-01 — T602x: start the ANE firmware without iBoot-preload addresses

## Result

`ane_t6021` can now run the selene firmware from memory it allocates. It
needs no preload physical address and no reserved-memory node. The mode is
`fw_alias_reserved=0` on T6021 (default off) and the only mode on T6020 and
T6022, which the driver now binds (`apple,t6020-ane`, `apple,t6022-ane`,
UNTESTED opt-ins).

| Check | Result |
| --- | --- |
| Driver copy with iBoot's patches replayed vs 17 pre-Linux T6021 captures | byte-identical on all 17 (177 bytes differ before the replay, 0 after; guard taken from each capture) |
| Default path (`fw_alias_reserved=1`, T6021): trace of the whole `ane_t6021_fwload.c`, 547ae7c vs this branch | identical: 449 `iommu_map` (IOVA, PA, size, prot), allocation, log formats, staged-buffer hash |
| Own-memory path, whole `ane_t6021_fwload.c`, fake DMA/IOMMU | staged buffer = capture with the test guard; same 449 IOVAs as the default path |
| `tools/h14_boot_regression.c` | 112 checks pass (105 before); 130 with the 17 captures |
| `tools/test_t6021_alias_rollback.py`, `test_ane_firmware_fetch.py`, `test_ane_dt.py`, `test_ane_m2.py` | ok |
| `tools/test_ane_overlays.py` (19 linux-asahi 7.1.13 board DTBs) | ok, 8 overlays, 22 applications; T6020/T6021/T6022 `.dtbo` bytes unchanged |
| aarch64 module build against linux-asahi 7.1.13-3-2, W=1 | exit 0, no new warning; aliases `apple,t6020-ane`, `apple,t6021-ane`, `apple,t6022-ane` |
| Device run | Passed on the M2 Max: boots A (reserved) and B (own memory) of the plan below, and a boot with the packaged m1n1 1.6.1; see [2026-10-01-t6021-default-on-gate](../2026-10-01-t6021-default-on-gate/README.md). |

## What iBoot's preload is

- iBoot places the 13.5 (22G74) selene image (`a9c4b771…`) at its Mach-O vm
  layout: TEXT vm 0 + 0xc4000 at SEG0, DATA vm 0xc4000 + 0x438000 at SEGi
  (ADT `ane0` `segment-ranges`, remap IOVA 0x10000000000 and 0x100000c4000).
  It latches RVBAR at 0x10000000001.
- Then it patches the image. On 17 pre-Linux captures (2026-09-27) the
  preload differs from the archive only here:

| Field (Mach-O symbol) | vm | Archive | T6021 preload | Source in this change |
| --- | --- | --- | --- | --- |
| DATA base (TEXT u64) | 0x423c | 0 | 0x100000c4000 | latched entry + DATA vmaddr |
| `__rtk_patch__rtk_stack_guard` | 0xca848 | 0xaff | random, one zero byte | `get_random_u64()`, one byte cleared |
| `__rtk_patch_RTK_soc` | 0xca9b3 | 0xffffffff | 0x6021 | of_match data (0x6020/0x6021/0x6022) |
| `__rtk_patch_RTK_soc_revision` | 0xca9bf | 0xffffffff | 0x11 | T6021 capture constant |
| `__rtk_patch_RTK_cpu_physical_address` | 0xca9cb | 0 | 0x285000000 | DT engine reg + 0x1000000 |
| `__rtk_patch_RTK_cpu_wrapper_physical_address` | 0xca9db | 0 | 0x285400000 | DT engine reg + 0x1400000 |
| `__rtk_platform_asc_tunables_block` (first 0x1e8 bytes) | 0xdcd78 | header `01 03 24 00 ff ff ff ff`, zero | header + 24 {u32 offset, u64 mask, u64 value} | T6021 capture constant |

  Only the stack guard changes between boots. Each `__rtk_patch` record is
  {u32 tag, u32 length, value}; the replay checks every tag, length and the
  unfilled tunables header before it writes anything.
- So the lab did not use the reserved copy for a placement reason. The old
  `fw_alias_reserved=0` branch aliased the unpatched archive: `RTK_soc`
  0xffffffff, zero ASC addresses, DATA base 0. The reserved copy was the only
  image that carried iBoot's values. The marker contradiction of the
  2026-09-27 staged trials (bootstrap tables written while a patched entry
  marker stayed intact) is still open; the device test below is its
  discriminator for this design.

## Options

- A, own memory (chosen): no address and no reservation. The firmware
  only needs iBoot's values, and the table above gives a source for each.
- B, addresses from the live DT: stock m1n1 calls
  `dt_reserve_asc_firmware()` only for dcpext, sio and isp (`src/kboot.c`
  :1838, 2202, 2821 at v1.6.1 06a4601a; :1889, 2260, 2897 at main
  a878095); it never exports the ANE
  `segment-ranges`. Only the lab m1n1 adds the `ane-firmware` nodes. The
  IPSW ADT `ane0` has no `segment-ranges` (iBoot adds them at boot), so a
  T6020 or T6022 placement needs a capture from that machine. B keeps both
  dependences.
- C, addresses from the live DART: rejected. The ANE DART is off at handoff;
  iBoot leaves no page table to read.

What stays SoC-specific: `RTK_soc` (from the compatible), and two values with
no live source: the chip revision (m1n1 gives Linux no `chip-revision`; MIDR
0x611f0380 encodes 0x10, not 0x11) and the ASC tunables (iBoot's own table;
the IPSW ADT `ane0` has none). They are T6021 capture values. T6020 and T6022
run the same image on the same `ane0` (13.5 ADTs), so the driver uses them
there too. That is an assumption until a T6020 or T6022 runs it. No per-board
address remains.

## Change

- `ane/t6021/ane_fw_validate.h`: `ane_fw_apply_boot_patches()` and the
  tunables table, shared by the kernel and the host regression. It writes
  bytes one at a time, so the external consumers of this header need nothing new.
- `ane/t6021/ane_t6021_fwload.c`: `fw_alias_reserved=0` stages, replays the
  patches, then aliases the copy at the latched entry. `fw_alias_reserved=1`
  maps the T6021 SEG0/SEGi windows as before, only on a SoC with a recorded
  placement (`ane_t602x_soc.preload_placement`, T6021). `fw_extra_ram` no
  longer requires the reserved mode: both modes map the whole allocation.
- `ane/t6021/ane_t6021_rtclient_main.c`, `ane_t6021.h`: of_match data for
  `apple,t6020-ane` (0x6020), `apple,t6021-ane` (0x6021, placement recorded),
  `apple,t6022-ane` (0x6022, die 0).
- `packaging/dt`: `t6020-ane.dts` and `t6022-ane.dts` are opt-in
  (`ane-t6020`, `ane-t6022`), UNTESTED. Their nodes do not change. No overlay
  names a physical firmware address; the one reservation is the IOVA
  0x10000000000 (ADT `dart-ane0` `vm-base`).
- `packaging/omarchy-ane-firmware-fetch`: T6020 and T6022 fetch the same
  pinned image (the 13.5 BuildManifest gives `t602x_ane0_fw_selene_rc4x` to
  j414s, j416s, j474s, j414c, j416c, j475c, j180d and j475d).
  `packaging/omarchy-ane-check`: T6020/T6022 report UNTESTED and check
  `ane_t6021`.
- Removed `test/test_anet6021_fwload_options_ok.c`: it has not compiled since
  27e996a removed its header, and it pinned the extra-RAM coupling removed here.

## How

```
gcc -Wall -Wextra -O2 -I ane/t6021 -o h14_boot_regression tools/h14_boot_regression.c
./h14_boot_regression                                # 112 checks
./h14_boot_regression SELENE.macho CAPTURE/bytes...  # + archive layout + one check per capture
make -C <linux-asahi 7.1.13-3-2 build> M=$PWD/ane/t6021 ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- W=1 modules   # copy-out build
tools/asahi-dtbs OUT && ANE_DTBS=OUT/dtbs PATH=OUT/bin:$PATH python3 tools/test_ane_overlays.py
```

The trace comparison compiled each tree's whole `ane_t6021_fwload.c` in
userspace against fake firmware, DMA, IOMMU and MMIO (RVBAR 0x10000000001,
engine 0x284000000) and printed every call. Private notebook entry
`entries/T602xIndependent/20261001T220851Z-ct-t602x-no-preload-addresses.md`
keeps the script, the traces and the hashes.

## T6021 device test (for the lead to schedule)

Pre-registered in the notebook entry before any run. One laptop owner; no
other agent's experiment on the box.

1. Build this branch's module for the running kernel and install it. Record
   boot ID, kernel, module and firmware SHA-256.
2. Boot A, default parameters. Pass: dmesg has `fwalias: reserved SEG0/SEGi
   at entry 0x10000000000`; `gate.sh` passes for add, mul, relu, add-scalar,
   mul-scalar, real-div-scalar, clip-low, clip-high, matvec and the islands;
   the Parakeet encoder is bit-exact (fp16 sha256 `fca96f13…`) with a median
   of 254.5 ms ± 0.5 % over 20 calls. This proves the new module keeps the
   default path.
3. Boot B: `options ane_t6021 fw_alias_reserved=0` in
   `/etc/modprobe.d/ane_t6021-own-memory.conf`, then reboot. Pass: dmesg
   has `fwload: own memory: iBoot patches replayed (soc 0x6021 rev 0x11 DATA
   0x100000c4000 cpu 0x285000000 wrapper 0x285400000)` and `fwalias: entry
   0x10000000000 <- 448 dart pages aliased`, then the same BOOT-PHASE lines as
   A (pollA READY, pollB DONE, chman table VALIDATED, LEGACY CONFIG_GET
   result=0), then the same gate set, bit-exact encoder and timing as in A.
   No new DART, ANE or EXCH kernel line.
4. Fail (no READY, or any gate miss): the module stays pinned (it cannot
   unload). Remove the options file and reboot. That returns the default path
   unchanged. Record the dmesg and keep the boot for the notebook.
5. If B passes, the next test is a disk boot with the packaged m1n1 1.6.1
   and `fw_alias_reserved=0`, because only the reserved mode needs the lab
   m1n1. Changing the default is a separate decision after that.

T6020 and T6022: no machine is reachable. They stay UNTESTED opt-ins.
`omarchy-ane-m2-enable` stays T6021-only, so the opt-in is by hand: the
README "Chip coverage" row lists the steps.
