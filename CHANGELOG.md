# Changelog

## 0.4.0 (2026-10-01)

The ANE is on by default on M1 (T8103), M1 Max (T6001) and M2 Max (T6021).
On T6021 the driver starts the ANE firmware from its own memory and replays
iBoot's runtime patches, so it needs no reserved-memory node from m1n1. This
is verified with the packaged m1n1 1.6.1 on one M2 Max laptop. The U-Boot
serial-stdin overlay stays an opt-in. This release also runs whole
models on the M2, adds ANE overlays with cited values for every M1 and M2 SoC
(the untested ones behind opt-in keys), and gives `ane_t6021` the package
version.

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
- ANE overlays for every M1 and M2 SoC with cited values. T6000 (M1 Pro) and
  the T6002 (M1 Ultra) die 0 get the T6001 nodes, behind the opt-in keys
  `ane-t6000` and `ane-t6002`; `ane.ko` binds them with the `apple,t6000-ane`
  data. T6020 (M2 Pro) and the T6022 (M2 Ultra) die 0 get the T6021 nodes,
  behind `ane-t6020` and `ane-t6022`. The macOS 13.5 ADTs give the same values
  on these SoCs (#28). M3 and later get no overlay.
- `ane_t6021` binds `apple,t6020-ane` and `apple,t6022-ane` (`32b916c`) and
  `apple,t8112-ane` (`e91d350`, #47) as UNTESTED opt-ins. T8112 (M2) runs the
  13.5 `h14_ane_fw_bia_j4xx` image. `packaging/dt/t8112-ane.dts` (key
  `ane-t8112`) has the ANE, its three DARTs, its seven power states, the
  mailbox and the eFuse window; the chip revision and the ASC tunables come
  from the macOS 26.6.2 iBoot (#39, #47).
- `omarchy-ane-firmware-fetch` installs the T8112 image (pin `af587dfa…`), and
  has `--check` (reads only the installed file) and `--hook`.
  `omarchy-ane-check` checks the firmware on the M2 family with it (#43, #44).
- The pacman hook `90-omarchy-ane-firmware.hook` fetches the firmware at
  install and upgrade on T6021. Without network it prints a note, and the ANE
  stays off until a fetch succeeds and the Mac reboots (#44).
- `tools/t8112-kit`: a read-only kit that collects the T8112 iBoot ANE values
  under macOS and over the m1n1 proxy. A replay on 17 T6021 captures matches
  the T6021 data (#43).
- README "Promotion" and `tools/promotion_check.py`: an untested SoC goes on
  by default after 3 passing community rows from 3 machines, 2 owners,
  2 boards and 2 kernel releases, with no failing row. A row's smoke golden is
  the add fixture's, so T8112 rows can pass; the Parakeet encoder hash stays a
  developer check. The regression rule: a chip that
  is on by default goes back to opt-in when its latest row shows
  `omarchy-ane-check` not ready or a fault line, until a clean row lands
  (`promotion_check.py` prints `REVERT`). The community rows agree with every
  ANE and DART address, interrupt and power-domain path of the T6000, T6002,
  T6020 and T6022 overlays that they carry (#45, #48, #50).
- `omarchy-ane-smoke` runs the shipped H14 add program
  (`fixtures/h14-anec/add`) 20 times through `omarchy-ane-run`, one process
  per call, and prints one JSON line for the community collector. Exit 0: 20
  calls bit-exact against the exact fp16 sum of fixed inputs. Exit 1: a call
  failed, was not exact, or ran past 60 s. Exit 2: no smoke on this Mac. The
  M1 family gets exit 2, because no H13 add program has run through `ane-run`
  on a device. `omarchy-ane-check --smoke` runs it after the checks (#50).
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
  - `ane/t6021/probes/ane_afbridge_probe.c` reads the 26 ANE0 AXI2AF bridge
    registers that macOS programs. Under Linux 0 of the 26 hold the macOS
    values (`17e3bdc`; #25, #33).
  - `ane/t6021/probes/ane_dart_probe.c` reads the ANE DART tunable and PERF
    words and can apply the bulk-DART tunables by group in the macOS order
    (`3ec8a28`, `61637ff`; #36, #37).
  - A prepared apple-dart kernel patch that writes the bulk-DART tunables at
    DART reset, and its boot record (#38, #41, #42, #46).
  - Candidate U-Boot `mtpkbd` patches in
    `receipts/2026-10-01-t6021-disk-boot/uboot-mtp/patches/`, not applied
    (#19).
  - Receipts for the ANE device-tree nodes and bindings on
    aurora-silicon/linux #65 (#22, #26).

### Changed

- The ANE is on by default on T6021: its overlay is enabled (no `ane-t6021`
  key), and the modprobe gate `install ane_t6021 /bin/false` is gone. The
  untested SoCs keep their opt-in keys (#44).
- `ane_t6021` runs the T6021 firmware from its own memory with iBoot's runtime
  patches replayed (`fw_alias_reserved=0`, the default). The reserved mode
  (`fw_alias_reserved=1`) needs the lab m1n1, and `ane_t6021` refuses it at
  probe, before any power access, unless no-map `/reserved-memory` nodes cover
  both iBoot firmware windows (`32b916c`, #44, #49).
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
  refuses, keeps the current copies, and names the steps. `OVERLAY_DIR` in
  `omarchy-ane-dt` is the one place that names the directory.
  `update-m1n1-dtbs` and the two `90-omarchy-ane-dt` hooks stay: Arch Linux
  ARM installs have no omarchy-mac-boot (#28).
- The T600x and T602x overlays share their nodes through `t600x-ane.dtsi` and
  `t602x-ane.dtsi`. The T8103, T6001 and T6021 `.dtbo` files do not change
  (#28).
- `omarchy-ane-dt` names the libfdt trap when fdtoverlay renumbers a phandle
  (#28).
- The README chip table gives each SoC its ANE firmware, support state,
  overlay gate, and the data that is missing (#28).
- Removed: `omarchy-ane-m2-enable` and `packaging/modprobe/ane_t6021.conf`
  (#44).
- Removed: the `af_bridge_macos` experiment parameter. The 26 macOS AXI2AF
  bridge values did not change the encoder time (254.274 vs 254.276 ms) (#33,
  #34).

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
  names a disabled provider, and `omarchy-ane-dt status` ignores disabled ANE
  nodes. See `receipts/2026-10-01-ane-dt-disabled-nodes` (#23).
- Removed the CoreSight PC-sampling path of the research module
  `ane_t8103_fw`, after it caused a hard reset of the M1.
- T6020 uses chip revision 0x01 and its own iBoot ASC tunables. Before, it ran
  with the T6021 revision (0x11) and tunables, which differ in 13 of 24 values.
  T6022 keeps the T6021 values (`e91d350`; #45, #47).

### Known limits

- Each on-by-default chip is tested on one machine: one M1 (T8103), one
  M1 Max (T6001) and one M2 Max (T6021).
- T6021 disk boot: verified with the packaged m1n1 1.6.1 on one M2 Max laptop
  (with the opt-in U-Boot stdin overlay for that laptop's phantom keyboard
  input). The own-memory default needs no reserved-memory node.
- On Omarchy, the overlays apply only through an omarchy-mac-boot with device
  tree overlay support (omacom/omarchy-mac#677, not merged). With an older
  omarchy-mac-boot, `omarchy-ane-dt` refuses, and the ANE node must come from
  the kernel.
- T6000, T6002, T6020, T6022 and T8112 are untested opt-ins. Each applies only
  with its own key, and `omarchy-ane-check` prints `UNTESTED SoC: SOC` on it.
  On T6020, T6022 and T8112, run `omarchy-ane-firmware-fetch` by hand. Their
  overlays were checked against the linux-asahi 7.1.13 board device trees, not
  on hardware.
- Speed: the whole Parakeet encoder takes 254 ms per CALL on the M2 under
  Linux and about 139 ms on the M1 (T8103). On the same M2 under macOS, the
  same MIL takes 89 ms with the same output bits (#29). The cause is under
  investigation. Rejected causes, each with a receipt commit: the compiled
  program (the M2's native program also takes 253 ms under Linux; `3b6fcf4`,
  #32), the AXI2AF bridge tunables (`17e3bdc`, #33), the bulk-DART tunables
  0x220, 0x224 and the SID words (`ec6ab5a`, #37), and the ten Linux-only P-1
  writes (`ee469e2`, #40). Writing DART 0x20c on a live DART breaks
  translation (`ec6ab5a`); a write at DART reset is still open (`a332e6a`,
  `d5a0e74`).
- Qwen: all 38 programs conform, but STAGED-QWEN-REF passes 3 of 10 prompts
  against the M1 reference. The M2's own macOS compile and runtime give the
  same tokens as Linux on 10 of 10 prompts, with a logit difference of 0, so
  the 3 of 10 comes from H14 against H13 numerics, not from the driver (#29).
- `trace_td` is off by default. Use it for measurement only.
- The T8103 firmware start is research. The package does not contain it.
- The disabled-node kernel case that #23 fixes has not been booted.
- After the firmware starts, `ane_t6021` cannot unload. Only a reboot removes
  it.
- Hardware gates of this release: T8103 passed on 2298beb (package build,
  install, load, bind, `omarchy-ane-check`, ABI probe; the encoder smoke did
  not run), and `ane.ko` did not change after 2298beb. T6001 passed on
  be6b352: `ane.ko` is byte-identical to the earlier builds, the smoke tests
  pass, and `ane_t6021` cannot autoload on T6001. T6021 passed two disk boots
  on 73da8f8 with the lab m1n1, in the reserved mode and in the own-memory
  mode (#49): the device gate passed, Qwen program 20 matched the M1 golden
  (rel L2 0.00117), and the whole encoder was bit-exact at a 254.5 ms median.
  A third disk boot ran b6ef8f1 with the packaged m1n1 1.6.1 and the default
  own-memory mode: 16 gates passed, and the whole encoder was bit-exact at a
  254.3 ms median, with outputs byte-identical to the own-memory lab boot.

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
