# Build proofs — agx_stats series (replacement PR #168, branch agent/jw16-agx-stats4)

Executor: omp-studio-local CT, cross aarch64. Toolchain (measured 2026-10-04):
rustc 1.98.0, bindgen 0.71.1, libclang 19 (`LIBCLANG_PATH=/usr/lib/llvm-19/lib`),
aarch64-linux-gnu-gcc 12.2.0, pahole present, 16 jobs. Source: worktree of
`joshuaswarren/aurorasilicon-linux` (remote `origin` of `~/src/omarchy-linux`).

## Series identity

Branch `agent/jw16-agx-stats4` (replacement PR
https://github.com/aurora-silicon/linux/pull/168, draft, base `aurora-wip`),
tip `aebd7f529942`, 8 commits on `3bb0a6104a11`, every commit
`Signed-off-by: Joshua Warren <816217+joshuaswarren@users.noreply.github.com>`:

| # | commit | subject |
|---|---|---|
| 1 | `a6e06b47b49d` | drm/asahi: Add stats_export module parameter |
| 2 | `f19bcf1a1e37` | drm/asahi: stats: Add StatsSnapshot and firmware-stats decoder |
| 3 | `72d21d654534` | drm/asahi: Wire StatsSnapshot into StatsChannel and GpuManager |
| 4 | `e8c789f9f77b` | drm/asahi: Add sysfs shim for agx_stats |
| 5 | `1b02b0b40af3` | drm/asahi: Register agx_stats sysfs file on probe and unregister on drop |
| 6 | `c86f87f33104` | Documentation/ABI: Document sysfs-driver-asahi-agx-stats |
| 7 | `742f54c4855d` | drm/asahi: count completed submissions in agx_stats jobs |
| 8 | `aebd7f529942` | Documentation/ABI: make sysfs-driver-asahi-agx-stats match the output |

Differences from the original #157 series (branch `agent/jw16-agx-stats2`,
closed): commits 1-6 are the original six with Signed-off-by added; the shim
commit carries the build fix from birth (the original `sysfs.c` did not
compile — `DEVICE_ATTR_RO(agx_stats)` expands `.show` to `agx_stats_show`,
the code said `asahi_agx_stats_show`); commit 4 also puts the exported-helper
prototypes into `drivers/gpu/drm/asahi/sysfs.h` (checkpatch: externs in .c),
fixes the `show` signature alignment and the header-comment path; commit 7
gives `jobs` a real call site (`JobFence::command_complete` bumps the
snapshot through an `Arc` carried on the fence; `GpuManager` retains the
snapshot); commit 8 makes the ABI document match the printed keys exactly
(no `temperature_tmin`/`tmax`, u64 wrap semantics, host-side `jobs`).

## Static checks

- `checkpatch.pl --strict`: **0 errors, 0 checks on all eight commits**. The
  only residue is the advisory "added, moved or deleted file(s), does
  MAINTAINERS need updating?" on the three commits that add files; this tree
  has no MAINTAINERS entry for `drivers/gpu/drm/asahi` at all (pre-existing
  gap, not addressed in this series).
- `rustfmt --check` (kernel `.rustfmt.toml`): clean on every touched Rust
  file (`stats.rs`, `sysfs_exports.rs`, `channel.rs`, `driver.rs`, `gpu.rs`,
  `queue/mod.rs`, `asahi.rs`). Pre-existing base-tree drift in
  `fw/initdata.rs` is outside the series.

## Proof A1 — M2 `7.1.13-3-1` config (config of the M2's running kernel)

Source: macstudio `~/src/m2-headers/7.1.13-3-1-ARCH/.config`. Build-only
deltas (recorded): `CONFIG_DRM_ASAHI=m` (module artifact; the runtime stock
kernels are `=y`), `CONFIG_RUST=y` enabled explicitly, `CONFIG_LOCALVERSION
="-ARCH-agxstats"`, `CONFIG_LOCALVERSION_AUTO=n`, DEBUG_INFO/BTF off,
MODULE_SIG off. Everything else = the source config.

- Full build (vmlinux + all modules, W=1): rc=0.
- `asahi.ko`: rc=0, **zero W=1 warnings touching the series**.
- sha256 `62dac78d6292652f5fec19f29c06fa63710b88cfdc9139538a39863681d5d6f6`,
  326,384 bytes.
- vermagic `7.1.12-ARCH-agxstats+ SMP preempt mod_unload aarch64` (the `+` is
  setlocalversion's dirty marker on a detached partial-clone worktree).
- srcversion: absent (`CONFIG_MODULE_SRCVERSION_ALL=n` in this config); no
  `parm=` entries (Rust params declared without `permissions` are not
  exported) — the export-off arm needs `asahi.stats_export=0` on the kernel
  command line.
- Boot products for the window: `Image-m2` (29,657,600 bytes, sha256
  `82eb46a8acdc73931350961694ecab40a11ad372e39b69f3410e7614c1c3d182`),
  `modules-m2.tar.zst` (491,434,304 bytes, sha256
  `9203537a7fe562dab854b00e863cc946aedc3c3b0eb2489f6753e7ddc029b910`),
  `RELEASE` = `7.1.12-ARCH-agxstats+`.

## Proof B — aurora tree config (`AneUapiClean/config.used`, 7.1.12)

Same treatment (`config.used` carries `RUST_IS_AVAILABLE` but no
`CONFIG_RUST`; enabled):

- modules build rc=0; zero W=1 warnings touching the series (one pre-existing
  base-tree Rust warning, `unused import: alloc::flags::*` in
  `rust/kernel/soc/apple/rtkit.rs`, is outside the series).
- `asahi.ko` sha256
  `a469a9c138bc89cb3c4585d50d71148ce2329737936d506c5e675ff2384c546b`,
  326,408 bytes, vermagic `7.1.12-ARCH-agxstats+ SMP preempt mod_unload
  aarch64`.

## Host unit tests (both PASS on the stats4 tree, transcript
`results-unit-tests.log`)

- `test_agx_stats_contract.py` — struct mirror (13 fields, order and widths),
  producer-contract key coverage, `DEVICE_ATTR_RO`, `key value` line format,
  plus a jobs drift guard (`stats.note_job()` must be called from
  `queue/mod.rs`'s completion path).
- `test_busy_ns_mirror.c` — first-sample skip, out-of-order skip, zero-delta
  stability, monotonicity, busy <= timestamp, wrap semantics (now also the
  documented semantics).

## History note

The first prep round built the ORIGINAL `agent/jw16-agx-stats2` (tip
`7c5300d16efc`): it failed to compile (`agx_stats_show` name), and a fixed
variant was built on `agent/jw16-agx-stats3` (`19291a93f142`) with receipts
`asahi.ko` sha256 `bff3485a...` (M2 config) / `af928a32...` (aurora config).
Those receipts are superseded by this page; the logs remain in the private
artifacts directory. `agent/jw16-agx-stats2` could not be fast-forwarded to
fix Signed-off-by, so the series was rebuilt as `agent/jw16-agx-stats4` and
#157 was closed with a redirect comment.

## Limitations

Build proofs prove compile/link identity only, not load, bind, sysfs
visibility, or hardware behavior. Cross-built on rustc 1.98.0/clang-19/
gcc-12.2; the M2's running kernel was built with rustc 1.93.1/LLVM 21.
