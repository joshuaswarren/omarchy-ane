# Path cutover: omarchy-platform dtb-overlay paths -> omarchy-mac-boot

Branch `agent/path-cutover` (PR open, unmerged). Clean cutover, no dual-path
shim: every installed path, tool default, check message, test assertion, doc
and tester step moved in one change.

## Upstream matched (read-only)

- `omacom/omarchy-mac-pkgs#3`, head commit `2dc0ad6c28f3ac77536bb809befe5eaaafe51965`
  ("Apply package-owned device tree overlays to m1n1 stage 2"), the port of
  `omacom/omarchy-mac#677` (`a76179cd`). Files API + contents API read at that
  commit: `omarchy-mac-boot/lib/dtb-overlays.sh`,
  `files/usr/share/libalpm/hooks/95-omarchy-mac-dtb-overlays.hook`,
  `files/etc/default/update-m1n1`, `install`, `README.md`, and its tests.
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
| `Target = usr/share/omarchy-platform/dtb-overlays/*` (90-omarchy-ane-dt.hook) | `Target = usr/lib/omarchy-mac-boot/dtb-overlays/*` |
| `OVERLAY_DIR = "usr/share/omarchy-platform/dtb-overlays"` | `OVERLAY_DIR = "usr/lib/omarchy-mac-boot/dtb-overlays"` |
| `OLD_OVERLAY_DIR = "usr/lib/omarchy-platform/dtb-overlays"` (refusal guard, pre-2026-10-01 dir) | `OLD_OVERLAY_DIR = "usr/share/omarchy-platform/dtb-overlays"` (the dir this cutover vacates) |

Pacman removes package-owned `.dtbo` files at the old path on upgrade, so a
package upgrade moves the files. A hand install in the old directory
(`packaging/build-dtbo /`, the lab M2) keeps its copies: `apply` refuses while
files remain in `OLD_OVERLAY_DIR`, keeps the current m1n1 copies and the
update-m1n1 line, and names the steps - the same one-generation guard the
2026-10-01 move used. The opt-in file is the owner's `/etc` file and is not
migrated by the package: re-add your opt-in lines to the new file (CHANGELOG
Unreleased says so). An absent opt-in file means not opted in, which fails
safe (hardware off).

## Files changed (21)

- `packaging/omarchy-ane-dt` - `OVERLAY_DIR`, `OLD_OVERLAY_DIR`, `OPT_IN`,
  docstring, constant comments (the one place that names the directory).
- `packaging/90-omarchy-ane-dt.hook` - second `Target`.
- `packaging/omarchy-ane-check` - both UNTESTED-SoC opt-in hint lines.
- `packaging/dt/overlays` - comment.
- `packaging/dt/t6002-ane.dts`, `t6022-ane.dts`, `t8112-ane.dts`,
  `t6021-uboot-serial-stdin.dts` - "Overlay state" header comments.
- `tools/promote_chip.py` - `STATE_OPTIN` template (promoted DTS headers).
- `tools/omarchy-ane-h15-stage` - pre-condition comment.
- `tools/test_ane_dt.py` - no edit needed: it derives paths from the
  `oadt.OVERLAY_DIR` / `oadt.OLD_OVERLAY_DIR` / `oadt.OPT_IN` constants and
  asserts the hook watches `OVERLAY_DIR`.
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

## Gates (run in the worktree at 51151df + this diff)

- `pytest -q tests tools` - result recorded below.
- `make -C tools check` - OK (ane_stats host unit test suite, 0 failures).
- `python3 ~/src/voice-program/scripts/voice_lint.py --mode article README.md`
  - `fail=0 warn=1` (one pre-existing Flesch warn at the title, below the
  fail threshold; the gate is fail=0).

## Recipe changes for omarchy-pkgs #745

`omarchy-pkgs-745.patch` (next to this README), against PR head
`80adc10c6cf6262ab796063d3377c7521b1779e5`: one comment block in
`package()` names the new overlay path and the ALARM directory creation.
No install line changes: `packaging/build-dtbo "$pkgdir"` reads `OVERLAY_DIR`
from the tree it packages. `omarchy-ane-dkms.install` needs no change (pacman
removes the old package-owned files on upgrade). Not pushed - it applies when
the omarchy-ane release carrying this cutover lands and #745 bumps `pkgver`.

## Open questions

1. Opt-in migration: not done by design (clean cutover, `/etc` is the
   owner's file). If Joshua prefers the package to carry opt-in lines over
   (post_upgrade `cat old >> new`), that is a one-line addition to the #745
   `.install` - say the word.
2. Our hook watches `usr/lib/omarchy-mac-boot/dtb-overlays/*` while
   upstream's 95-hook targets `*/*.dtbo`; pacman's fnmatch makes both match
   the same installed files. Left as-is to keep the one-target form
   `test_ane_dt.py` asserts; harmless either way.
3. `OLD_OVERLAY_DIR` is again a one-generation guard (the pre-2026-10-01
   `usr/lib/omarchy-platform` dir is no longer named). Its own comment says
   when to remove the check.
