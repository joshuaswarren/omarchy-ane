# omarchy-ane 0.4.2: release receipt

The release commit moves the CHANGELOG to `## 0.4.2 (2026-10-03)` on top of
main `dc174cd` (#104) and fixes one README sentence about the chips after the
M2. The project keeps no version number in the tree: the recipe passes the
package version to `dkms.conf` (`@PKGVER@`), as in 0.4.1.

## What changed since v0.4.1

The tag v0.4.1 is on `4f01bb3`, a branch of `6608721`. So
`git log --first-parent v0.4.1..dc174cd` also holds #89, which v0.4.1 does
not contain. The merges: #89 and #91 to #105. #89 and #91 change only the
promotion workflow, `tools/promote_from_verdict.py` (one import) and the
promotion tests; the CHANGELOG names every other one.
`git diff v0.4.1..dc174cd -- ane/src/uapi libane/ane.h libane/ane_m2.h` is
empty: the ioctl interface does not change.

One entry is a placeholder for an open PR. It starts with `PLACEHOLDER`, so
`grep -n PLACEHOLDER CHANGELOG.md` finds it: #106, the T6021 0.4.2 gate
receipt. Remove the line if #106 does not merge before the tag.

## The bytes that the hardware gates ran

A DKMS-tree build of this tree (below) gives `ane.ko` srcversion
`B4AE691E569A2BBD37E18E3` and `ane_t6021.ko` srcversion
`59494CBC56F28ED8D1122C6`. srcversion is a checksum of the module sources.

- `ane.ko`, T8103 (one M1 laptop) and T6001 (one M1 Max laptop): the
  `map_batch` runs built `ane.ko` and the test libane from the #104 head
  `13c684a`, and loaded srcversion `B4AE691E569A2BBD37E18E3`.
  `git diff 13c684a..dc174cd` changes only
  `receipts/2026-10-03-t6021-powerdown/README.md`, so `ane/`, `libane/`,
  `packaging/`, `tools/`, `fixtures/` and `dkms.conf` are the bytes of this
  release. The autosuspend runs before them (#95 head `198db99`, srcversion
  `B94A022ECB6F0FF27D17BB8`) cover the power behavior: #104 changes only the
  map path.
- `ane_t6021.ko`, T6021 (one M2 Max laptop): the run on `5a457a3` loaded
  srcversion `59494CBC56F28ED8D1122C6`.
  `git diff 5a457a3..dc174cd -- ane/t6021 ane/include ane/ane_stats_show.c
  libane dkms.conf packaging/omarchy-ane-smoke fixtures` is empty.

## Host gate

On x86_64 (gcc 12.2.0, Python 3.11.2, pytest 9.1.1), on the release tree
before the commit (this README excluded), with the steps of
`.github/workflows/dt-overlays.yml` in order and the kernel tree's dtc
(`DTC 1.7.2-g53373d13`) first in `PATH`. Every step exited 0:

| Step | Result |
| --- | --- |
| `tools/validate_ane_soc.py data/ane-soc/*.json tests/fixtures/ane-soc/*.json` | 13 files ok |
| `tools/test_validate_ane_soc.py`, `tools/gen_coverage_table.py --check` | ok, ok |
| `tools/asahi-dtbs OUT` | 29 board DTBs |
| `make -C libane libane`, `make -C tools tools` | rc 0 (`-Wall -Werror -Wextra`) |
| `make check` | SELF-CHECK PASS, IOCTL-CHECK PASS, `test_ane_stats` (model check: 90 interleavings, 0 failures; stress: 1000 jobs, busy_ns equals the consumed slot union) |
| `tools/ane-selfcheck fixtures/h14-anec` | SELF-CHECK PASS |
| `test_ane_dt`, `test_ane_firmware_fetch`, `test_ane_m2` (with `ANE_DTBS`), `test_t8112_kit` | ok |
| `test_ane_overlays` (with `ANE_DTBS`) | ok (9 overlays, 5 data-only, 32 applications) |
| `test_promotion_check`, `test_ane_intree`, `test_aurora_dt`, `test_promote_from_verdict` | ok |
| `test_ane_smoke`, `test_ane_probe` | ok |
| `test_dkms_exclusive` | no dkms on the host, nothing checked; with `DKMS=` set to dell/dkms v3.4.3 (the Arch `dkms` 3.4.3-2), ok: `CONFIG_DRM_ACCEL_ANE=m` and `=y` exit 77 without make, and an unset option builds |
| `tools/h14_boot_regression.c` | PASSED: 127 checks, 0 failures |
| `test_promote_chip` (with `ANE_DTBS`) | ok (8 chips) |
| `python3 -m pytest -q tests tools` | 58 passed, 1 skipped |
| `voice_lint.py --mode article README.md` | fail=0 before and after (one Flesch warning, also on main) |

The same steps on a `git archive` of the tree (no `.git`, as the tag
tarball) also all exit 0, `test_promote_chip` and `test_promote_from_verdict`
included.

## Package recipe (omarchy-pkgs `omarchy-ane-dkms`)

The 0.4.1 recipe lines (`receipts/2026-10-03-omarchy-ane-0.4.1/README.md`)
build 0.4.2 without a change. Only these lines change:

```sh
pkgver=0.4.2
pkgrel=1
sha256sums=('<sha256 of archive/refs/tags/v0.4.2.tar.gz>')
```

No packaged file was added or removed since v0.4.1. `ane/h16` is not part of
the DKMS tree or the package. `dkms.conf` now sets `BUILD_EXCLUSIVE_CONFIG`,
which dkms 3.4.3 honours (the `test_dkms_exclusive` row above).

### Package checks on the host

From `git archive --prefix=omarchy-ane-0.4.2/` of the release tree (the
layout of the tag tarball), with the method of the 0.4.1 receipt (the
`dkms.conf` `MAKE[0]` commands, `ANE_VERSION=0.4.2`, `ARCH=arm64`,
`CROSS_COMPILE=aarch64-linux-gnu-`, gcc 12.2.0, an arm64 7.1.13 module build
tree with `CONFIG_DRM_ACCEL=y`):

| DKMS tree | Result |
| --- | --- |
| the whole source tree | rc 0; `ane.ko` `1c2f8b58…`, `ane_t6021.ko` `db294614…` |
| the 0.4.1 `prepare()` copy list | rc 0; both `.ko` byte-identical to the whole-tree build |
| the 0.4.0 `prepare()` copy list | fails: `ane_stats.h: No such file or directory` |

`modinfo`: version `0.4.2` in both modules; `autosuspend_ms`, `map_batch` and
`stats` in `ane`; `dyn_pg` and `stats` in `ane_t6021`. The tree's vermagic is
not a distribution kernel's, so these `.ko` files only prove that the file
list is complete. They are not a module to load.

The 0.4.1 `package()` lines, into a scratch `$pkgdir`, exit 0 and install 34
files, including the twelve `usr/share/omarchy-ane/soc/*.json` tables and an
H13 fixture that is byte-equal to the source.

## Hardware gates

- T8103 (M1) and T6001 (M1 Max): the `map_batch` runs on `13c684a` (the
  bytes of this release, above) passed: smoke 20/20 bit-exact, whole encoder
  bit-exact with the job count exact, 10-minute stress with no error. The
  autosuspend runs on `198db99` passed before them.
- T6021 (M2 Max): passed at the default parameters on `5a457a3`, with the
  `ane_t6021`, libane and tools of this release.
