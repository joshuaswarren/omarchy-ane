#!/bin/bash
# OwnMemGate boot P staging: build the boot.bin a packaged install makes, to a NEW path. It runs the
# packaged update-m1n1 (asahi-scripts) in a private mount namespace where /etc/default/update-m1n1 is
# the packaged-flow file: only the line omarchy-ane-dt adds (no lab M1N1 pin), so M1N1 and U_BOOT are
# the package defaults /usr/lib/asahi-boot/m1n1.bin and u-boot-nodtb.bin. Writes no ESP file and no
# system file. usage: bash stage-p.sh
set -euo pipefail
P=/var/tmp/ownmem/P
test ! -e "$P"
mkdir -p "$P"
date -u +%FT%TZ
pacman -Q m1n1 uboot-asahi asahi-scripts
pacman -Qkk m1n1 uboot-asahi asahi-scripts
bsdtar -xOf /var/cache/pacman/pkg/m1n1-1.6.1-1-aarch64.pkg.tar.xz usr/lib/asahi-boot/m1n1.bin | sha256sum | sed 's/-$/m1n1.bin in the cached m1n1-1.6.1-1 package/'
bsdtar -xOf /var/cache/pacman/pkg/uboot-asahi-2026.07.asahi2-1-aarch64.pkg.tar.xz usr/lib/asahi-boot/u-boot-nodtb.bin | sha256sum | sed 's/-$/u-boot-nodtb.bin in the cached uboot-asahi package/'
sha256sum /usr/lib/asahi-boot/m1n1.bin /usr/lib/asahi-boot/u-boot-nodtb.bin /usr/bin/update-m1n1
grep -x '\[ -r /usr/lib/omarchy-ane/update-m1n1-dtbs \] && \. /usr/lib/omarchy-ane/update-m1n1-dtbs  # omarchy-ane' \
	/etc/default/update-m1n1 | tee "$P/update-m1n1.packaged"
[ "$(wc -l <"$P/update-m1n1.packaged")" = 1 ]
[ ! -e /etc/m1n1.conf ]
sudo -n unshare -m sh -c "mount --bind $P/update-m1n1.packaged /etc/default/update-m1n1 && cat /etc/default/update-m1n1 && update-m1n1 $P/boot.bin.P"
sudo -n chown "$(id -u):$(id -g)" "$P/boot.bin.P"
cat /etc/default/update-m1n1
sha256sum "$P/boot.bin.P" /var/lib/omarchy-ane/dtbs/7.1.13-3-1-ARCH/t6021-j414c.dtb
sudo -n sha256sum /boot/efi/m1n1/boot.bin
ls -l "$P"
echo STAGE-P-OK
