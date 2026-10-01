# Changelog

## Unreleased

- Changed: the ANE is on by default on T8103, T6001 and T6021. The T6021 overlay is enabled (no `ane-t6021` key), the modprobe gate `install ane_t6021 /bin/false` is gone, and the new pacman hook `90-omarchy-ane-firmware.hook` runs `omarchy-ane-firmware-fetch --hook` on T6021 (a failed fetch is a note: the ANE stays off until a fetch succeeds and the Mac reboots). The untested T6000, T6002, T6020 and T6022 keep their opt-in keys, and the T6021 U-Boot input keeps `uboot-serial-stdin-t6021`; see receipts/2026-10-01-t6021-default-on/.
- Changed: `ane_t6021` with the default `fw_alias_reserved=1` refuses at probe, before any power access, unless no-map `/reserved-memory` nodes cover both iBoot firmware windows (0x10000848000+0xc4000, 0x10001400000+0x438000). T6021 default-on needs that reservation (the lab m1n1 adds it) or the own-memory mode (`fw_alias_reserved=0`, the default after its T6021 device test). With the packaged m1n1 1.6.1 the ANE stays off, with one log line, until the own-memory default lands.
- Added: `omarchy-ane-firmware-fetch --check` (reads only the installed file) and `--hook`; `omarchy-ane-check` checks the firmware on T6020, T6021 and T6022 with it.
- Removed: `omarchy-ane-m2-enable` and `packaging/modprobe/ane_t6021.conf`: the ANE is on by default on T8103, T6001 and T6021.
- Removed: the `af_bridge_macos` experiment parameter (#33); the 26 macOS AXI2AF bridge values did not change encoder time (254.274 vs 254.276 ms); see receipts/2026-10-01-t6021-af-bridge-run/.
- Added: `ane/t6021/probes/ane_dart_probe.c` reads the ANE DART tunable and PERF words (read only by default) and applies the bulk-DART tunables by group in the macOS order. Linux leaves the bulk DARTs at reset values; 0x220/0x224 and the SID words do not change the speed, and 0x20c breaks translation on a live DART; see receipts/2026-10-01-t6021-dart-tunables/.
- Added: T8112 (M2) ANE support in `ane_t6021`, UNTESTED: `apple,t8112-ane` runs the 13.5 `h14_ane_fw_bia_j4xx` image from driver memory with iBoot's patches replayed. `packaging/dt/t8112-ane.dts` (opt-in key `ane-t8112`) has the ANE, its three DARTs, its seven ANE power states (macOS 13.5 and 27.0 ADTs, 13.5 ANE kext), the mailbox (send-empty AIC 1000, a lab choice) and the eFuse window. The chip revision is read from the eFuse words that iBoot reads, and the ASC tunables are iBoot's ANE table (23 records); both come from the macOS 26.6.2 iBoot, which is not encrypted. See receipts/2026-10-01-t8112-ane/ and receipts/2026-10-01-t8112-optin/.
- Fixed: T6020 ran with the T6021 chip revision (0x11) and ASC tunables. It now uses revision 0x01 (community rows) and its own iBoot tunables, which differ from T6021 in 13 of 24 values. T6022 keeps the T6021 values; its iBoot has the same table. See receipts/2026-10-01-t8112-optin/.
- Tested, not merged: skipping the ten P-1 engine writes that macOS never makes (`p1_groups=0x1f`, branch `agent/m2-p1-groups`). The firmware boots and every output stays bit-exact; the encoder time does not change (254.147 vs 254.349 and 254.121 ms). The DART 0x300-0x310 words are ADT tunables whose values bound the IOMapper range; see receipts/2026-10-01-t6021-p1-groups/.
- Added: `tools/t8112-kit`, a read-only kit for one T8112 owner. `collect-macos.sh` saves the macOS ANE device-tree data (the entry IOVA). `collect-m1n1.py` reads the iBoot-patched firmware preload over the m1n1 proxy (chip revision, ASC tunables, entry IOVA) and reads engine registers only when every ANE power state is on. `ingest.py` checks a result against the driver's per-SoC data; a replay on the 17 T6021 captures matches the T6021 data. The T8112 firmware gives its pmgr page itself (0x23b70c000); see receipts/2026-10-01-t8112-kit/.
- Added: `omarchy-ane-firmware-fetch` installs the T8112 image `h14_ane_fw_bia_j4xx` (13.5 pin `af587dfa…`).
- Added: README "Promotion rule" and `tools/promotion_check.py`: an untested SoC goes on by default after 3 passing community rows (3 machines, 2 owners, 2 boards, 2 kernel releases, no failing row; each row: `omarchy-ane-check` ready, 20 bit-exact smoke calls, no ANE/DART/mailbox fault, 30 min uptime). Each ANE and DART address and interrupt and each power-domain path that the community rows carry agrees with the T6000, T6002, T6020 and T6022 overlays (no row carries the mailbox or the firmware placement). The rows show T6020 chip revision 0x01, not 0x11; see receipts/2026-10-01-community-rows/.

## 0.4.0 (2026-10-01)

The T6021 (M2 Max) driver stays opt-in. This release adds whole-model runs on
the M2, a first disk boot, ANE overlays for every SoC with cited values, and
the version string for `ane_t6021`.

### Added

- The whole Parakeet TDT 0.6B v3 encoder runs on the M2 ANE as one
  Apple-compiled H14 program and one CALL. The output is bit-exact with the
  golden macOS capture, and the decode gives the golden 104 tokens (#17).
- `tools/hwx_h14_staged_to_anec.py` converts a multi-port HWX to an ANEC (#17).
- The Qwen M2 conformance harness compares each of the 38 programs with the M1
  step dump. All 38 programs conform, 456 of 456 runs (#12, #14).
- `tools/qwen_m2_decode.py` runs a greedy staged-Qwen decode on the M2 (#15).
- `tools/qwen_precision.py` measures where the M2 decode diverges and
  pre-registers the gate for a native macOS reference run (#20).
- `ane_t6021.bo_total_max_mb` (default 12288) sets the cap on the BO bytes
  held at one time. `bo_total_bytes` shows the bytes held now (#13).
- `ane_t6021.trace_td` records a read-only per-CALL timeline in debugfs. The
  default is 0, and then the CALL path does not change (#18).
- `packaging/dt/t6021-uboot-serial-stdin.dts`: an opt-in overlay
  (`uboot-serial-stdin-t6021`) that lets one M2 Max boot from the internal disk
  (#16).
- ANE overlays for every SoC with cited values. T6000 (M1 Pro) and the T6002
  (M1 Ultra) die 0 get the T6001 nodes, behind the opt-in keys `ane-t6000` and
  `ane-t6002`; `ane.ko` binds them with the `apple,t6000-ane` data. T6020 (M2
  Pro) and the T6022 (M2 Ultra) die 0 get the T6021 nodes, but the package
  does not install them: no driver binds them. The macOS 13.5 ADTs give the
  same values on these SoCs. T8112 and M3 and later get no overlay (#28).
- `omarchy-ane-check` prints `UNTESTED SoC: SOC` on every SoC other than T8103,
  T6001 and T6021, and fails on a SoC that no driver supports (#28).
- `tools/test_ane_overlays.py` applies every overlay to every linux-asahi
  7.1.13 board device tree it selects. `tools/asahi-dtbs` builds those device
  trees, byte for byte as the Arch package has them, and a dtc 1.7.2 with
  libfdt linked in. The `dt-overlays` workflow runs them (#28).
- `tools/qwen_m2_decode.py --m-per-prompt` decodes each prompt at
  M = len(prompt) + 32, the attention size that the M1 reference used (#27).
- `tools/native-macos/` runs the staged Qwen programs and the Parakeet encoder
  on a Mac's own ANE compile under macOS. `tools/staged-qwen/dump_step_ports.py`
  and `check_step_dump.py` dump and check every port of a decode step (#29).
- Research only, not in the DKMS build:
  - `ane/h13/ane_t8103_fw.c` stages 1-3 and the H13 `sCSneCmdProgramLoad`
    packer `tools/h13_progload.py`, for a Linux-side firmware start on the M1
    (T8103).
  - `ane/t6021/probes/ane_afbridge_probe.c`, a read-only probe of the 26 ANE0
    AXI2AF bridge registers that macOS programs. It is built but has not run
    (#25).
  - Candidate U-Boot `mtpkbd` patches in
    `receipts/2026-10-01-t6021-disk-boot/uboot-mtp/patches/`, not applied
    (#19).
  - Receipts for the ANE device-tree nodes and bindings on
    aurora-silicon/linux #65 (#22, #26).

### Changed

- `ane_t6021` takes `ANE_VERSION` like `ane`. `dkms.conf` passes the package
  version, so `modinfo ane_t6021` and `/sys/module/ane_t6021/version` show it.
  Before, a DKMS build showed `unknown` or the commit of an enclosing repository.
- Port tables size every port from the HWX slot table (#10).
- `ane/t6021/gate/gate.sh` checks every island op with `tools/island_ref.py`
  (#13).
- `packaging/build-dtbo` installs `PREFIX-NAME.dts` as
  `PREFIX/omarchy-NAME.dtbo`, so one prefix can hold two overlays (#16).
- The overlays install to `/usr/share/omarchy-platform/dtb-overlays`, the
  directory of omacom/omarchy-mac#677. It was
  `/usr/lib/omarchy-platform/dtb-overlays`. A package upgrade moves the
  files. A hand install in the old directory (`packaging/build-dtbo /`) must
  move: while `.dtbo` files are in the old directory, `omarchy-ane-dt apply`
  refuses and keeps the current copies, and the README gives the three move
  commands. `OVERLAY_DIR` in `omarchy-ane-dt` is the one place that names the
  directory. `update-m1n1-dtbs` and the two `90-omarchy-ane-dt` hooks stay:
  Arch Linux ARM installs have no omarchy-mac-boot (#28).
- The T600x and T602x overlays share their nodes through `t600x-ane.dtsi` and
  `t602x-ane.dtsi`. The T8103, T6001 and T6021 `.dtbo` files do not change
  (#28).
- `omarchy-ane-dt` names the libfdt trap when fdtoverlay renumbers a phandle
  (#28).
- The README chip table gives each SoC its ANE firmware, support state,
  overlay gate, and the data that is missing (#28).

### Fixed

- libane: M2 send and read use the port model, not the ANEC header count.
  Before, `ane-run --ports` read only the first output, and 37 of the 38 Qwen
  programs failed on the M2 (#14).
- libane: the port-table build takes any task count from the header. Before,
  it refused programs with more than 128 tasks (#17).
- libane refuses a port-table ANEC whose `taskCount` is larger than its task
  stream (#17).
- `hwx_ports.py` and the converter read LC 0x40 tensor names longer than
  8 bytes (#17).
- `omarchy-ane-dt` skips each overlay on its own. Before, it skipped every
  overlay when the kernel tree had the ANE node (#16).
- `omarchy-ane-dt` counts only an enabled kernel node (no `status`, `"okay"`
  or `"ok"`) as the kernel's ANE node. Before, a disabled node, as in
  aurora-silicon/linux #65, made it skip the T6021 overlay, and the ANE stayed
  disabled. Now the overlay applies over the disabled nodes and enables them.
  The T6021 overlay sets `status = "okay"` on its three DARTs, so a merged
  DART does not stay disabled. `validate` refuses a new or changed node that
  names a disabled provider. `omarchy-ane-dt status` and
  `omarchy-ane-m2-enable` ignore disabled ANE nodes. See
  `receipts/2026-10-01-ane-dt-disabled-nodes` (#23).
- Removed the CoreSight PC-sampling path of the research module
  `ane_t8103_fw`, after it caused a hard reset of the M1.

### Known limits

- Disk boot is proven on one M2 Max laptop only, with the opt-in U-Boot
  serial-stdin overlay. That boot used a lab m1n1 stage 2 that adds the two
  `ane-firmware` reserved-memory nodes. The packaged m1n1 1.6.1 does not add
  them, so a disk boot with the packaged m1n1 and `ane_t6021` is not proven.
- The whole encoder takes 254 ms per CALL on the M2 under Linux and about
  139 ms on the M1 (T8103). On the same M2 under macOS, the same MIL compiled
  by macOS takes 89 ms with the same output bits (#29). The cause of the gap is
  not known.
- Qwen: all 38 programs conform, but STAGED-QWEN-REF passes 3 of 10 prompts
  against the M1 reference. The M2's own macOS compile and runtime give the
  same tokens as Linux on 10 of 10 prompts, with a logit difference of 0, so
  the 3 of 10 comes from H14 against H13 numerics, not from the driver (#29).
- Only T8103, T6001 and T6021 have run the ANE. The T6000 (M1 Pro) and T6002
  (M1 Ultra) overlays are untested: each applies only with its own opt-in key
  (`ane-t6000`, `ane-t6002`), and `omarchy-ane-check` prints
  `UNTESTED SoC: SOC` on them. The T6020 and T6022 overlays are not installed.
  The new overlays were checked against the linux-asahi 7.1.13 board device
  trees, not on hardware.
- `trace_td` is off by default. Use it for measurement only.
- The T8103 firmware start is research. The package does not contain it.
- Hardware gates of this release: T6021 passed the device gate with the
  release-built `ane_t6021` after a disk boot of one M2 Max, with the #23
  overlay (#30). T6001: both modules compile, and a smoke test passed on the
  loaded older `ane` module; the 0.4.0 module was not loaded. T8103: pending.
- The disabled-node kernel case that #23 fixes has not been booted.
- After the firmware starts, `ane_t6021` cannot unload. Only a reboot removes
  it. The driver stays opt-in through `omarchy-ane-m2-enable`.

## 0.3.0 (2026-09-30)

- T6021 (M2 Max) opt-in driver `ane_t6021` on the stock kernel: DRM ABI 2
  (UAPI `23b8eef`, driver `27e996a`) and the libane ABI-2 backend (`8a4379e`).
- Complete T6021 device-tree overlay with the send-empty IRQ. The poll-TX
  kernel patch is not necessary.
- `hello_wait_ms` defaults to 0. This stops a mailbox IRQ storm of 700,000 per
  second and the 95-152 ms add stalls (#8).
- A recycle pool for the io BOs that the firmware saw. Before, the driver
  stopped after about 14,500 processes per boot (#9).
- A CALL completes on the firmware finish event. Long programs no longer
  return zero output (#11).
- Port-table tools for Apple-compiled H14 programs (`ane-run --ports`).

## 0.2.0

- T6001 recovery drains the retained tm/tq state after an engine wedge.
- `ane_boost` holds the CPU clusters at the top frequency while the engine
  works (`boost_idle_ms`, default 100 ms).
- The staged Qwen tool has a 512-token prefill mode. The M1 Qwen ANE cell
  passes the parity bar.

## 0.1.0

- First release: the `ane` DRM accelerator module, `libane` and the Python
  bindings, for M1 (T8103) and M1 Max (T6001), driver ABI 1 (`7529715`).
- A three-tier SoC gate: qualified, recognized-untested and unsupported.
