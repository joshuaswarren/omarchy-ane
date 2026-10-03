# omarchy-ane 0.4.4: release receipt

0.4.4 is a cleanup release on top of main `89c2088` (#117). It changes no
driver or libane code. It fixes the packaging defects of v0.4.3, including
the promotion release step, which #117 left without a version heading. The
project keeps no version number in the tree: the recipe passes the package
version to `dkms.conf` (`@PKGVER@`).

## Why 0.4.4 supersedes v0.4.3

The promotion workflow tagged v0.4.3 on `0477f72`, the merge of the T6000
promotion, before its CHANGELOG move (`fd97719`). So the v0.4.3 tarball
(sha256 `dcd5442e…`) has no 0.4.3 section. It also holds
`tools/__pycache__/aurora_dt.cpython-312.pyc`, a README that names only
T8103, T6001 and T6021 as tested, T6000 and T6020 overlay headers that say
untested and opt-in, a libane entry written before the hardware runs, and a
`tools/test_promote_chip.py` that fails (a T6002 promote and revert reordered
the opt-in list of `packaging/omarchy-ane-check`). The 0.4.4 tarball carries
the 0.4.3 and 0.4.4 notes, and the tag of this release goes on its release
commit, made by hand, not by the promotion workflow.

## What changed since v0.4.3

`git log --first-parent v0.4.3..89c2088`: `fd97719` (the 0.4.3 CHANGELOG move)
and #117. The release PR adds two code fixes to the promotion tools:

- `release()` in `tools/promote_from_verdict.py` writes the `## X.Y.Z (date)`
  heading when Unreleased moves. As merged in #117 it rebuilt the file as
  `## Unreleased` plus the old entries, so the heading never reached the
  file. A run of the real `release()` on this CHANGELOG, with git and GitHub
  stubbed, gave the headings `Unreleased`, `0.4.3`, `0.4.2` before the fix
  and `Unreleased`, `0.4.4`, `0.4.3` after it. `test_promote_from_verdict`
  now seeds an entry under Unreleased and compares the moved text byte for
  byte; it fails with the #117 `release()` and passes with the fix.
- `edit_changelog()` in `tools/promote_chip.py` keeps a blank line between a
  new Unreleased entry and the released heading below it, which is where an
  empty Unreleased sits after a release. Before, the entry went directly
  above that heading. `test_promote_chip` checks the shape after every flip;
  it failed before the fix.

## The bytes that the hardware gates ran

- `git diff v0.4.3..89c2088 -- ane libane dkms.conf` is empty, and the
  release PR changes none of these paths. So `ane.ko`, `ane_t6021.ko` and
  libane are the bytes of v0.4.3.
- The module sources (`dkms.conf`, `ane/Makefile`, `ane/src`, `ane/include`,
  `ane/ane_stats_show.c`, and the C sources, headers and Makefile of
  `ane/t6021`) are also unchanged since v0.4.2. A DKMS build of v0.4.2 gave
  `ane.ko` srcversion `B4AE691E569A2BBD37E18E3` and `ane_t6021.ko` srcversion
  `59494CBC56F28ED8D1122C6`; the v0.4.2 receipt has the hardware runs of
  those modules on T8103, T6001 and T6021.
- libane: `git diff 471c03d..89c2088 -- libane` is empty. `471c03d` is the
  #111 head that ran on T8103 and T6001 (receipt
  `receipts/2026-10-03-libane-no-staging/README.md`). On T6021, libane and
  `ane-run` were built from the v0.4.3 tree and ran the smoke (20 of 20
  bit-exact) and the whole encoder (bit-exact).
- `tools/ane-run.c`, `tools/Makefile`, `packaging/omarchy-ane-smoke`, the
  fixtures and the ABI headers are unchanged since v0.4.3.
- Packaged files that #117 changed: the eight `packaging/dt/<chip>-ane.dts`
  (comments only: each compiles to a `.dtbo` byte-identical to v0.4.3's),
  the comment line of `packaging/dt/overlays`, and the order of two `case`
  patterns in `packaging/omarchy-ane-check` (`t6000|t6020` for
  `t6020|t6000`), which does not change what it prints.
- T8103 smoke of this release's libane (H246, one M1 laptop, run on
  `89c2088`, whose `libane/`, `tools/ane-run.c`, `tools/Makefile`,
  `packaging/omarchy-ane-smoke` and fixtures equal this release's). libane and
  `tools/ane-run` built from a `git archive` of that commit with `-Werror`
  (`libane.a` `6cac307d…`, `ane-run` `82349ca5…`, the same bytes as the
  T6021 build of v0.4.3). `packaging/omarchy-ane-smoke` from that tree: exit
  0, 20 of 20 calls bit-exact against the H13 golden `5ad7eccd…`, 0 errors,
  min 0.030 ms, median 0.031 ms. The tree has no `packaging/omarchy-ane-run`,
  and no package that installs `omarchy-ane-run` was installed on that laptop,
  so the smoke ran the tree's `tools/ane-run`; the run did not log the path it
  resolved. The kernel module was the installed stock 0.4.0 `ane.ko`
  (srcversion `9109B200A150B27F484F718`), not this release's module. A
  development worker with libane from the same tree ran the whole encoder
  for 8 calls in each of three cold sessions (direct load, `ANE_LOAD_STAGED=1`,
  direct again): every session exited 0 with the same output sha256
  (`5259688c…`, as in H239), and dmesg had no ANE, DART or runtime-PM error.
  The job count rests on the worker's own status lines (8 iterations, three
  times); the stock 0.4.0 module has no `ane_stats`, so no kernel counter
  confirms it. Which load path ran in each session was not traced.

## Host gate

On x86_64 (gcc 12.2.0, Python 3.11.2, pytest 9.1.1), with the steps of
`.github/workflows/dt-overlays.yml` in order and the kernel tree's dtc
(`DTC 1.7.2-g53373d13`) first in `PATH`, on a fresh `git archive` of
`d1f1f46`. That commit is this release except for the last edit of this
receipt: `git diff d1f1f46..<release commit> -- .
':!receipts/2026-10-03-omarchy-ane-0.4.4'` is empty. Every step exited 0:

| Step | Result |
| --- | --- |
| `tools/validate_ane_soc.py data/ane-soc/*.json tests/fixtures/ane-soc/*.json` | 13 files ok |
| `tools/test_validate_ane_soc.py`, `tools/gen_coverage_table.py --check` | ok, ok |
| `tools/asahi-dtbs OUT` | 29 board DTBs |
| `make -C libane libane`, `make -C tools tools` | rc 0 (`-Wall -Werror -Wextra`) |
| `make check` | SELF-CHECK PASS (one optional fixture check skipped, as in v0.4.3), IOCTL-CHECK PASS, `test_ane_stats` (model check: 90 interleavings, 0 failures) |
| `test_ane_dt`, `test_ane_firmware_fetch`, `test_ane_m2` (with `ANE_DTBS`), `test_t8112_kit` | ok |
| `test_ane_overlays` (with `ANE_DTBS`) | ok (9 overlays, 5 data-only, 32 applications; overlay state lines ok) |
| `test_promotion_check`, `test_ane_intree`, `test_aurora_dt`, `test_promote_from_verdict` | ok |
| `test_ane_smoke`, `test_ane_probe` | ok |
| `test_dkms_exclusive` with `DKMS=` dell/dkms v3.4.3 (the Arch `dkms` 3.4.3-2) | ok: `CONFIG_DRM_ACCEL_ANE=m` and `=y` exit 77 without make, and an unset option builds |
| `tools/h14_boot_regression.c` | PASSED: 127 checks, 0 failures |
| `test_promote_chip` (with `ANE_DTBS`) | ok (8 chips, every promote or revert round-trips) |
| `python3 -m pytest -q tests tools` | 100 passed, 1 skipped |

## Package recipe (omarchy-pkgs `omarchy-ane-dkms`)

The 0.4.1 recipe lines (`receipts/2026-10-03-omarchy-ane-0.4.1/README.md`)
still apply. Only `pkgver=0.4.4` and `sha256sums` change: no packaged file was
added or removed, and `dkms.conf` and the module sources are unchanged.
