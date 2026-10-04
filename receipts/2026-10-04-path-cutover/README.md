# Path cutover: omarchy-platform dtb-overlay paths -> omarchy-mac-boot

Branch `agent/path-cutover` (PR open, unmerged). Clean cutover, no dual-path
shim: every installed path, tool default, check message, test assertion, doc
and tester step moved in one change.

## Upstream matched (read-only)

- `omacom/omarchy-mac-pkgs#3`, MERGED as `2a3ed89eb20c5695f6291698b6646a49f8f6489d`
  ("Apply package-owned device tree overlays to m1n1 stage 2"), the port of
  `omacom/omarchy-mac#677` (`a76179cd`). The final merged `lib/dtb-overlays.sh`
  is byte-identical to the PR-head `2dc0ad6c` file this branch first matched;
  the hook, `etc/default/update-m1n1` and `install` are unchanged too. The
  maintainer's checklist (omacom/omarchy-pkgs#745 comment by maralcbr,
  2026-10-04 04:55Z) is ticked item by item below.
- Contract taken from that diff: overlays at
  `/usr/lib/omarchy-mac-boot/dtb-overlays/PREFIX/NAME.dtbo` (glob `*/*.dtbo`),
  opt-in `/etc/omarchy-mac-boot/dtb-overlays.opt-in` (one string per line,
  `grep -Fqx` against the overlay root property `omarchy,opt-in`), hook target
  `usr/lib/omarchy-mac-boot/dtb-overlays/*/*.dtbo`, `Exec = /usr/bin/update-m1n1`
  (PostTransaction; a hook first installed in the same transaction as its
  trigger does not run then), `omarchy-mac-boot` ships
  `usr/lib/omarchy-mac-boot/dtb-overlays/` empty, and the library lives at
  `/usr/lib/omarchy-mac/boot/dtb-overlays.sh` (note: `omarchy-mac/boot`, not
  `omarchy-mac-boot`). Root properties `omarchy,opt-in` and
  `omarchy,skip-if-compatible` are still read by `dtb-overlays.sh` in the new
  place - unchanged on our side.
- Upstream env knobs `OMARCHY_DTB_OVERLAYS_ROOT` / `OMARCHY_DTB_OVERLAYS`
  belong to omarchy-mac-boot's lib; our tools use none of them. No
  `OMARCHY_*` path knob exists in omarchy-ane-dt (tests use `--root`), so no
  env knob changed here.

## Old -> new

| Old (before this branch) | New (this branch) |
| --- | --- |
| `/usr/share/omarchy-platform/dtb-overlays/PREFIX/NAME.dtbo` | `/usr/lib/omarchy-mac-boot/dtb-overlays/PREFIX/NAME.dtbo` |
| `/etc/omarchy-platform/dtb-overlays.opt-in` | `/etc/omarchy-mac-boot/dtb-overlays.opt-in` |
| `Target = usr/share/omarchy-platform/dtb-overlays/*` (90-omarchy-ane-dt.hook) | `Target = usr/lib/omarchy-mac-boot/dtb-overlays/*/*.dtbo` (the glob of omarchy-mac-boot's 95 hook) |
| `OVERLAY_DIR = "usr/share/omarchy-platform/dtb-overlays"` | `OVERLAY_DIR = "usr/lib/omarchy-mac-boot/dtb-overlays"` |
| `OLD_OVERLAY_DIR = "usr/lib/omarchy-platform/dtb-overlays"` (one retired dir) | `OLD_OVERLAY_DIRS = ("usr/lib/omarchy-platform/dtb-overlays", "usr/share/omarchy-platform/dtb-overlays")` (both retired dirs refuse) |

Pacman removes package-owned `.dtbo` files at the old path on upgrade, so a
package upgrade moves the files. A hand install in either old directory
(`packaging/build-dtbo /`, the lab M2) keeps its copies: `apply` refuses while
files remain in either `OLD_OVERLAY_DIRS` location, keeps the current m1n1
copies and the update-m1n1 line, and names the steps - the same one-step guard
the 2026-10-01 move used, now covering both retired locations. The opt-in file
is the owner's `/etc` file and is not migrated by the package: re-add your
opt-in lines to the new file (CHANGELOG Unreleased says so). An absent opt-in
file means not opted in, which fails safe (hardware off).

## Maintainer checklist (omarchy-pkgs#745 comment, maralcbr)

1. `OVERLAY_DIR` + `OPT_IN` in `omarchy-ane-dt` - done; re-verified against
   the merged `2a3ed89` files: same dir, same one-string-per-line opt-in
   format.
2. Old-dir refusal covers both retired locations (`usr/lib/omarchy-platform`
   and `usr/share/omarchy-platform`), with a test for each
   (`tools/test_ane_dt.py` loops both).
3. `build-dtbo` reads `OVERLAY_DIR` from `packaging/omarchy-ane-dt`
   (`sed -n 's/^OVERLAY_DIR = ...'`), nothing hardcoded - consistent.
4. `90-omarchy-ane-dt.hook` keeps both Targets: kernel dtbs and the new
   overlay glob `usr/lib/omarchy-mac-boot/dtb-overlays/*/*.dtbo`, the same
   pattern as omarchy-mac-boot's `95-omarchy-mac-dtb-overlays.hook`.
5. Opt-in path in `omarchy-ane-check` and every doc - done; `receipts/` are
   point-in-time evidence and untouched.
6. `DTBS=` behavior: upstream's `dtb_overlays_update_m1n1` returns early when
   the admin set `DTBS=` (warning only, overlays left out). Our text already
   matches: `omarchy-ane-check` prints `dtbs_source=kernel: overlay opt-in
   has no effect, chip enabled only by its node in the kernel DT` and
   `apply` refuses rather than build something m1n1 will not boot.

## Files changed

- `packaging/omarchy-ane-dt` - `OVERLAY_DIR`, `OPT_IN`, `OLD_OVERLAY_DIRS`
  (both retired dirs), docstring, constant comments (the one place that names
  the directory).
- `packaging/90-omarchy-ane-dt.hook` - second `Target`, now the
  `*/*.dtbo` glob.
- `packaging/omarchy-ane-check` - both UNTESTED-SoC opt-in hint lines.
- `packaging/dt/overlays` - comment.
- `packaging/dt/t6002-ane.dts`, `t6022-ane.dts`, `t8112-ane.dts`,
  `t6021-uboot-serial-stdin.dts` - "Overlay state" header comments.
- `tools/promote_chip.py` - `STATE_OPTIN` template (promoted DTS headers).
- `tools/omarchy-ane-h15-stage` - pre-condition comment.
- `tools/test_ane_dt.py` - derives paths from the `oadt.OVERLAY_DIR` /
  `oadt.OPT_IN` constants and asserts the hook watches `OVERLAY_DIR` with the
  `*/*.dtbo` glob; the old-directory refusal test loops both retired dirs.
- `tools/test_ane_intree.py`, `tools/test_ane_m2.py` - literal assertions of
  the check's opt-in step line.
- `tools/test_promote_chip.py` - literal assertion of the promoted DTS header.
- `tools/test_ane_overlays.py`, `tools/test_aurora_dt.py`,
  `tools/test_ane_firmware_fetch.py`, `tools/aurora_dt.py`,
  `tools/promote_from_verdict.py`, `tools/promotion_check.py` - audited, no
  old-path strings.
- `README.md` - opt-in path, judged-row paragraph, Install/Omarchy bullet
  (overlay dir + hook name), the omarchy-mac-boot support paragraph (now names
  `omacom/omarchy-mac-pkgs#3`, the port of #677), Arch Linux ARM section
  (wording unchanged; it never named the overlay dir and stays true).
- `docs/h15-volunteer.md` - install path and opt-in line in the Step 1 block.
- `docs/ane-soc-data.md` - opt-in path in the data-only rules.
- `ane/h15/README-bringup.md`, `ane/h16/README-bringup.md` - opt-in path.
- `ane/h15/t8122-ane-experimental.dts`,
  `ane/h16/t8132-ane-experimental.dts` - header comments.
- `CHANGELOG.md` - new Unreleased entry (breaking move, opt-in re-add note,
  ALARM note). Older changelog sections are history and keep the old paths.

Receipts under `receipts/` are point-in-time evidence of past runs and were
left untouched.

## What the tester steps now say

`omarchy-ane-check` on an untested, opted-out SoC (T6002 / T6022 / T8112):

    UNTESTED SoC: $soc. $mod has not run on it. Its overlay applies only when ane-$soc is a line of /etc/omarchy-mac-boot/dtb-overlays.opt-in.
      To bring the chip up:
        1. echo ane-$soc | sudo tee -a /etc/omarchy-mac-boot/dtb-overlays.opt-in
        2. sudo omarchy-ane-firmware-fetch        (T602x/T8112 only)
        3. sudo omarchy-ane-dt apply
        4. sudo update-m1n1
        5. sudo reboot

With `DTBS=` set, step 1 becomes "boot a kernel whose own device tree enables
the $soc ANE node" and no step names an opt-in key (unchanged rule). The
H15/H16 volunteer runbooks (`docs/h15-volunteer.md`, `ane/h15
/README-bringup.md`, `ane/h16/README-bringup.md`) install the hand-built
`.dtbo` to `/usr/lib/omarchy-mac-boot/dtb-overlays/PREFIX/` and echo the key
into `/etc/omarchy-mac-boot/dtb-overlays.opt-in`. First-install note carried
from the upstream contract: the overlay reaches `boot.bin` at the next
`update-m1n1`, so the steps still end in an explicit `sudo update-m1n1` and
reboot.

## ALARM route (no omarchy-mac-boot)

MEASURED in code and tests, not on ALARM hardware: `overlays_for` globs
`root/OVERLAY_DIR/*/*.dtbo` (empty when the dir is absent), `opted_in`
returns an empty set on `FileNotFoundError`, and the package itself installs
the `.dtbo` files into `/usr/lib/omarchy-mac-boot/dtb-overlays/` (build-dtbo's
`install -d "${out%/*}"` creates the parents), so ALARM users without
omarchy-mac-boot get the directory from the package and need nothing else.
`apply`, `update-m1n1-dtbs` and both `90-omarchy-ane-dt` hooks are unchanged
on that route. No ownership conflict on Omarchy: our package installs files
under the directory omarchy-mac-boot owns; it ships no empty-dir entry of its
own (the recipe patch keeps it that way).

## Gates (final tree, commit 6f1c837)

- `pytest -q tests tools` - `100 passed, 1 skipped` (163.04 s). Round-2
  history: the first run of the two-old-dir loop had a collection error (the
  test reused one temp root, `FileExistsError`); fixed with a fresh root per
  iteration, then green. The very first round had one failure from an unbuilt
  `tools/ane-run` binary in the fresh worktree; after `make -C tools`, green.
- `make -C tools check` - OK (ane_stats host unit test suite, 0 failures).
- `python3 ~/src/voice-program/scripts/voice_lint.py --mode article README.md`
  - `fail=0 warn=1` (one pre-existing Flesch warn at the title, below the
  fail threshold; the gate is fail=0).
- PR CI on 6f1c837 (observed via the check-runs API): overlays success,
  ane-soc-data success (both required), data-only success, Kilo Code Review
  success; mergeable_state clean.

## Recipe changes for omarchy-pkgs #745

`omarchy-pkgs-745.patch` (next to this README), against PR head
`80adc10c6cf6262ab796063d3377c7521b1779e5`:

- the `package()` comment block names the new overlay path and the ALARM
  directory creation;
- `conflicts=('omarchy-mac-boot<20261004-2')` (the maintainer's guard from the
  #745 comment: an older omarchy-mac-boot reads no overlay directory, so this
  package's overlays would install where nothing reads them);

No install line changes: `packaging/build-dtbo "$pkgdir"` reads `OVERLAY_DIR`
from the tree it packages. `omarchy-ane-dkms.install` needs no change (pacman
removes the old package-owned files on upgrade). Not pushed - it applies when
the omarchy-ane release carrying this cutover lands and #745 bumps `pkgver`.
Whether that release cuts from the 0.4.2 line or keeps T6000/T6020 on is the
release step's call (maintainer's note), not this PR's.

## Open questions

1. Opt-in migration: not done by design (clean cutover, `/etc` is the
   owner's file). If Joshua prefers the package to carry opt-in lines over
   (post_upgrade `cat old >> new`), that is a one-line addition to the #745
   `.install` - say the word.
2. `OLD_OVERLAY_DIRS` now covers two retired locations; the even-older
   pre-2026-10-01 dir is not separately versioned - the guard's comment says
   when to remove the check.
