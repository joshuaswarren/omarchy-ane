# omarchy-ane 0.4.1: release receipt

The release commit moves the CHANGELOG to `## 0.4.1 (2026-10-02)` on top of
main `6608721` (#87). It also restores the 0.1.0 to 0.4.0 CHANGELOG sections
that #57 removed, fixes two README facts, and updates the `dkms.conf`
source-tree comment. The project keeps no version number in the tree: the
recipe passes the package version to `dkms.conf` (`@PKGVER@`), as in 0.4.0.

## What changed since v0.4.0

`git log --oneline v0.4.0..6608721`: PRs #53 to #87, except #79 and #86
(open) and #83 (an issue).
The CHANGELOG 0.4.1 section lists them. The drivers changed only by
`ane_stats` (#57, #59, #63, #82):
`git log --oneline v0.4.0..6608721 -- ane dkms.conf` gives 8 commits, all
from those PRs. libane changed only by #55 (`libane/ane_m2.c`, the pad
members). `git diff v0.4.0..6608721 -- ane/src/uapi libane/ane.h` is empty.

## The bytes that the hardware gates ran

The T8103 and T6001 gates ran on `cbb1beb` (main after #82).
`git diff cbb1beb..6608721 -- ane libane packaging/omarchy-ane-smoke fixtures
dkms.conf` is 0 lines, so every hardware gate result on `cbb1beb` holds for
this tree. The release commit changes `dkms.conf` only in its comment (lines
12-15).

## Host gate

On x86_64 (gcc 12.2.0, Python 3.11.2, pytest 9.1.1), on the release tree
before the commit (this README excluded), with the steps of
`.github/workflows/dt-overlays.yml` and the kernel tree's dtc
(`DTC 1.7.2-g53373d13`) first in `PATH`. Every step exited 0:

| Step | Result |
| --- | --- |
| `tools/asahi-dtbs OUT` | 29 board DTBs |
| `tools/validate_ane_soc.py data/ane-soc/*.json tests/fixtures/ane-soc/*.json` | 13 files ok (the twelve tables and the t9999 fixture) |
| `tools/test_validate_ane_soc.py`, `tools/gen_coverage_table.py --check` | ok, ok |
| `make -C libane libane`, `make -C tools tools` | rc 0 (`-Wall -Werror -Wextra`) |
| `make check` | SELF-CHECK PASS, IOCTL-CHECK PASS, `test_ane_stats` (model check: 90 interleavings, 0 failures; stress: 1000 jobs, busy_ns equals the consumed slot union) |
| `tools/ane-selfcheck fixtures/h14-anec` | SELF-CHECK PASS |
| `test_ane_dt`, `test_ane_firmware_fetch`, `test_ane_m2` (with `ANE_DTBS`), `test_t8112_kit` | ok |
| `test_ane_overlays` (with `ANE_DTBS`) | ok (9 overlays, 5 data-only, 32 applications) |
| `test_promotion_check`, `test_ane_smoke`, `test_ane_probe` | ok |
| `tools/h14_boot_regression.c` | PASSED: 127 checks, 0 failures |
| `test_promote_chip`, `test_promote_from_verdict` | ok (8 chips), ok |
| `python3 -m pytest -q tests tools` | 51 passed, 1 skipped |
| `voice_lint.py --mode article README.md` | fail=0 before and after (one Flesch warning, also on main) |

## Package recipe (omarchy-pkgs `omarchy-ane-dkms`)

The 0.4.0 recipe does not build 0.4.1. Both modules now compile
`ane/ane_stats_show.c` and include `ane/include/ane_stats.h`, and the 0.4.0
`prepare()` copies neither. These are the 0.4.1 lines:

```sh
pkgver=0.4.1
pkgrel=1
sha256sums=('<sha256 of archive/refs/tags/v0.4.1.tar.gz>')

prepare() {
  local src="$_srcname-$pkgver" dst="dkms/$_srcname-$pkgver"
  rm -rf dkms
  install -d "$dst/ane"
  cp -r "$src/dkms.conf" "$dst/"
  cp -r "$src/ane/Makefile" "$src/ane/ane_stats_show.c" "$src/ane/include" \
    "$src/ane/src" "$src/ane/t6021" "$dst/ane/"
  sed -i "s/@PKGVER@/$pkgver/" "$dst/dkms.conf"
}

package() {
  cd "$_srcname-$pkgver"
  install -d "$pkgdir/usr/src"
  cp -r --no-preserve=ownership "$srcdir/dkms/$_srcname-$pkgver" "$pkgdir/usr/src/"

  install -Dm755 -t "$pkgdir/usr/bin" \
    packaging/omarchy-ane-check packaging/omarchy-ane-dt \
    packaging/omarchy-ane-firmware-fetch packaging/omarchy-ane-smoke
  install -Dm755 tools/omarchy-ane-probe "$pkgdir/usr/bin/omarchy-ane-probe"
  # omarchy-ane-smoke runs omarchy-ane-run from its own directory with these programs.
  install -Dm755 tools/ane-run "$pkgdir/usr/bin/omarchy-ane-run"
  install -Dm644 fixtures/h14-anec/add/program-0.anec \
    "$pkgdir/usr/share/omarchy-ane/fixtures/h14-anec/add/program-0.anec"
  install -Dm644 fixtures/h13-anec/add/program-0.anec \
    "$pkgdir/usr/share/omarchy-ane/fixtures/h13-anec/add/program-0.anec"
  install -Dm644 packaging/update-m1n1-dtbs "$pkgdir/usr/lib/omarchy-ane/update-m1n1-dtbs"
  install -Dm644 -t "$pkgdir/usr/share/libalpm/hooks" \
    packaging/90-omarchy-ane-dt.hook packaging/90-omarchy-ane-dt-remove.hook \
    packaging/90-omarchy-ane-firmware.hook
  # Overlays to /usr/share/omarchy-platform/dtb-overlays/PREFIX/omarchy-NAME.dtbo,
  # and data/ane-soc/SOC.json to /usr/share/omarchy-ane/soc/ (twelve in 0.4.1).
  # The data-only overlays compile as a check and are never installed.
  packaging/build-dtbo "$pkgdir"

  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
```

The other recipe lines (`depends`, `makedepends`, `provides`, `build()`,
`check()`) do not need a change. Do not add the separate
`install -Dm644 -t "$pkgdir/usr/share/omarchy-ane/soc" data/ane-soc/*.json`
line from `docs/ane-probe.md`. `build-dtbo` already installs the tables, and
on a release with no table that glob does not expand, so `install` fails.

### Package checks on the host

From a tarball of the release tree (`omarchy-ane-0.4.1/`, as the tag
tarball):

- DKMS tree: the `dkms.conf` `MAKE[0]` commands (`ANE_VERSION=0.4.1`,
  `ARCH=arm64`, `CROSS_COMPILE=aarch64-linux-gnu-`, gcc 12.2.0) against an
  arm64 7.1.13 module build tree with `CONFIG_DRM_ACCEL=y`. All variants
  build in the same directory.

  | DKMS tree | Result |
  | --- | --- |
  | the whole source tree | rc 0; `ane.ko` `b4e70f84…`, `ane_t6021.ko` `04247ba2…` |
  | the 0.4.0 `prepare()` copy list | fails: `ane_stats.h: No such file or directory`, `No rule to make target 'ane_stats_show.o'` |
  | the 0.4.1 `prepare()` copy list above | rc 0, 0 warnings; both `.ko` byte-identical to the whole-tree build |

  `modinfo`: version `0.4.1` and the `stats` parameter in both modules;
  `ane.ko` srcversion `389B601E4710B9AFCFAE58C`. The T8103 and T6001 gates
  loaded an `ane` with that srcversion. The tree's vermagic is not a
  distribution kernel's, so these `.ko` files only prove that the tree is
  complete. They are not a module to load.

- `package()`: the lines above, into a scratch `$pkgdir`, exit 0. They install
  34 files: the twelve `usr/share/omarchy-ane/soc/*.json` tables, 9 `.dtbo`
  files (no data-only overlay), and an H13 fixture that is byte-equal to the
  source. `build-dtbo` prints `compiled, not installed` for the five data-only
  overlays (T6040, T6041, T8132, T8140, T8152). The installed
  `omarchy-ane-probe` runs from `usr/bin` (rc 0). With a T8132 device tree
  compatible, it reads `usr/share/omarchy-ane/soc/t8132.json` and compares 16
  fields.

## Hardware gates

- T8103 (M1) and T6001 (M1 Max): the `ane_stats` acceptance and the H13
  smoke (20 of 20 bit-exact, golden `5ad7eccd…`) passed on `cbb1beb`, with
  `ane.ko` built from that source, not from a package. The release notes
  give the numbers.
- T6021 (M2 Max): pending at the time of this commit. The release notes give
  the result.
