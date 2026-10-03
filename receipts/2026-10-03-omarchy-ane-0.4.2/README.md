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

On x86_64 (gcc 12.2.0, Python 3.11.2, pytest 9.1.1), with the steps of
`.github/workflows/dt-overlays.yml` in order and the kernel tree's dtc
(`DTC 1.7.2-g53373d13`) first in `PATH`, on a fresh `git archive` of
`f9ef8be`. That commit is this release except for the last edit of this
receipt: `git diff f9ef8be..<release commit> -- .
':!receipts/2026-10-03-omarchy-ane-0.4.2'` is empty. So the CHANGELOG,
README and code of the release are the gated bytes. Every step exited 0
(earlier runs on `d4aa669` and on `40f3317` gave the same results):

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

The archive has no `.git`, like the tag tarball, so `test_promote_chip` and
`test_promote_from_verdict` passing there shows the #92 change.

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

## Hardware results (MEASURED by the hardware lanes)

One machine per chip ran each lane. The lane records are in the private lab
notebook; this section copies their numbers. Nothing here was measured again
for this receipt. The design receipts that the CHANGELOG also cites
(`2026-10-03-ane-autosuspend`, `2026-10-03-ane-iommu-batch`,
`2026-10-03-t6021-dynpg`) were written before these runs, so their "not
verified" sections predate this data.

### Autosuspend (#95): `198db99`, `ane.ko` srcversion `B94A022ECB6F0FF27D17BB8`

Idle power in 5-minute blocks at 1 Hz, arms A (module removed), B (ANE held
on) and C (`autosuspend_ms` 1500). T8103 gives the median of Total System
Power per block; T6001 gives the mean of `total_uW` per block.

| | T8103 (one M1 laptop) | T6001 (one M1 Max laptop) |
| --- | --- | --- |
| A, module removed | 3315.4 / 3307.7 mW | 12.811 W |
| B, ANE held on | 3455.9 / 3434.6 mW | 20.451 / 14.910 W |
| C, autosuspend | 3327.8 / 3283.2 mW | 12.808 / 12.804 W |
| B minus C, per block | 107 to 173 mW (block medians) | 2.10 to 7.65 W (block means) |
| mean of the block means, B / C | 3469 / 3313 mW | 17.68 / 12.81 W (from the rows above) |
| C runtime state | suspended in 300 of 300 samples per block | suspended in 300 of 300 samples per block |
| first open after 5 s idle | median 330 µs (warm: 10 µs) | 0.35 ms |
| whole encoder | 40 blocks of 16 calls bit-exact; per-call engine time min-of-min 136.885 ms in both arms | 40 blocks of 16 calls, one output digest; jobs +640 exact |
| smoke from the suspended state | 20/20 bit-exact | 20 runs, each 20/20 bit-exact |
| stress, 10 min, random idle 0-4 s | 295 cycles, 0 failures | 304 cycles, 0 failures |

### `map_batch` (#104): `13c684a`, `ane.ko` srcversion `B4AE691E569A2BBD37E18E3`

The whole Parakeet encoder program, 10 cold opens per arm, with the test
libane built from the same tree (`tools/ane_cold_start.py --trace`).

| | T8103 (one M1 laptop) | T6001 (one M1 Max laptop) |
| --- | --- | --- |
| BO_INIT, `map_batch=1` | 22.772 ms median (22.727 min) | 24.75 ms min |
| BO_INIT, `map_batch=0` (per page) | 179.852 ms median (179.778 min) | 232.74 ms min |
| BO_INIT, `map_batch=1`, second arm | 22.757 ms median (22.725 min) | 24.59 ms min |
| whole open, `map_batch` 1 / 0 / 1 (second arm) | 585.288 / 758.617 / 593.406 ms median | 574.25 / 787.89 / 571.44 ms min |
| whole open, `map_batch=0` minus `map_batch=1` | 165.2 to 173.3 ms (medians) | 213.6 to 216.5 ms (minimums) |
| release (close), 1 / 0 | 177.830 / 177.977 ms median | 241.40 / 241.20 ms min |
| whole encoder, 20 blocks of 16 per arm | bit-exact, jobs 16 per block; per-call min 136.599 / 136.613 ms | bit-exact, jobs +640 exact; block min 7431 / 7641 ms |
| smoke with `map_batch=1` | 20/20 bit-exact | 20 runs, each 20/20 bit-exact |
| stress, 10 min, `map_batch` flipped every 60 s | 223 cycles, 0 failures | 236 cycles, 0 failures |
| new kernel error lines | 0 | 0 |

On T6001 the protocol's encoder bar (min-of-min within 1 % between the arms)
failed at 2.748 %, because `map_batch=1` is faster; the lane accepted the
change and asked for the bar to be reviewed.

### T6021 (one M2 Max laptop): `5a457a3`, `ane_t6021` srcversion `59494CBC56F28ED8D1122C6`

- Default parameters: boot and bind; 8 `ane` lines at the default log level
  (6 info, 2 warning) and no `ANERD`, `ANEWR`, `ps probe` or emergency line;
  one license and one description in `modinfo`; smoke 20/20 bit-exact; nine
  gate ops; whole encoder bit-exact at 254.306 ms min-of-min (+0.004 %
  against v0.4.1); `ane_stats` items 1-4 (stats cost +0.017 %).
- `dyn_pg=1`: the firmware answered `SET_DYNAMIC_POWERGATE` with status 0.
  `ane_pg_state` read 0x3ff (ACTUAL 0xf) in all seven words before the first
  job, 30 s after the last gate, and in every 10 s sample of three idle
  blocks. Smoke, gates, job counts, encoder time (+0.005 %) and output bits
  matched the defaults.
- `dyn_pg=1 boot_prevent_nap=0`: the firmware booted, CONFIG_GET timed out
  (-110), and probe failed with -110, so no device registered.
