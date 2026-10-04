# Build proofs — agx_stats series (PR #157)

Executor: omp-studio-local CT, cross aarch64. Toolchain (measured 2026-10-04):
rustc 1.98.0, bindgen 0.71.1, libclang 19 (`LIBCLANG_PATH=/usr/lib/llvm-19/lib`),
aarch64-linux-gnu-gcc 12.2.0, pahole present, 16 jobs. Source: worktree of
`joshuaswarren/aurorasilicon-linux` (remote `origin` of `~/src/omarchy-linux`).

Source identity:

| tree | commit | note |
|---|---|---|
| `agent/jw16-agx-stats2` (PR #157 head) | `7c5300d16efc` | does NOT compile (below) |
| `agent/jw16-agx-stats3` (fix branch, pushed) | `19291a93f142` | all builds below |

Config identities (both shipped with build-only deltas, recorded):
`CONFIG_DRM_ASAHI=m` (module artifact for receipts; runtime stock kernels are
`=y`), `CONFIG_LOCALVERSION="-ARCH-agxstats"`, `CONFIG_LOCALVERSION_AUTO=n`,
DEBUG_INFO/BTF off, MODULE_SIG off. Everything else = the source config.

## Finding first: PR #157 does not compile as shipped

`sysfs.c:94` — `DEVICE_ATTR_RO(agx_stats)` expands `.show = agx_stats_show`,
but the callback is named `asahi_agx_stats_show`:

```
sysfs.c:94:23: error: 'agx_stats_show' undeclared here (not in a function)
```

The first M2-config module build failed on exactly this (fixup-m2.log in the
private artifacts). Additional W=1 findings on the original: missing prototypes
for `asahi_sysfs_register/unregister`, `show` unused; checkpatch: **Missing
Signed-off-by on all six commits**; `sysfs.c` and the ABI doc lack trailing
newlines. Fix branch `agent/jw16-agx-stats3` (one commit, Signed-off-by
Joshua Warren) renames the callback, adds the prototypes, adds the newlines.
Main can re-point PR #157's head via REST, or the branch owner can apply the
same three changes.

## Proof A1 — M2 `7.1.13-3-1` config (config of the M2's running kernel)

Source: macstudio `~/src/m2-headers/7.1.13-3-1-ARCH/.config` (the config that
matches the M2's running kernel; proven by months of matching-vermagic ane.ko
builds). `CONFIG_RUST=y`, `CONFIG_DRM_ASAHI=y` in the source config — the
on-device module-only gate is impossible; this build is the receipts artifact
plus the boot kernel source.

- Full build (vmlinux + all modules, W=1): rc=0.
- `asahi.ko` (fix branch): rc=0, **zero W=1 warnings in
  drivers/gpu/drm/asahi** (fresh targeted recompile after the fix).
- sha256 `bff3485a66f3fa7b1f2f2d32d91e66a9b201babfc868b8bb57ab594a3ba1f2a8`,
  326,576 bytes.
- vermagic `7.1.12-ARCH-agxstats+ SMP preempt mod_unload aarch64` (the `+` is
  setlocalversion's dirty marker on a detached partial-clone worktree).
- srcversion: absent — `CONFIG_MODULE_SRCVERSION_ALL=n` in this config.
- module parameters: NO `parm=` entries at all. Rust params declared without
  `permissions` are not exported — `/sys/module/asahi/parameters/stats_export`
  will not exist; the export-off arm needs `asahi.stats_export=0` on the kernel
  command line (the protocol's second boot).
- Determinism: an independent recompile of `sysfs.c` + relink reproduced the
  byte-identical `.ko` (same sha256).
- Boot products for the window: `arch/arm64/boot/Image` and the modules tree
  from the same build (release `7.1.12-ARCH-agxstats+`).

## Proof B — aurora tree config (`AneUapiClean/config.used`, 7.1.12)

`config.used` carries `RUST_IS_AVAILABLE=y` but no `CONFIG_RUST` line, so
`CONFIG_RUST` defaults to n and `DRM_ASAHI` (depends on RUST) is invisible.
Enabling `CONFIG_RUST=y` + `CONFIG_DRM_ASAHI=m` (fixup recorded in
`fixup-aurora.log`), then:

- modules build rc=0; `asahi.ko` produced.
- sha256 `af928a3200a311471ac23524fc6e2059793b85a107f1f4761ca470d8760fa403`,
  326,600 bytes.
- vermagic `7.1.12-ARCH-agxstats+ SMP preempt mod_unload aarch64`; no
  srcversion, no `parm=` entries (same config gaps as proof A1).
- Zero W=1 warnings in the series' files. One pre-existing base-tree Rust
  warning (`unused import: alloc::flags::*` in `rust/kernel/soc/apple/
  rtkit.rs`) is outside the series.

## Host unit tests (both PASS, transcript `results-unit-tests.log`)

- `test_agx_stats_contract.py` — `stats.rs` `StatsSnapshot` and the `sysfs.c`
  mirror struct match field-for-field (13 fields, order and widths), the sysfs
  output covers every coreglass producer-contract key, the attribute is
  `DEVICE_ATTR_RO` (0444), every key prints as one `key value` line.
- `test_busy_ns_mirror.c` — first-sample skip, out-of-order skip, zero-delta
  stability, monotonicity, busy <= timestamp, and wrap semantics (the ABI
  doc's "saturates at u64::MAX" claim is not what the code does — reported).

## Limitations

Build proofs do not prove load, bind, sysfs visibility, or any hardware
behavior. The Rust side compiled under rustc 1.98.0; the M2's running kernel
was built with 1.93.1 — no version gate was exercised beyond the kernel's own
`CONFIG_RUSTC_VERSION` checks (min 1.85).
