# Changelog

## Unreleased

## 0.4.2 (2026-10-03)

`ane` powers the ANE off when it is idle and maps each buffer object with one
DART TLB sync, both drivers log their register tracing at debug level, and
DKMS steps aside on a kernel that ships the ANE driver itself. This release
also adds a reachability verdict to `omarchy-ane-probe`, in-tree rows to the
promotion checker, a lab `dyn_pg` parameter to `ane_t6021`, and data and an
experimental bring-up module for chips after the M2. The ioctl interface
(ABI 1 and ABI 2) does not change: from `4f01bb3` (v0.4.1) to `dc174cd`, no
line of `ane/src/uapi/drm/ane_accel.h`, `libane/ane.h` or `libane/ane_m2.h`
changes.

### Added

- `omarchy-ane-probe` has a `reachability` section (`schema_version` 2)
  (#102). From `/proc/device-tree` and `/sys` only, it tells whether this
  machine's device tree gives the ANE to the OS: `reachable`,
  `owned-elsewhere` (the node has the ADT property `exclave-assigned`),
  `not-exposed` (no ANE node, a disabled node, or no `reg`, `iommus` or
  `power-domains`) or `unknown`, with the reason and, for each ANE node, the
  status, compatible, windows, IOMMU, power-domain and mailbox targets,
  `exclave*` properties and the bound driver. It also gives the `exclave`
  record of `data/ane-soc/<soc>.json` and the `adt` phram region. It reads
  no register. One run on a MacBook Neo or an M5 Mac answers the question.
- libane prints per-stage load times and per-`ane_exec` times on stderr when
  `ANE_TRACE_TIMING` is set (not empty, not `0`): one line
  `LIBANE: TIMING stage=NAME ms=MS bytes=N` per stage (#101). Unset, each
  stage costs one branch.
- `tools/ane_cold_start.py` measures the per-process program open of the
  resident worker (seal, fork, `dlopen`, `ane_init`) without a submit, under
  the ANE lock and the idle rule. `docs/ane-worker.md` designs a persistent
  worker (#101). See `receipts/2026-10-03-ane-cold-start/README.md`.
- `omarchy-ane-check` prints `dtbs_source` and `driver_source`, the fields
  the community collector records (#97). When `DTBS=` is set in
  `/etc/default/update-m1n1` (`dtbs_source=kernel`), it names no opt-in key:
  overlay opt-in has no effect there, and `omarchy-ane-dt apply` refuses.
- The promotion checker judges in-tree rows (`driver_source=intree`) like
  DKMS rows (#97). A PROMOTE with a passing in-tree row also has the target
  `aurora-dt`: `tools/aurora_dt.py` and `promote_from_verdict.py aurora-plan`
  and `aurora-pr` make the aurora-silicon/linux device-tree PR. The job that
  builds the aurora tree holds no credential; only the PR step, which builds
  nothing, gets the secret `AURORA_PR_TOKEN` (a dry run without it).
- `ane_t6021` has a lab parameter `dyn_pg` (default 0, T602x only) (#96).
  With `dyn_pg=1`, probe sends the selene firmware command
  `SET_DYNAMIC_POWERGATE` = 1 after CONFIG_GET, so that the firmware can turn
  the compute islands off between jobs. With `dyn_pg=1`, `trace_td` reads no
  TD word. The debugfs file `ane_t6021/ane_pg_state` prints the seven ANE
  power-state words. Measured on one M2 Max laptop: the firmware answers the
  command with status 0, but `ane_pg_state` shows the islands fully on
  (ACTUAL 0xf) before the first job, 30 s after the last one, and every 10 s
  through the idle blocks, so it does not gate them. Smoke, gates, encoder
  time and output bits are the same as with the defaults. With
  `dyn_pg=1 boot_prevent_nap=0`, the firmware boots but does not answer
  CONFIG_GET (-110), and `ane_t6021` registers no device. See
  `receipts/2026-10-03-t6021-dynpg/README.md`.
- PLACEHOLDER (#106, not merged; remove this line if #106 does not merge
  before the tag): the measured run is in
  `receipts/2026-10-03-t6021-v042-gate/README.md`.
- `ane/h16`: an opt-in experimental bring-up module, `ane_h16`, for the M4
  family (T8132, T6040, T6041) (#100, #103). It is not part of the package
  or the DKMS build, nothing autoloads it, and it refuses to probe without
  `optin=SOC`. `stage=status` powers the ANE domains, waits until every pmgr
  ANE state word reads ACTUAL 0xf, and logs the CPU and mailbox registers.
  `stage=boot` also validates the iBoot-preloaded firmware against the
  pinned image, maps it at the ADT addresses, releases the ASC CPU and
  answers the RTKit start. It registers no DRM device and sends no ANE
  command. No silicon has run it. `data/ane-soc/t8132.json`, `t6040.json`
  and `t6041.json` carry the H16 values from the macOS 27.0 images, with
  T6040 as the M4 Pro and T6041 as the M4 Max. See
  `receipts/2026-10-03-ane-h16/README.md` and `ane/h16/README-bringup.md`.

### Changed

- `ane` powers the ANE off when it is idle (#95). Before, probe kept a
  runtime-PM reference until the driver unbound, so the ANE power domains
  (`ane_sys`, `ane_sys_cpu`, `ane_base`, `ane_set*`) and its three DARTs
  stayed on all the time. Now the device suspends `autosuspend_ms` after the
  last open, ioctl or close (module parameter, default 1500), and the next
  one powers it up again. `autosuspend_ms=0` keeps the old behavior. At run
  time, `power/autosuspend_delay_ms` on the ANE platform device changes the
  delay, and `echo on > power/control` keeps the ANE powered. A write to the
  `reset` attribute and a file close now hold the device powered while they
  run. 32-bit (compat) ioctls go through the same command filter and power
  reference as native ones. `ane_t6021` does not change. Measured on one M1
  laptop (T8103) and one M1 Max laptop (T6001), with the `ane.ko` sources of
  this release: with the ANE suspended, idle system power equals the level
  with the module removed (T8103 within 45 mW, T6001 within 7 mW). With the
  ANE held on, T8103 used 156 mW more (mean of the block means, 3469 against
  3313 mW), and T6001 used 14.91 and 20.45 W against 12.80 and 12.81 W. The
  first open after idle takes about 0.33 ms (T8103) and 0.35 ms (T6001)
  more. Whole-encoder outputs stayed bit-exact, smoke runs from the
  suspended state passed 20 of 20, and about 300 suspend and wake cycles
  per machine gave no error. See
  `receipts/2026-10-03-ane-autosuspend/README.md`.
- DKMS skips a kernel that ships the ANE driver itself
  (`CONFIG_DRM_ACCEL_ANE=y` or `=m`, aurora-silicon/linux #155): that kernel
  keeps its own `ane.ko` and `ane_t6021.ko` (#94). `dkms.conf` sets
  `BUILD_EXCLUSIVE_CONFIG="!CONFIG_DRM_ACCEL_ANE"`, so `dkms build` exits 77
  (excluded) there. Before, `ane_t6021` failed to build on such a kernel:
  its `include/uapi/drm/ane_accel.h` hides ours and lacks
  `ANE_ABI_M2_MAJOR`. `tools/test_dkms_exclusive.py` runs dkms on both kinds
  of kernel.
- libane fills the program staging buffer with one `pread` and zeroes only
  the tail past a short read, instead of a `memset` over the whole buffer
  first (#101). The buffer stays 16 KiB aligned and zero past the file end.
- `ane` maps a buffer object into the ANE DARTs with one `iommu_map_sg`
  call, so apple-dart invalidates the TLB of each DART once per BO_INIT
  (#104). Before, BO_INIT called `iommu_map` once per 16 KiB page, and each
  call invalidated the TLB of all three DARTs: 83,865 invalidates for the
  458 MB Parakeet encoder program. A stray PTE in the new range is still
  cleared once and the map retried. Module parameter `map_batch` (0644,
  default 1); `echo 0 > /sys/module/ane/parameters/map_batch` selects the
  old per-page path for the next BO_INIT, without a reload. The unmap path
  does not change. Measured with the whole Parakeet encoder, 10 cold opens
  per arm: on one M1 laptop (T8103), BO_INIT took 22.77 ms instead of
  179.85 ms (median), and the whole open took 165 to 173 ms less; on one M1
  Max laptop (T6001), BO_INIT took 24.75 ms instead of 232.74 ms (minimum),
  and the whole open took 574.25 ms instead of 787.89 ms. Outputs stayed
  bit-exact, the job count exact, and the smoke passed 20 of 20. Stress runs
  of 10 minutes that flipped `map_batch` every 60 s gave no error. See
  `receipts/2026-10-03-ane-iommu-batch/README.md`.

### Fixed

- `ane_stats`: a submission that opened a busy period while the previous
  period was closing could read the previous end too early, so `busy_ns`
  could count the overlap twice. The start of a busy period is now read
  after the submission wins the transition, so periods never overlap (#93).
  Both drivers.
- The M3 data files `data/ane-soc/t8122.json`, `t6030.json` and `t6031.json`
  no longer name the 0x4000 window at engine + 0x1050000 as the ANE mailbox,
  or give a reordered mailbox IRQ list (#98). No source gave either. That
  offset is RVBAR on T6021. Both values are now null, with the expected
  wrapper + 0x8000 address in the reason. Also corrected: the mailbox
  compatible in all four M3 files (the T6030 one contradicted its own
  source), the j575d board (BuildManifest chip id 0x6032) and its three
  missing die-1 windows, the T8122 iommu-parent reason, and the driver
  family. See `receipts/2026-10-03-ane-h15`.
- `omarchy-ane-check` lists `sudo omarchy-ane-firmware-fetch` in the
  bring-up steps of every SoC that `ane_t6021` drives (#92). Before, a T6021
  that a promotion revert made opt-in again got no firmware step.
- `ane_t6021` logs its probe and firmware boot progress at debug and info
  level, not at emergency level (#94). Emergency lines go to every console
  and terminal at any loglevel, on every M2 Max boot. The `BOOT-PHASE`
  markers and the register readouts are debug messages now: turn them on
  with dynamic debug (for example `ane_t6021.dyndbg=+p` on the kernel
  command line). The outcome lines are info, and the dump when the ANE CPU
  starts but never reports READY is an error. aurora-silicon/linux #155 has
  the same change (Chris Kearney).
- `ane` and `ane_t6021` log register and address tracing at debug level
  (#94): the `ANERD`/`ANEWR` recovery trace, the `ps probe` SET-window
  reads, the `ANE-resume` progress lines, each DART, the ChMan table
  entries, RTKit endpoint starts and messages, and the firmware alias and
  staging addresses. Dynamic debug shows them (`ane.dyndbg=+p`,
  `ane_t6021.dyndbg=+p`). The only info lines left on the `ane` probe path
  are `DART containment armed` and `loaded ane`. Faults stay at error and
  warning level.
- `modinfo ane_t6021` shows one license and one description (#94). Each of
  the three objects of the module carried its own.
- The T6000, T6001 and T6002 overlays have no `ane_set5` power state at
  pmgr 0xc030 (#94). It is past the end of the ANE pmgr range in the ADT
  (0x28e080000+0xc02c), and the ane node does not use it.
- The T8103, T600x and T8112 overlays set `status = "okay"` on their ANE
  power states (#94). aurora-silicon/linux #155 has these nodes at the same
  paths, disabled where its ANE is disabled (T8103, T6000, T6002, T8112).
  On such a kernel `omarchy-ane-dt` refused the overlay ("power-domains
  names ..., which is disabled") and the ANE stayed off.
  `tools/test_ane_overlays.py` now also disables the power states when it
  checks the overlay over disabled kernel nodes.
- The host suites run from a `git archive` of the tree, as in the release
  tarball: `tools/test_promote_chip.py` and `tools/test_promote_from_verdict.py`
  no longer need `.git` (#92).

### Research and receipts

- H17 b0 (`receipts/2026-10-03-ane-h17`, #102): T8140 (MacBook Neo), T8142
  (M5) and T6050 (M5 Pro/Max) are the `ane_t6021` firmware-boot family, on
  the H16 window and DART layout plus the `exclave-*` properties. T8140 has
  the six M4 (T8132) windows with the same sizes; T8142 differs in one
  window size; T6050 has the seven-window shape of the M4 Max (T6041). The
  receipt compares the 27 per-SoC entries of `ane_t6021` with the H16 b0
  values (7 have no local value), names what the probe and the owner's
  macOS can supply, and plans b1 as H17 rows in `ane_h16`.
  `data/ane-soc/t6050.json` no longer says that the j775d `ane1` has no
  exclave marking: no source measured it.
- H18 b0 (`receipts/2026-10-03-ane-h18`, #99): T8152 (M6) is the `ane_t6021`
  firmware-boot model as a new `ascwrap-v8` variant, and T8150 (A19 Pro) is
  Exclave-owned. The receipt lists the 28 per-SoC entries a T8152 smoke
  module needs (13 have no local value), the macOS `ioreg` commands an M6
  owner can run to supply some of them, and the b1 module plan.
  `data/ane-soc/t8152.json` records the firmware `_rtk_patchbay` tags and
  load commands, the `ane1` clock-gates, and the new family statement. Its
  `compiler` leaf is gone: it cited the aurora DT and repeated
  `hwx_lab_cross_target`.
- The T6021 power-down study (`receipts/2026-10-03-t6021-powerdown`, #105):
  the state that a power-domain cycle loses, the firmware power commands, and
  ranked designs. Offline, no hardware.

### Known limits

- Each on-by-default chip is tested on one machine.
- On Omarchy, the overlays apply only through an omarchy-mac-boot with device
  tree overlay support (omacom/omarchy-mac#677, not merged).
- `dyn_pg=1` does not gate the compute islands on the tested M2 Max firmware,
  and `dyn_pg=1 boot_prevent_nap=0` makes probe fail. Keep the defaults.
- `ane_t6021` keeps the ANE powered while it is bound; only `ane` suspends.
- Closing a program still unmaps its buffer objects page by page: the
  release of the whole encoder program took about 178 ms on T8103 and
  241 ms on T6001, the same with and without `map_batch`.
- No silicon has run `ane_h16`.
- The hardware gates of this release are in the GitHub release notes.

## 0.4.1 (2026-10-02)

Both drivers count ANE work for monitoring, and the M1 family has its own
smoke program. This release also adds a read-only ANE state probe, a
promotion flow that runs with no person in the loop, and cited ANE data for
twelve chips after the M2 that no driver binds. The ioctl interface (ABI 1
and ABI 2) does not change.

### Added

- `ane` and `ane_t6021` count ANE work (#57, #59, #63, #82). The sysfs file
  `/sys/class/accel/accel*/device/ane_stats` (mode 0444) gives `busy_ns` and
  `jobs`. `busy_ns` is the union of the submit-to-completion windows since
  the device bound, and it increases while work runs. `jobs` counts engine
  submissions: one per `ANE_SUBMIT` on `ane`, and one per firmware procedure
  call on `ane_t6021`. Program loads and other control messages do not count.
  The debugfs file `ane_timeline` (in `ane/` or `ane_t6021/`) lists the last
  256 submissions. The module parameter `stats` (default 1) controls both
  files. With `stats=0`, the driver creates neither file and the hot path is
  one branch. `tools/test_ane_stats` checks the counter logic on the host and
  runs in `make -C tools check`.
- `omarchy-ane-smoke` runs on the M1 family (#60, #62). T8103, T6000, T6001
  and T6002 run the H13 add program `fixtures/h13-anec/add/program-0.anec`.
  T6020, T6021, T6022 and T8112 run the H14 add program, as before. The H13
  check reads 64 fp16 planes and requires zero padding in the rest of the
  16 KiB output. See `receipts/2026-10-02-h13-smoke/README.md`.
- `omarchy-ane-probe` (`tools/omarchy-ane-probe`) prints the ANE state of an
  Apple Silicon Linux machine as one JSON document (#74). It reports the ANE,
  DART, power-domain and mailbox device-tree nodes (also of unknown
  generations), the `ane*` modules and interrupts, the runtime-PM state, the
  pacman versions, the DKMS module files, and the results of
  `omarchy-ane-check` and `omarchy-ane-firmware-fetch --check`. It compares
  the device tree with `data/ane-soc/SOC.json` when that file exists. It is
  read-only, needs no root and no network, exits 0, and keeps the document
  within 8 KiB. See `docs/ane-probe.md`.
- Data-only SoCs (#78, #85). `data/ane-soc/SOC.json` keeps cited ANE data for
  a chip that no driver binds, and `tools/validate_ane_soc.py` checks the
  file. `packaging/build-dtbo` installs each file as
  `/usr/share/omarchy-ane/soc/SOC.json`. It compiles a data-only overlay
  (`packaging/dt/SOC-ane-dataonly.dts`, root property `omarchy,data-only`) as
  a check, and never installs it. `omarchy-ane-dt` never applies one.
  `omarchy-ane-dt status` prints `data-only (no driver): SOC`, and
  `omarchy-ane-check` prints `DATA-ONLY SoC: SOC (no driver yet)`.
  `tools/gen_coverage_table.py` writes the README data-only table. CI: the
  `ane-soc-data` job, the data-only overlays in `tools/test_ane_overlays.py`,
  and the `aurora-dtbs` workflow for the M3 and later board device trees. See
  `docs/ane-soc-data.md`.
- Data files for twelve chips that no driver binds (#87): H15 (T8122, T6030,
  T6031, T6034), H16 (T8132, T6040, T6041), H17 (T8140, T8142, T6050) and
  H18 (T8150, T8152). `packaging/build-dtbo` installs the twelve files to
  `/usr/share/omarchy-ane/soc/`, where `omarchy-ane-probe` compares them with
  the device tree. Data-only overlays exist for T8132, T6040, T6041, T8140
  and T8152; `build-dtbo` compiles them and installs none. Each generation
  has a receipt (`receipts/2026-10-02-ane-gen-h15`, `-h16`, `-h18`,
  `receipts/2026-10-02-neo-ane`). The README data-only table gives the SoC,
  the internal name, the board count and the overlay state, and the README
  names the four ANE shapes after the M2.
- `tools/promote_chip.py` moves a chip between opt-in and on by default
  (#65). It edits the files that the earlier promotion PRs edited by hand:
  the overlays table row and tested header, the overlay's opt-in marker, the
  `omarchy-ane-check` chip lists, the `omarchy-ane-dt` opt-in list,
  `DEFAULT_ON` in `omarchy-ane-firmware-fetch`, the README chip table, and
  one CHANGELOG line. `--check` prints the diff and writes nothing. A second
  `--apply` changes nothing, and the reverse flip restores each file byte for
  byte, except the CHANGELOG line.
- The `promotion` workflow runs the promotion flow with no person in the loop
  (#66-#69, #71, #72, #75-#77). A daily cron judges the community rows and
  opens or updates one auto-promotion PR for each `PROMOTE` or `REVERT`
  verdict. The gate runs `make -C tools check` on the unchanged tree, the
  host tests on the PR, and a fresh verdict. Then it squash-merges the PR and
  cuts the patch release. `dry_run` prints the plan. The workflow refuses
  synthetic verdict fixtures in this repository.
- `make -C tools check` also runs `tools/test_libane_ioctl` (#55). It builds
  libane with `-ftrivial-auto-var-init=pattern` against a fake DRM node. The
  fake node refuses a nonzero pad, flags or reserved member, as the in-tree
  drivers do. No device runs this check.

### Changed

- Promotion (#61): one passing community row promotes an opt-in chip. The
  0.4.0 thresholds (3 rows from 3 machines, 2 owners, 2 boards and 2 kernel
  releases, and the uptime rule) are gone. Passing and failing rows for the
  same chip give `CONFLICT`, and the chip does not promote. A chip that is on
  by default goes back to opt-in when its latest judged row is not clean.
  Rows from a machine without omarchy-ane, and clean rows with no smoke
  attempt, are not judged.
- `omarchy-ane-check` prints the bring-up steps after its `UNTESTED SoC`
  line (#64): add the opt-in key, run `omarchy-ane-firmware-fetch` first on
  T6020, T6022 and T8112, then `omarchy-ane-dt apply`, `update-m1n1` and a
  reboot, and submit a collector row with `--ane-smoke` when the machine is
  idle. The lines that scripts read do not change.
- `omarchy-ane-firmware-fetch` (the install, `--check` and `--hook`) and
  `omarchy-ane-check` accept the Asahi vendor firmware copy (#56). The kernel
  loads `/usr/lib/firmware/vendor/apple/ane/NAME` before
  `/usr/lib/firmware/apple/ane/NAME`. A vendor copy that matches the pin is
  enough, and the tool fetches nothing. A vendor copy that does not match
  shadows the fetched copy, so the driver refuses the firmware. Then
  `--check` and `omarchy-ane-check` fail, the install exits 1, and `--hook`
  prints a note. With no matching vendor copy, the tool installs to
  `/usr/lib/firmware/apple/ane/`, as before.
- The DKMS source tree also needs `ane/ane_stats_show.c` and `ane/include/`.
  Both modules compile the shared stats code from there. A package recipe
  must copy them next to `ane/Makefile`, `ane/src/` and `ane/t6021/`.
- The host tests read the on-by-default chips from the overlays table and
  from `DEFAULT_ON`, so a promotion needs no test edits (#65, #70, #73, #84).

### Fixed

- libane sets the `pad` member of `struct drm_ane_bo_free` and of each
  `struct drm_ane_generic_bind` to zero (#55). Before, these members held
  stack bytes. The in-tree `ane_t6021` driver refuses a nonzero pad with
  `-EINVAL`, so ABI 2 `PROG_LOAD` could fail, and `BO_FREE` could keep the BO
  until the fd closed. The DKMS drivers of 0.4.0 accept the zero pads.
- `tools/promotion_check.py` counts a fault line only when it names the ANE
  device, its DARTs or its mailbox (#61). Before, an apple-dcp RTKit line
  gave a false T6001 `REVERT`, and rows from machines without omarchy-ane
  counted as T6000 and T8112 failures.
- `CHANGELOG.md` has the 0.1.0 to 0.4.0 sections again. #57 removed them.

### Research and receipts

- The T6021 default-on gate: boots A (reserved mode), B (own memory), P (the
  packaged boot.bin with the stock m1n1 1.6.1) and a final boot all passed 16
  of 16 gates, with the encoder bit-exact (#53).
- The 19 macOS words of each bulk DART, written at DART reset in the macOS
  order, give the same fault as the write on a live DART: `NO PGD FOR IOVA`
  at the first CALL. DART 0x20c with the macOS value does not work with the
  Linux page tables in any write order (findings section 33, #54).
- The U-Boot `mtpkbd` candidate patches point at the
  joshuaswarren/aurora-u-boot branches (#58).

### Known limits

- The DART 0x20c write at DART reset, open in 0.4.0, is rejected (#54). One
  DART test stays open: 0x20c together with the DVA window and BOs above
  4 GiB. The cause of the encoder time on the M2 (254 ms under Linux, 89 ms
  under macOS) is still unknown.
- No driver binds the twelve data-only chips. With the package installed,
  `omarchy-ane-check` prints `DATA-ONLY SoC` on them, and `omarchy-ane-dt`
  applies no overlay for them.
- Hardware gates: T8103 and T6001 passed the `ane_stats` acceptance and the
  H13 smoke (20 of 20 calls bit-exact) on `cbb1beb`, with `ane.ko` built
  from source, not from the package. From `cbb1beb` to this release, no file
  under `ane/`, `libane/`, `fixtures/` or `packaging/omarchy-ane-smoke`
  changes. The T6021 result is in the GitHub release notes.

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
