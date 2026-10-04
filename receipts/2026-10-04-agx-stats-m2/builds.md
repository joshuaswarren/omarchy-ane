# Round 4 — readout layout fix (window B retry findings) and verified products

## What the retry measured and the root cause (proven, not inferred)

The retry window (artifacts `M2AgxStats/20261004T1500Z-agxstats-retry/`) ran
the full protocol on the round-3 kernel: GPU probed, agx_stats present, decode
digests bit-identical on all arms (`eee1cf9635d6d4eb`), reader overhead 0.27%,
export-off 0.32%. Three series defects remained, all rooted in ONE bug:

**`StatsSnapshot` was `#[repr(Rust)]`** — rustc legally reorders fields, and
it did: empirically with the build toolchain (rustc 1.98.0), the AtomicU64s
land at +0/+8/+16 and the AtomicU32s at +24.., while `sysfs.c` reads the
declared offsets (last_busy_ts +40, busy_ns +48, jobs +56). Every readout was
cross-aligned:

- `busy_ns` bounced in 2^32 steps and was not monotonic (12884910534 ->
  12884910482 -> 8589940317 -> ... in the S1 idle series) — the C readout
  assembled adjacent u32 fields, not the accumulated u64.
- `jobs` read 0 across 15 decode cells although the completion site FIRES
  (decode fences must signal for decode to complete at all — the decode
  finished) — the counter incremented at the Rust offset; the C readout
  looked at a zeroed wrong offset.
- On the off arm the "live pstate" and the zeroed counters were the same
  cross-read artifact, not a gating failure.

Fix commit `4189501b643f`: `#[repr(C)]` on `StatsSnapshot` (declaration order
binding), `BUILD_BUG_ON` offset/size asserts in `sysfs.c` (pstate==16,
last_busy_ts==40, busy_ns==48, jobs==56, sizeof==64) so future drift is a
compile error, and the ABI document states the monotonicity + layout
contract.

## Also fixed in the same round (commit `4189501b643f`)

- `stats_export=0` now prints exactly `unsupported` (the ABI promise):
  `asahi_sysfs_register()` takes the export flag; the show callback prints
  `unsupported` when the export is disabled. (Previously the file stayed
  visible with a zeroed key set.)

## Dart/dcp display lines: out of series scope (stated plainly)

The 3 apple-dart -EPROBE_DEFER and 3 dcp supplier-miss lines are display-path
and pre-date the series: the series diff (base `3bb0a6104a11` -> head) touches
only `drivers/gpu/drm/asahi/*` and the ABI document (429 added lines, zero
dcp/dart/display files). They come from the 7.1.12 aurora base pairing with
the stock 7.1.13-era device tree supplied by the untouched boot.bin — a
base-tree/DT-skew matter for the aurora maintainers, not this PR.

## The boot kernel (verified)

Stock M2 config, ONLY intentional delta `CONFIG_LOCALVERSION
="-ARCH-agxstats"`; `CONFIG_DRM_ASAHI=y` (stock). Config diff = tree skew +
toolchain-reported versions, recorded in the build log.

| product | sha256 | bytes |
|---|---|---|
| `Image-m2` | `04d32601d0d3cc61b20f912c16b06a0a89a6a3f5d020d54dd7e59d9b803a2aca` | 36,277,568 |
| `System.map` | `40ef17a7cd6b31f0861c5e0f87dc7992d8692059b194da71fffe2909ad0e481a` | see SHA256SUMS-stage |
| `modules-m2.tar.zst` | see SHA256SUMS-stage (rebuilt) | — |
| `kernel.config` | `3f2911ca...`-lineage | see SHA256SUMS-stage |
| `RELEASE` | `c4e3169346cbec43db91f54585e45164fd0bb0e581c3de3fc2e1d5d422f535bb` | 21 |

`AGX_VERIFY_OK 2026-10-04T17:09:47Z` (verify-boot-product.sh): built-in
symbols `T asahi_sysfs_register` / `t agx_stats_show` in System.map, 34 AGX
driver string hits + agx_stats symbols in the Image, tarball vermagic match,
checksums verified. Series: W=1 zero warnings, checkpatch --strict 0/0,
rustfmt clean. Unit tests extended (repr(C) guard with negative test,
BUILD_BUG_ON assert guard, unsupported-string guard) — PASS.

## Series state

`agent/jw16-agx-stats4` fast-forwarded to `4189501b643f` = PR #168 head
(draft): 13 commits = 8 from round 2 + 5 Signed-off-by fix commits
(`bbfb96f309c8`, `01d19b8a4f55`, `91c24dc11b09`, `dbc8b48aeb06`,
`8ca5289bec3a` from round 3, `4189501b643f` from this round).

## Module-receipt proof (aurora config, round 4 code)

`asahi.ko` (config.used, `CONFIG_DRM_ASAHI=m`): sha256
`c0beffdad7caa43f6047f687f1cde98932746cfd19acc464dc317ad8b2f2840f`,
24,221,736 bytes — a REAL module now (26 GPU-driver string hits, agx_stats
symbols present) versus the round-2 shim-only 326 KB artifact. The
`stats_export` parameter appears in `parm=` (param registration returns with
the crate linked in). vermagic `7.1.12-ARCH-agxstats+ SMP preempt mod_unload
aarch64`. vmlinux rc=0, modules rc=0, zero series warnings.

---

# Round 3 — boot-kernel fix (window B boot 1 failure) and verified products

## What failed on the M2 and why (root cause chain)

1. `asahi-y := sysfs.o` (original series) made Kbuild build asahi.o as a
   composite whose only part was sysfs.o. The composite link overwrote the
   object rustc emits for the crate root (both named asahi.o): every earlier
   "asahi.ko" was the sysfs shim ALONE, and — the part my earlier proofs
   missed — the Rust driver crate was never compiled at all in any config.
2. Window B boot 1 ran this as a MODULE on the stock kernel (built-in): no
   AGX driver, no GPU, display-dart -16 probe failures, dcp missing.
3. Fixing the composite (`asahi-y := asahi_core.o sysfs.o`, crate root
   renamed `asahi_core.rs`) exposed the real state of the series: it had
   never compiled its own driver code. Four further defects surfaced and are
   fixed, each by a Signed-off-by commit on `agent/jw16-agx-stats4`
   (fast-forward pushes only):

| commit | fix |
|---|---|
| `bbfb96f309c8` | Makefile composite: crate + shim actually linked |
| `01d19b8a4f55` | `Arc::new` returns Result — propagate with `?` |
| `91c24dc11b09` | `StatsMsg` is a versions-macro type; decode moved into `StatsChannel::poll` (versioned context), snapshot updated inline |
| `dbc8b48aeb06` | variant patterns qualified `StatsMsg::ver::...` |
| `8ca5289bec3a` | the pointer bridge: Rust wrote its own `SNAPSHOT_PTR`, the C `show` read a never-written static — C static is now the single storage, written via `asahi_stats_set_snapshot_ptr()` |

## The boot kernel (verified)

- Stock M2 config (`7.1.13-3-1-ARCH` headers `.config`), ONLY intentional
  delta `CONFIG_LOCALVERSION="-ARCH-agxstats"`. `CONFIG_DRM_ASAHI=y` (stock
  value, built-in like the running kernel). Everything else that differs in
  the final `.config` is 7.1.13-vs-7.1.12 tree skew (options that do not
  exist in this tree: ARM64_LSUI, RELR, ML_DSA, MT7932_FULLMAC,
  DOCKCHANNEL, T8140_AOP audio; APPLE_TUNABLE m->y is a tree difference) or
  toolchain-reported versions (AS/BINDGEN/PAHOLE/GCC; CT pahole 1.24 vs
  distro 1.31). The full diff is in the build log.
- `RELEASE` = `7.1.12-ARCH-agxstats+`, `CONFIG_DRM_ASAHI=y` verified in the
  built `.config`.

| product | sha256 | bytes |
|---|---|---|
| `Image-m2` | `025609d95efa6885b3b86c8a37d19d85ba007c908d540f567bf0ebbcfbc1509f` | 36,276,736 |
| `System.map` | `48111270cb8215ca74912c31c00b5ac44dcdd1cfdc5bd59805e7729e1b3b481f` | 6,136,325 |
| `modules-m2.tar.zst` | `e4bc67c4fc21109e308a979e786bd15684a13afaf6df91b483f2676f558f16fd` | 498,405,660 |
| `kernel.config` | see SHA256SUMS-stage | 239,677 |
| `RELEASE` | `c4e3169346cbec43db91f54585e45164fd0bb0e581c3de3fc2e1d5d422f535bb` | 21 |

## Verification (offline, `verify-boot-product.sh`, receipt
`verify-boot-product.txt`: AGX_VERIFY_OK)

- `System.map`: `T asahi_sysfs_register`, `t agx_stats_show`,
  `B asahi_stats_snapshot_ptr` (built-in; `asahi_probe` is static/inlined in
  this tree, so presence is asserted by the strings check instead).
- Image strings: 34 GPU-driver hits (`MMU: map:`, `MMU: KernelMapping`, ...),
  `agx_stats_show` present.
- Modules tarball: no asahi.ko (built-in build — correct), spot module
  vermagic matches the release string.
- W=1: zero warnings in the series' files (base-tree coda/usbip/rtkit
  warnings are pre-existing and untouched by the series).
- checkpatch --strict: 0 errors, 0 checks on the five new commits; rustfmt
  clean on all touched files.
- Unit tests: contract test (struct mirror, producer keys, RO attr, jobs
  call-site guard) + busy_ns mirror — PASS against the fixed tree.
- The `agx_stats_show` + `B asahi_stats_snapshot_ptr` pair in System.map is
  the offline proof that the sysfs bridge (C static as single storage, Rust
  writes through `asahi_stats_set_snapshot_ptr`) is linked in.

## Gate hardening (this failure class cannot reach an install again)

- `verify-boot-product.sh` (CT, offline): checksums, release string, built-in
  symbols in System.map, driver + agx_stats strings in the (decompressed if
  needed) Image, tarball vermagic spot-check. Writes `verify-boot-product.txt`.
- `install-test-kernel.sh` (device): REFUSES unless the stage dir carries the
  `AGX_VERIFY_OK` receipt and the staged `Image-m2` sha256 matches the
  receipt bit-for-bit.
