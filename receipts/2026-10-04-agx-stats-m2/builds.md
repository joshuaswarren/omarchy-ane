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
