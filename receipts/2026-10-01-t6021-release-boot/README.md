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

Pending: the reboot waits for the lab's GPU lock on the M2.
