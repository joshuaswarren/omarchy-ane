# T6021: boot of the 0.4.0 release module and of the #23 overlay (2026-10-01)

## Purpose

The v0.4.0 draft (target `9f37b47`) lists two T6021 items as pending:

1. A boot of the release-built `ane_t6021.ko` (sha256 `54c1da56…`). The draft's
   device gate ran on the module `a584a967…`, not on the release bytes.
2. A boot of a tree made by the #23 overlay change: `packaging/dt/t6021-ane.dts`
   sets `status = "okay"` on the three ANE DARTs.

Both changes go into one disk boot of the M2 Max (j414c). The laptop boots from
the internal disk: m1n1 stage 1, a lab m1n1 stage 2 (`v1.6.1-pdtrace`, it adds the
two `ane-firmware` reserved-memory nodes), U-Boot `2026.07` with the opt-in
`uboot-serial-stdin-t6021` overlay, GRUB, and linux-asahi `7.1.13-3-1-ARCH`. There
is no USB proxy catcher for this boot, so a boot that does not come back needs
the owner at the laptop.

## Inputs before the boot

| Item | Now (boot `529d10a1`) | For the test boot |
|---|---|---|
| `/lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko` | `a584a96744a8d51232ee1ede5996c8fe8f8cf11556b2751283446cc539f7ae7f` (no version) | `54c1da562f869797932403ec6af1f140ed86658142c21287bb2f26dcad128235` (0.4.0, srcversion `1FEE1B2BCF064C6109E1904`) |
| ESP `m1n1/boot.bin` | `e1c37287b837a43bf91ce88eab58a33963add45c71e73fa5c72523c2a825f75b` | `62ba3010847146347ae572987482f04f6443dd80719f0d9fca58af25fe4bd540` |
| j414c tree in boot.bin = `omarchy-ane-dt` copy | `8f5491c27c28ec69aeab5d696b0fec606cf1bd34a5193547a197b12506590e50` | `c31a54c3fc917059af9aa4531ec319ef5e438799805b87f0a133834afc6c8a5b` |
| t6021 `omarchy-ane.dtbo` | `c9441f55…` (before #23) | `60ff275f…` (`build-dtbo` of `9f37b47`) |
| `/usr/bin/omarchy-ane-dt` | `28b596eb…` | `f8520319…` (`9f37b47`) |

The opt-in file (`ane-t6021`, `uboot-serial-stdin-t6021`) and
`/etc/default/update-m1n1` (the lab `M1N1` pin and the omarchy-ane line) do not
change.

### Module bytes

`54c1da56…` is the module built for the draft (archive of `45776fc`; `ane/` and
`libane/` are the same at `9f37b47`). A rebuild from an archive of `9f37b47` in
`/var/tmp/rel-0.4.0r` gave `2728e38c2c967edcd92eee86bd58eef951ea5557f022d06ba0f3122c6c9943fe`:
the same srcversion, the same version and vermagic, and the same `.text`
(sha256 `8fd4828b…` for both). The difference is the build directory: the module
holds the path 10 times, and `rel-0.4.0r` is one character longer than
`rel-0.4.0`, so the file is 8 bytes longer. The module bytes depend on the build
directory, so a DKMS build only gives the same bytes when it builds from the
same path. The boot uses the `54c1da56…` bytes.

### boot.bin candidate

Built on the M2 the way the package does it: the release `omarchy-ane-dt`,
`update-m1n1-dtbs` and `build-dtbo` installed, `omarchy-ane-dt apply`, then
`update-m1n1 /var/tmp/rel-0.4.0r-boot/boot.bin.R` (a new path, so no ESP write).
dtc and fdtoverlay are both from `dtc 1:1.8.1-1` (`libfdt.so.1.8.1`).

Checks against the installed `e1c37287…`:

- m1n1 part (0x0-0x120000) `6fdf8bba…` and U-Boot (660,216 B) `b8d52d16…` are
  identical; 110 trees in both; no config tail.
- All bytes before the j414c tree (offset 0x277276) and after it are identical.
  Only the j414c tree changed (110,694 B to 110,754 B).
- `dtc -s` diff of the new tree against `8f5491c2`: exactly three added lines,
  `status = "okay";` in `iommu@285800000`, `iommu@285810000` and
  `iommu@285820000`.

## Rollback (written before the reboot)

Backups on the M2 in `/var/tmp/rel-0.4.0r-pre/` (with `SHA256SUMS`): the old
module, the ESP boot.bin, the DTB copy and `.src`, the old tools and overlays.
The ESP gets `m1n1/boot.bin.e1c37287.bak`. `/var/tmp/diskboot/boot.bin.D2` is a
second copy of `e1c37287…`.

1. Module (if the boot comes up and a module check fails):

   ```
   sudo install -m 0644 /var/tmp/ane_t6021-a584a967.ko /lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
   sudo depmod -a 7.1.13-3-1-ARCH
   sync; sleep 40; sync
   sudo systemctl reboot
   ```

2. Tree (only if the boot comes up without the ANE):

   ```
   sudo cp /boot/efi/m1n1/boot.bin.e1c37287.bak /boot/efi/m1n1/boot.bin
   sudo cp -p /var/tmp/rel-0.4.0r-pre/t6021-j414c.dtb /var/tmp/rel-0.4.0r-pre/t6021-j414c.dtb.src \
       /var/lib/omarchy-ane/dtbs/7.1.13-3-1-ARCH/
   sudo install -m 0755 /var/tmp/rel-0.4.0r-pre/omarchy-ane-dt /usr/bin/omarchy-ane-dt
   sudo cp -a /var/tmp/rel-0.4.0r-pre/dtb-overlays/. /usr/lib/omarchy-platform/dtb-overlays/
   sync
   sudo sha256sum /boot/efi/m1n1/boot.bin /var/lib/omarchy-ane/dtbs/7.1.13-3-1-ARCH/t6021-j414c.dtb  # e1c37287…, 8f5491c2…
   sleep 40; sync; sudo systemctl reboot
   ```

   `omarchy-ane-dt remove` is not the rollback: it removes the ANE node.
   `boot.bin.5c5a25c8.bak` is not the rollback: that image stops at the U-Boot
   prompt on this laptop.

3. No ssh 6 min after the reboot: stop and report. No retry.

## Result

PASS on boot `65d832d5-482a-48b3-abcc-017a87ad4c7e`. No rollback ran.

The M2 had been parked for a macOS session (14:30-16:52Z). It came back on
boot `a273ac7b` with the old module and `e1c37287`. The install ran at
16:53Z: ESP backup `boot.bin.e1c37287.bak`, module `54c1da56…` and boot.bin
`62ba3010…`, each verified by readback, then sync and 45 s. The lab locks
were free right before the reboot (T0 16:54:36Z, `systemctl reboot`). ssh
answered 110 s later.

After the boot:

| Check | Result |
|---|---|
| Boot path | disk: `BOOT_IMAGE=/vmlinuz-linux-asahi …`, m1n1 stage 2 `v1.6.1-pdtrace`, U-Boot 2026.07; `running`, 0 failed units |
| Loaded module | `/lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko` sha256 `54c1da562f869797932403ec6af1f140ed86658142c21287bb2f26dcad128235`; `modinfo` and `/sys/module/ane_t6021/version` `0.4.0`; srcversion `1FEE1B2BCF064C6109E1904` |
| Device tree | `iommu@285800000/285810000/285820000` `status = "okay"` (before: no status), each bound to `apple-dart`; `284000000.ane` bound to `ane_t6021`; `omarchy-ane-dt status`: `node=present source=overlay` |
| `omarchy-ane-check` (release tree) | 6 ok, `ready` |
| Mailbox IRQs `285408000` recv/send | 0 / 0 |
| `bo_total_max_mb` | 12288 |
| add, mul | GATE PASS, 512/512 lanes bit-exact, 4 trials + reopen |
| matvec 2048x5120 | GATE PASS, 5120/5120 lanes in band, max 0.187 cond units |
| island-c-pv, island-a-kt, island-a-attn-p1 | GATE PASS, 3 seeds, max 0.487 cond units |
| island-b-select-runtime, island-b-select-constfill | GATE PASS, 3 seeds, exact |
| rms-c2048-gamma | GATE PASS, 3 seeds, max 1.55 ulp |
| Qwen prog_020, one call | vs M1 golden rel L2 0.001174, max abs 0.000732; 4.830 ms |
| Whole Parakeet encoder, 5 calls | vs golden max abs 0, rel L2 0; fp16 sha256 `fca96f13…`; 254.275 ms min, 254.442 ms median |
| Lifecycle | 20 of 20 `gate.sh add` loads GATE PASS |
| 60 s burst, 4 workers | 13,141 processes, 13,141 exact, 0 fail |
| Kernel log | 0 lines of `EXCH.*failed`, `completion wait`, `mailbox.*timed out`, `ETIMEDOUT`, `quarantin`, `translation fault`, DART error over the whole boot; the only new lines during the gates are firewall (`UFW BLOCK`) lines |

Every device process ran under `flock /var/tmp/ane-run.lock timeout 120`, with
the userspace built on the M2 from an archive of `9f37b47` (`ane-run`
`f687a7ab…`, equal to the draft's build). One protocol slip: the first
prog_020 and encoder runs had a second, outer flock around
`tools/qwen_prog_run.py`, which takes the same lock itself. Both waited on
each other and ended at the 120 s timeout before any device work (no program
load, no output surface). The numbers above come from the rerun without the
outer lock.

The M2 stays on the release module and on boot.bin `62ba3010…`.

Logs: `logs/tree.diff` (new tree against `8f5491c2`), `logs/compare.txt`
(boot.bin parts), `logs/gate-summary.log` (first gate run, with the two
timed-out steps), `logs/qwen-rerun-summary.log` (prog_020 and encoder rerun).

## Limits

- One boot of one M2 Max, with two changes (the module and the tree). A
  pass covers both. It does not say what each one does alone.
- The #23 change was booted on a kernel with no ANE nodes. A kernel tree that
  ships them disabled (aurora-silicon/linux #65) has not been booted.
- The disk path here uses the lab m1n1 stage 2 and the opt-in U-Boot
  overlay (see [2026-10-01-t6021-disk-boot](../2026-10-01-t6021-disk-boot/README.md)).
- No camera watched this boot.
