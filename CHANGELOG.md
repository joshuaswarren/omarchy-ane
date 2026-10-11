# Changelog

## Unreleased

## 0.4.8 (2026-10-10)

This release is for the Apple M2 Max (T6021). It also holds the v0.4.7 changes below, which were tagged without a changelog heading. The GitHub release notes list the hardware evidence and its limits.

- `ane_t6021`, `libane`: new `DRM_IOCTL_ANE_PROG_LOOKUP` (0x7, appended to the
  uAPI). A process asks the driver for a program by the SHA-256 digest of its
  sections. If the driver already holds that program, the process skips the
  section allocation. A kernel module that lacks the call, or a miss, falls
  back to the old load path. Measured on one M2 Max, module build `bd34a12`
  (#141, `receipts/2026-10-10-m2-max-prog-lookup`): three passes over the
  38 Qwen3.8-2B programs, one process each, loaded 38 of 38 every time. The
  allocator total grew by 2,416,623,616 B, 196,608 B and 0 B. On `ca09ce8` the
  second pass failed 5 times with `BO_INIT failed`. One run, zero-valued
  inputs.
- `tools/qwen_m2_decode.py --resident`, `ResidentSession` and the proxy A/B
  harness, with host tests (#144). A resident decode on `bd34a12` gave logits
  that equal the per-call baseline (`receipts/2026-10-10-m2-max-resident-qwen-decode`).
- Receipts for the Parakeet whole encoder on the M2 Max (#138, #142).

- `tools/qwen_m2_decode.py` warns at the start of a run when numpy uses the reference BLAS, and
  `docs/qwen-m2-decoder-requirements.md` says to install `blas-openblas`. Measured on the M2 Max
  (2026-10-10, module `bd34a12`): the output projection took 604 ms per generated token on the reference
  BLAS (any thread count) and 20 ms on OpenBLAS; the resident Qwen3.8-2B decode went from a generation
  step p50 of 0.826 s (1.21 tokens per second) to 0.211 s and 0.226 s in two runs (4.64 and 4.48), with
  the same decoder files and the same 16 generated tokens. The float32 logits differ at the rounding
  level (largest difference 4.0e-5, so the old logits hash no longer matches); the new hash is the same in
  both runs. Receipt: `receipts/2026-10-10-m2-max-resident-decode-openblas/`. Verified the same package
  swap on an M1 Max and an M1 (matvec 21 ms and 47.5 ms).

- `docs/ultra-test-runbook.md`, `docs/h15-volunteer.md`: audit against the
  code they describe. Opt-in path is `/etc/omarchy-mac-boot/dtb-overlays.opt-in`
  (`omarchy-ane-dt` has read that since 2026-10-04; the runbook still named the
  `omarchy-platform` one). Stage 2 names the die-1 smoke honestly: the shipped
  smoke measures the runner's default device and its `die` field is computed
  from the first bound node, so the die-1 run is `ANE_DEVICE=1` and the report
  must say which run each JSON came from; the stop rule adds the die-1 mailbox
  (0x2285408000), which `promotion_check.py` counts as an ANE line; the
  `ultra-die1.md §9.2` anchors point at §9 fact 2, the only numbering that
  section has. `h15-volunteer.md`: the kernel fork is
  `joshuaswarren/aurora-linux-tbnet` (branch `ane-driver-aurora` at `efe6e359`
  re-verified via the old name's redirect), and the collector-not-wired check
  is re-dated 2026-10-10.

- `ane_t6021`: the parked-BO pool never parked anything: the pool entry's
  size was never recorded, so every admission check saw 0 bytes and every
  freed duplicate section was really freed (measured on the M2,
  2026-10-09, boot 6fcfac08: `bo_pool_bytes` stayed 0 through a whole
  A/B). `ane_t6021_pool_park()` and `ane_t6021_pool_park_uncharged()` now
  take the size and record it on the entry themselves, so the caller
  cannot forget it.
- `tools/ane-session`: a LOAD whose (anec bytes, ports bytes) pair is
  already loaded rebinds the name with no device work; FREE drops the
  name binding only and the cached program dies at QUIT. A configure
  pass that FREEs and re-LOADs the same 38-program table now costs zero
  `BO_INIT`s (measured 2026-10-09: every pass's duplicate sections had
  asked for a fresh 223 MiB dma32 hole the window no longer had).
- `tools/ane-bo-probe`: each step now prints the power-of-two IOVA size
  class the request lands in (the dma32 IOVA allocator rounds and
  naturally aligns every coherent allocation to that class, so a
  223 MiB BO needs a free 256 MiB window).
- `ane_t6021`: a freed duplicate section BO of at least `bo_pool_min_kb`
  (default 2048 KiB) now parks in the BO pool under a runtime budget
  `bo_pool_max_mb` (default 512 MiB, 0644), evicting the oldest park by
  real free. `BO_INIT` of the same page-aligned size gets the same dma
  range back, so repeated configure passes stop churning the dma32
  window into fragments no 224 MiB section fits: measured 2026-10-09 on
  boot 2105990f, a 222,980,416-byte `BO_INIT` failed in every process
  at `bo_total_bytes` 2.76 GB after the passes had run. New read-only
  `bo_pool_bytes`. The policy lives in `ane/t6021/ane_t6021_pool.h`
  with a host unit test (`tools/test_t6021_pool`).
- `libane`: the ABI-2 open allocates its six section BOs in descending
  size order, so the largest contiguous request lands while the
  window's largest hole is still fresh. The ioctl regression records
  the `BO_INIT` order and fails on the old order.
- `libane`: the ABI-2 (M2) client releases its six section BOs as soon as
  `PROG_LOAD` + `PROC_CREATE` succeed. On a first load the driver holds
  the section bytes anyway (`fw_ref`); on a dedup hit the client's copy
  is memory the firmware never reads. Before, every live `ane_nn` pinned
  a full duplicate of its program's sections inside the sub-4-GiB window
  until close, so a resident session failed `BO_INIT` for prog_006's
  222,980,416 B kernel section once the 38-program set was cached:
  measured 2026-10-09 on boot 2105990f, `REFUSE: prog_006` with
  `bo_total_bytes` at 2,757,607,424. The host regression
  (`tools/test_libane_ioctl`, "resident pattern" case) fails on the old
  libane and passes now.
- `ane_t6021`: `trace_td=1` now creates the debugfs blob in its own
  directory, `ane_t6021_trace/trace_td` (was `ane_t6021/trace_td`). The
  probe (`stats=1`, the default) owns the `ane_t6021` debugfs directory;
  the trace setter created the same name again, lost the collision
  (`debugfs: 'ane_t6021' already exists`), and the parameter flipped with
  no `trace_td` file to read (measured on the M2, 2026-10-07). The trace
  now owns its directory outright, so any load order works and neither
  side removes the other's files. Update readers of the old path.

## 0.4.6 (2026-10-06)

This release makes each H14 (T6021, M2 Max) CALL faster and adds the first
Ultra (two-die) plumbing. The CALL change is in `ane/t6021` only. On one
M2 Max boot (aurora 11.38) with the same sampler, the ledger ANE cell goes
from 2,932.5 to 3,348.0 jobs/s (+14.1 %) and the add median from 0.359 to
0.257 ms with `call_poll_us=1000` on top of `call_settle_us=0`. The Ultra
change also touches `ane.ko` and libane (see its entry). The M1 and M2
drivers keep ABI 1 and ABI 2. The earlier removal of the 1,000 us settle
(about 674 to about 3,206 jobs/s on the aurora 11.36 install, the
`call_settle_us` entry below) did not reproduce its absolute figure on
11.38: the same configuration measured 2,933 there. Quote the paired
numbers from one boot.

- `ane_t6021`: the CALL finish-event wait polls at a 1 us cadence for the
  first `call_poll_us` microseconds (default 1000), then falls back to the
  50-100 us sleep, so the 254 ms encoder does not spend its wait on timer
  wakeups. `call_poll_us=0` restores the old cadence at run time. A 300 us
  busy-spin head added +0.4 % for about one core and is not included.
  Receipt: `receipts/2026-10-06-t6021-call-poll/README.md`.

- Ultra die-1 plumbing (#120, static: no T6002 or T6022 hardware run).
  `ane.ko` takes the SET base from the node's set `reg` window and
  qualifies (compatible, SET base) pairs through a die-keyed table; T6002
  die 1 is recognized behind `allow_unqualified`, and an unknown base
  refuses. `ane.ko` names its stats debugfs directory per device, and die 0
  keeps the legacy name. `ane_t6021` keys the T6022 per-die data on the same
  window. `omarchy-ane-firmware-fetch` refuses on a die-1 node (no pin).
  New opt-in overlays `t6002-ane-die1` and `t6022-ane-die1`. libane:
  `ane_m2_init_ports()` takes a new `dev_id` argument, and `ane-run` gains
  `--dev` / `ANE_DEVICE`. Callers of `ane_m2_init_ports()` must pass the
  device id (0 for the first accel node).

- `ane_t6021`: the Makefile searches `ane/src` with `-iquote`, so a kernel
  header package that ships its own `include/uapi/drm/ane_accel.h`
  (linux-aurora-headers does) no longer shadows the in-tree UAPI header.
  Before the fix the out-of-tree `ane_t6021` build failed on stock aurora
  11.36 headers with `ANE_M2_MAX_BINDS` and `ANE_ABI_M2_MAJOR` undeclared.
  The M1-family module was not affected (#132).

- `ane_t6021`: a CALL returns on the firmware's IO_T2H finish event with
  no fixed sleep after it. `call_settle_us` now defaults to 0 (was 1000
  us per call): the finish event already marks the output in DRAM. H14
  add on the M2 Max drops from 1.478 to about 0.31 ms per call. A
  completion timeout now logs at error level and quarantines as before.
  Receipt: `receipts/2026-10-06-t6021-call-settle/README.md`.

- `tools/wdt-hang-test`: an out-of-tree test module that arms the Apple SoC
  watchdog (WD1) through the kernel-registered `apple_wdt` device (DT-window
  fallback only), pets it from a kernel thread, then hangs the CPU with
  interrupts off, to prove the unattended reset-to-stock recovery under a
  one-shot boot (Limine `LoaderEntryOneShot`, GRUB `next_entry`). W=1 clean
  on the 7.1.13-3-1-ARCH and aurora trees; experimental and hardware-gated:
  it never autoloads and is not packaged. Receipt:
  `receipts/2026-10-04-wdt-hang-test/README.md`.

## 0.4.5 (2026-10-04)

This release moves the device tree overlays and the opt-in file to the
overlay contract of omarchy-mac-boot. The move breaks hand installs and
opt-in files from earlier releases. The release also adds the opt-in H15
(M3) bring-up module. It changes no code that DKMS builds and no libane
code: `git diff v0.4.4..v0.4.5 -- dkms.conf ane/Makefile ane/src
ane/include ane/ane_stats_show.c ane/t6021 libane` is empty, so `ane.ko`,
`ane_t6021.ko`, libane and the ioctl interface (ABI 1 and ABI 2) are the
bytes of v0.4.4. The new `ane/h15/` tree is not in `dkms.conf`, and the
package does not build it. The packaged drivers, libane and the firmware
fetch are unchanged, so this release has no new hardware run. The path move
is tested on the host only.

- Breaking: the overlays move to the overlay contract of omarchy-mac-boot
  (#124; omacom/omarchy-mac-pkgs#3, the port of omacom/omarchy-mac#677).
  They install to `/usr/lib/omarchy-mac-boot/dtb-overlays/PREFIX/`, the
  directory that omarchy-mac-boot ships for package-owned overlays. They
  were in `/usr/share/omarchy-platform/dtb-overlays`. The second `Target`
  of `90-omarchy-ane-dt.hook` is now
  `usr/lib/omarchy-mac-boot/dtb-overlays/*/*.dtbo`, the glob of the
  omarchy-mac-boot hook. A package upgrade moves the files. A hand install
  (`packaging/build-dtbo /`) must move: while `.dtbo` files are in
  `/usr/lib/omarchy-platform/dtb-overlays` or
  `/usr/share/omarchy-platform/dtb-overlays`, `omarchy-ane-dt apply`
  refuses, keeps the current copies, and names the steps.
- Breaking: the opt-in file is now
  `/etc/omarchy-mac-boot/dtb-overlays.opt-in`. It was
  `/etc/omarchy-platform/dtb-overlays.opt-in`. Re-add your opt-in lines to
  the new file. Nothing reads the old file, and the package does not copy
  its lines. `omarchy-ane-check`, the docs and the H15 and H16 runbooks
  name the new file (#124).
- On Arch Linux ARM installs without omarchy-mac-boot, the package creates
  the overlay directory, and `omarchy-ane-dt apply`, `update-m1n1-dtbs`
  and the two `90-omarchy-ane-dt` hooks work as before (#124).
- H15 (M3) opt-in experimental bring-up module (#121): `ane/h15/` with
  four SoC rows (T8122, T6030, T6031, T6034), a module with four stages
  (dt, status, wrapper, boot) and a host self test of its ADT walker
  (`make -C ane/h15 check`); three experimental overlays in `ane/h15/`
  (T8122, T6030, T6031; opt-in key `ane-h15-experimental`); three
  data-only overlays in `packaging/dt/`; and `tools/omarchy-ane-h15-stage`.
  The README data-only table marks the new overlays (#122). The module has
  no `MODULE_DEVICE_TABLE`, so nothing loads it automatically, and its
  probe refuses unless `optin=` names the SoC. No M3 has run it. Only
  stage 1 touches MMIO: it reads the PMGR power-state words and writes no
  register. Stages 2 and 3 refuse on H15 until a macOS capture clears the
  words that are INFERENCE now.
- H15 host-side fixes (#123): the module compiles (a macro that holds a
  list was used as an expression); the ADT walker matches child names,
  accepts nodes with no properties, reads segment-ranges as 64-bit fields
  and no longer reads an uninitialized error; the self test no longer hangs
  on property sizes that are not a multiple of 4; the probe refuses
  (`-ENXIO`) when `of_iomap` returns NULL; `ps_wait_ms` is a limit for each
  word, as documented; the compile line in `ane/h15/README-bringup.md`
  uses `-O dtb`. #123 reports a clean W=1 build against the aurora
  `ane-driver-aurora` tree. The T8122 overlay applies to all five T8122
  board device trees. New volunteer runbook: `docs/h15-volunteer.md`.

### Known limits

- No hardware ran this release. The code that DKMS builds and libane are
  the bytes of v0.4.4, whose hardware results are in
  `receipts/2026-10-03-omarchy-ane-0.4.4/README.md`. The path move is
  tested on the host only, with fake roots
  (`receipts/2026-10-04-omarchy-ane-0.4.5/README.md`). The release gates
  did not install it on a Mac.
- T6000 and T6020 stay on by default in the overlays table of this release.
  The Omarchy package (omacom/omarchy-pkgs#745) keeps them opt-in through
  its recipe until a passing row from the DKMS modules of this package
  exists. On these chips, the overlay and the DKMS modules of this package
  have not run: their rows ran the kernel's own ANE driver and device tree.

## 0.4.4 (2026-10-03)

This release fixes the packaging defects of v0.4.3 and changes no driver or
libane code: `git diff v0.4.3..v0.4.4 -- ane libane dkms.conf` is empty, so
`ane.ko`, `ane_t6021.ko`, libane and the ioctl interface (ABI 1 and ABI 2)
are the bytes of v0.4.3. The v0.4.3 tag sits on `0477f72`, the merge of the
T6000 promotion, one commit before its CHANGELOG move. So the v0.4.3 tarball
has no 0.4.3 section, and it carries a committed `__pycache__` file, stale
T6000/T6020 wording and a failing `test_promote_chip`. This release
supersedes it: its tarball carries the 0.4.3 and 0.4.4 notes.

- The promotion release step cuts the release it should (#117). The release
  notes cover every promotion merged since the last tag (verdict, chip, row
  shas, driver_source, merge sha), and the tag goes on the release commit,
  so the tarball carries its own CHANGELOG section, with `## Unreleased`
  kept as the first section. v0.4.3 tagged the T6000 merge before the
  CHANGELOG move and named only T6000. As merged in #117, the move left the
  entries under `## Unreleased` and wrote no version heading; this release
  writes the heading, and `test_promote_from_verdict` checks the moved text
  byte for byte, with an entry to move. Its fixture already held a
  `## 0.4.1` heading, so the old check passed without one. After a release,
  `## Unreleased` sits empty right above the released heading, and
  `promote_chip.py` keeps a blank line between a new entry and that heading
  (`test_promote_chip` checks it).
- The promotion flip is state-independent (#117). The check's case lists and
  the tested-header list take their order from rules (T8103, T6001 and T6021
  first, promoted chips sorted after them), not from seats in the v0.4.2
  five-chip list. Each `<chip>-ane.dts` carries one tool-written
  `Overlay state:` line, the offline suites take their chips from the
  overlays table, and `test_promote_chip` runs in the promotion gate. In
  v0.4.3 a T6002 promote and revert reordered the check's opt-in list, and
  the suite failed.
- `propose` stages only the tracked files that the flip wrote (#117).
  `git add -A` had shipped a committed `__pycache__` file in v0.4.3; it is
  removed, and `__pycache__/` and `*.pyc` are ignored now.
- The T6000/T6020 prose matches 0.4.3 (#117): the README's default-on line
  and the evidence cells name the passing rows 6eb94f49985b and
  3c9389040f51, the overlay dts headers and the libane receipt stop calling
  them untested or opt-in, and the libane entry and receipt carry the
  hardware numbers of #111.
- The 0.4.3 section below lists the rest of what v0.4.3 shipped: the probe
  change (#79), the T6020 firmware fetch at install, and the development-only
  changes (#108, #109).

### Research

- `docs/ultra-die1.md`: a design for the die-1 ANE of T6002 (M1 Ultra) and
  T6022 (M2 Ultra), from a read-only capture on one M1 Ultra under macOS and
  the ADT decode of both Ultra boards (`receipts/2026-10-03-ultra-die1`).
  Design only, no code: no Ultra runs Linux today.

### Known limits

- T6000 and T6020 are on by default from one passing community row each.
  Both rows ran the kernel's own ANE driver and device tree; on these chips
  the overlay and the DKMS modules of this package have not run.
- The hardware results of this release are in
  `receipts/2026-10-03-omarchy-ane-0.4.4/README.md`.

## 0.4.3 (2026-10-03)

- T6000 (M1 Pro) ANE on by default. (row 6eb94f49985b)
- T6020 (M2 Pro) ANE on by default. (row 3c9389040f51)
- A chip that is on by default and has a passing in-tree row gets the
  `aurora-dt` target, so its aurora-silicon/linux PR still comes after the
  overlay flip. The aurora tree decides: a disabled node is planned, an
  enabled node is a no-op, and no node is `SKIPPED`. Before, the target was
  lost once the overlay PROMOTE merged.
- The promotion gate computes the fresh verdict on the base branch. It ran
  on the PR head, where the chip is already flipped, so every PROMOTE PR
  failed (runs 37144741326 and 37146125714, PR #113). The gate now merges
  while the verdict and the targets stay the same, every judged row keeps
  its outcome, and no new judged row fails; new passing rows and rows that
  are not judged do not refuse a PR. It refuses to run on a checkout that
  already has the PR's flip.
- `propose` closes its own open promotion PR (labeled `auto-promotion`, from
  this repository) whose chip it no longer proposes, with a comment, so no
  stale PR reaches the gate.
- `promote_from_verdict.py aurora-plan` skips a chip that has no ANE node on
  any board of the aurora base branch, as `aurora-wip` before #155 merges.
  It prints a line that starts with `SKIPPED`, writes no plan entry and
  exits 0, so `aurora-pr` does not run and the promotion run stays green.
  Before, the run failed (promotion run 37143877823, T6000). A build error,
  a node on some boards only, or a node that the change cannot enable still
  fails the plan.
- libane (M1/ABI 1) loads the ANEC payload straight into the command buffer
  object and parses the task stream there: one pass instead of two and no
  458 MB staging buffer resident for the life of the network. `nn->data` is
  `NULL` on this path. `ANE_LOAD_STAGED=1` restores the old staged load for
  A/B. The direct path reports `model_header`, `model_map`, `model_map_copy`
  (or `model_pread_fallback`), and `model_zero_tail` stages. The ABI and every
  header are unchanged; the T6021 (ABI 2) loader is untouched. Verified on
  hardware before the merge (#111): on T8103 the whole-encoder cold open went
  from 178.3 to 107.9 ms `init_total` (182.4 to 101.1 ms on the repeat arm),
  `open_ms` 67-72 ms less; on T6001 from 361.4 to 286.5 ms, `open_ms` 828.5 to
  760.1 ms; encoder output bit-exact across arms and smoke 20/20 on both
  chips. Protocol and runs: `receipts/2026-10-03-libane-no-staging/README.md`.
- `omarchy-ane-probe` reads data-only tables whose sections or `reg` lists
  are wrapped as `{v, src}` leaves (#79). Before, a wrapped `reg` list failed
  the whole `soc_table` section.
- On T6020 the pacman hook fetches the ANE firmware at install and upgrade:
  `omarchy-ane-firmware-fetch` has `apple,t6020` in its default list.
- Development only, not installed by the package: every shell script runs
  with `set -euo pipefail`, the H13 program-load verifier is split and has a
  golden test (#108), and the pre-push hook finds `privacy_check.py` (#109).

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
  CONFIG_GET (-110), and `ane_t6021` registers no device. Design:
  `receipts/2026-10-03-t6021-dynpg/README.md`; measurements:
  `receipts/2026-10-03-t6021-v042-gate/README.md` (#106).
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
  laptop (T8103) and one M1 Max laptop (T6001), with `ane.ko` from the #95
  head `198db99`, before #104 changed the map path: with the ANE suspended,
  idle system power per 5-minute block equals the level with the module
  removed (T8103 within 33 mW, T6001 within 7 mW). With the ANE held on, it
  was 107 to 173 mW higher on T8103 and 2.10 to 7.65 W higher on T6001. The
  first open after 5 s of idle took a median of 330 µs on T8103 (10 µs when
  warm) and 0.35 ms on T6001. Whole-encoder outputs stayed bit-exact, smoke
  runs from the suspended state passed 20 of 20, and about 300 suspend and
  wake cycles per machine gave no error. Design:
  `receipts/2026-10-03-ane-autosuspend/README.md`; measurements:
  `receipts/2026-10-03-omarchy-ane-0.4.2/README.md`.
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
  of 10 minutes that flipped `map_batch` every 60 s gave no error. Design:
  `receipts/2026-10-03-ane-iommu-batch/README.md`; measurements:
  `receipts/2026-10-03-omarchy-ane-0.4.2/README.md`.

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
- The T6021 gate of this release (`receipts/2026-10-03-t6021-v042-gate`,
  #106), on one M2 Max laptop with the `ane_t6021` of `5a457a3` (srcversion
  `59494CBC56F28ED8D1122C6`): at the default parameters the release gate
  passed (smoke, 9 gate ops, `ane_stats` items 1-4), and the whole encoder
  was bit-exact at 254.306 ms min-of-min. `dyn_pg=1` was accepted but did
  not gate, and `dyn_pg=1 boot_prevent_nap=0` was refused at probe
  (CONFIG_GET -110). On two boots with the driver never loaded, idle power
  was 14.46 to 14.50 W (5-minute block medians, fans off), against 14.69 to
  14.77 W with the driver bound: holding the driver costs 227 mW (95 %
  interval 207 to 246 mW, same session) to 275 mW (260 to 290 mW, against
  the gate run's blocks). With the driver never loaded, `ane_cpu` and
  `ane_sys` stay on and the other seven ANE power domains are off.

### Known limits

- Each on-by-default chip is tested on one machine.
- On Omarchy, the overlays apply only through an omarchy-mac-boot with device
  tree overlay support (omacom/omarchy-mac#677, not merged).
- `dyn_pg=1` does not gate the compute islands on the tested M2 Max firmware,
  and `dyn_pg=1 boot_prevent_nap=0` makes probe fail. Keep the defaults.
- `ane_t6021` keeps the ANE powered while it is bound; only `ane` suspends.
  On one M2 Max laptop that costs about 0.23 to 0.27 W at idle against a
  boot where the driver never loads (#106).
- Closing a program still unmaps its buffer objects page by page: the
  release of the whole encoder program took about 178 ms on T8103 and
  241 ms on T6001, the same with and without `map_batch`.
- No silicon has run `ane_h16`.
- The hardware results of this release are in
  `receipts/2026-10-03-omarchy-ane-0.4.2/README.md` and
  `receipts/2026-10-03-t6021-v042-gate/README.md`.

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
