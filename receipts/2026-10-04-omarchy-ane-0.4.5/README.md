# omarchy-ane 0.4.5: release receipt

0.4.5 is the release of the overlay path cutover (#124) on top of main
`fe936a3`. It also carries the H15 (M3) bring-up work (#121, #122, #123).
It changes no code that DKMS builds and no libane code. The project keeps
no version number in the tree: the recipe passes the package version to
`dkms.conf` (`@PKGVER@`).

## What changed since v0.4.4

`git log --first-parent v0.4.4..fe936a3`: #121 (`9ddcf53`, the H15 module),
#122 (`00097de`, the README data-only table), #123 (`51151df`, H15 host-side
fixes and the volunteer runbook) and #124 (`fe936a3`, the path cutover; its
receipt is `receipts/2026-10-04-path-cutover/README.md`). The Ultra die-1
design (#118, `4c41fa9`) is in v0.4.4 already. PR #120 (Ultra die-1 driver
plumbing) is open and is not in this release: its head `b52d16e` is not an
ancestor of `fe936a3`. The release PR changes only `CHANGELOG.md` and this
receipt.

## The bytes that the hardware gates ran

- `git diff v0.4.4..fe936a3 -- dkms.conf ane/Makefile ane/src ane/include
  ane/ane_stats_show.c ane/t6021 libane` is empty. So `ane.ko`,
  `ane_t6021.ko` and libane are the bytes of v0.4.4, and the hardware results
  in `receipts/2026-10-03-omarchy-ane-0.4.4/README.md` apply to them.
- `git diff v0.4.4..fe936a3 -- ane libane dkms.conf` is not empty. It lists
  the new `ane/h15/` tree (#121, #123) and one line each in
  `ane/h16/README-bringup.md` and the header comment of
  `ane/h16/t8132-ane-experimental.dts` (#124, the opt-in path). `dkms.conf`
  builds only `ane/` (`ane/Makefile`: `obj-m := ane.o`) and `ane/t6021/`.
  No file in `packaging/`, `.github/`, `dkms.conf`, or the `Makefile` of
  the top level, `ane/`, `libane/` or `tools/` names `h15` or `h16`. The
  recipe of omarchy-pkgs #745 (head `80adc10`) copies only the DKMS source
  tree and installs named files. So the package does not build or install
  `ane/h15/` or `tools/omarchy-ane-h15-stage`.
- `tools/ane-run.c`, `tools/Makefile`, `packaging/omarchy-ane-smoke`, the
  fixtures and the ABI headers are unchanged since v0.4.4.
- The packaged drivers, libane and the firmware fetch are unchanged, so no
  hardware ran this release. The new H15 driver code in `ane/h15/` is not
  packaged, and no M3 has run it. Of the packaged files, the path move
  changes only userspace tools, the pacman hook and comments. It is tested
  on the host (below). The release gates did not install the package on a
  Mac.

## Packaged files

Packaged files that #124 changed: `packaging/omarchy-ane-dt` (`OVERLAY_DIR`,
`OPT_IN`, `OLD_OVERLAY_DIRS`), the second `Target` of
`packaging/90-omarchy-ane-dt.hook`, two opt-in hint lines in
`packaging/omarchy-ane-check`, the comment of `packaging/dt/overlays`, and
the header comments of four `packaging/dt/*.dts` files. The new
`packaging/dt/*-ane-dataonly.dts` files (#121) compile as a check and are
never installed (`packaging/build-dtbo`).

A fake-root check (a throwaway script, not in the tree) ran
`packaging/build-dtbo` from a `git archive` of v0.4.4 and of `46f7c8f`, and
`packaging/omarchy-ane-dt` against fake roots with board DTBs from
`tools/asahi-dtbs` and the kernel tree's dtc and fdtoverlay. All 19 checks
passed:

- The release installs the same 21 files as v0.4.4 (9 overlays and 12
  `data/ane-soc` files), byte for byte. The overlays are now in
  `/usr/lib/omarchy-mac-boot/dtb-overlays/PREFIX/`, and no file is in an
  old overlay directory. T6000 and T6020 are installed as default-on.
- The hook Targets are `usr/lib/modules/*/dtbs/*` and
  `usr/lib/omarchy-mac-boot/dtb-overlays/*/*.dtbo`. The second glob matches
  every installed overlay.
- T8103 (j293, default-on): `status` before `apply` gives `source=none`;
  `apply` writes the overlaid copy and the `update-m1n1` line; `status`
  then gives `source=overlay`; a second `apply` says the copy is current;
  `remove` removes the copy and the line.
- With one `.dtbo` in `/usr/lib/omarchy-platform/dtb-overlays` or in
  `/usr/share/omarchy-platform/dtb-overlays`, `apply` exits 1, names the old
  directory, and keeps the current copy byte for byte.
- T6002 (j375d, opt-in): with no opt-in file, or with `ane-t6002` only in
  the old `/etc/omarchy-platform/dtb-overlays.opt-in`, `apply` applies
  nothing. With `ane-t6002` in `/etc/omarchy-mac-boot/dtb-overlays.opt-in`
  it applies the overlay, and `status` gives `source=overlay`.
- With omarchy-mac-boot 20261004-2 and its
  `/usr/lib/omarchy-mac/boot/dtb-overlays.sh` in the fake root, `apply`
  says that omarchy-mac-boot applies `/usr/lib/omarchy-mac-boot/dtb-overlays`
  itself, and keeps no copy.

The T8122 overlay of `ane/h15/` compiles with `dtc -@` and applies with
`fdtoverlay` to the five T8122 board DTBs of `tools/asahi-dtbs` (j433, j434,
j504, j613, j615). Each result has `ane@310000000` with compatible
`apple,t8122-ane`.

## Host gate

On x86_64 (gcc 12.2.0, Python 3.11.2, pytest 9.1.1), with the steps of
`.github/workflows/dt-overlays.yml` in order (the workflow is unchanged since
v0.4.4) and the kernel tree's dtc (`DTC 1.7.2-g53373d13`) first in `PATH`,
on a fresh `git archive` of `46f7c8f`. That commit is this release except
for the last edit of this receipt. Every step exited 0:

| Step | Result |
| --- | --- |
| `tools/validate_ane_soc.py data/ane-soc/*.json tests/fixtures/ane-soc/*.json` | 13 files ok |
| `tools/test_validate_ane_soc.py`, `tools/gen_coverage_table.py --check` | ok, ok |
| `tools/asahi-dtbs OUT` | 38 board DTBs |
| `make -C libane libane`, `make -C tools tools` | rc 0 (`-Wall -Werror -Wextra`) |
| `make check` | SELF-CHECK PASS (one optional fixture check skipped, as in v0.4.4), IOCTL-CHECK PASS, `test_ane_stats` (model check: 90 interleavings, 0 failures) |
| `make -C ane/h15 check` | `test: ane_h15_adt — ok` |
| `test_ane_dt`, `test_ane_firmware_fetch`, `test_ane_m2` (with `ANE_DTBS`), `test_t8112_kit` | ok |
| `test_ane_overlays` (with `ANE_DTBS`) | ok (9 overlays, 8 data-only, 41 applications; overlay state lines ok) |
| `test_promotion_check`, `test_ane_intree`, `test_aurora_dt`, `test_promote_from_verdict` | ok |
| `test_ane_smoke`, `test_ane_probe` | ok |
| `test_dkms_exclusive` with `DKMS=` dell/dkms v3.4.3 (the Arch `dkms` 3.4.3-2) | ok: `CONFIG_DRM_ACCEL_ANE=m` and `=y` exit 77 without make; with the option unset, dkms runs make |
| `tools/h14_boot_regression.c` | PASSED: 127 checks, 0 failures |
| `test_promote_chip` (with `ANE_DTBS`) | ok (8 chips, every promote or revert round-trips) |
| `python3 -m pytest -q tests tools` | 100 passed, 1 skipped |

CI: `dt-overlays` (`overlays`, `ane-soc-data`, run 37179585594) and
`aurora-dtbs` (`data-only`, run 37179585567) passed on main `fe936a3`. The
release PR changes only `CHANGELOG.md` and this receipt. Both are outside
the path filters of the two workflows, so CI does not run those jobs on the
release PR or on its merge. The host gate above ran every step of both
`dt-overlays` jobs on the content of the release PR.

## Package recipe (omarchy-pkgs `omarchy-ane-dkms`)

The 0.4.1 recipe lines (`receipts/2026-10-03-omarchy-ane-0.4.1/README.md`)
still apply: no packaged file was added or removed, and `dkms.conf` and the
module sources are unchanged. `pkgver=0.4.5` and `sha256sums` change, and
the recipe takes the cutover patch
`receipts/2026-10-04-path-cutover/omarchy-pkgs-745.patch`
(`conflicts=('omarchy-mac-boot<20261004-2')` and the overlay path comment).

The package keeps T6000 and T6020 opt-in until a passing row from its own
DKMS modules exists. The opt-in key is the root property `omarchy,opt-in`
of each overlay, so a change to `packaging/dt/overlays` alone does not make
a chip opt-in. On a copy of this tree, `python3 tools/promote_chip.py --chip
t6000 --to opt-in --apply` and the same command for `t6020` wrote the
`opt-in` table state, the `Overlay state:` headers and the properties
`ane-t6000` and `ane-t6020`. `packaging/build-dtbo` then installed both
overlays as opt-in, and `fdtget` read those keys from the installed files.
