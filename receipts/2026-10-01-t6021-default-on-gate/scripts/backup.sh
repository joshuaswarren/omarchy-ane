#!/bin/bash
# OwnMemGate rollback receipt: copy every file the package install changes (and the ESP boot.bin,
# read only) to /var/tmp/ownmem/pre with SHA256SUMS. Changes no system file.
# usage: bash backup.sh (on the M2)
set -euo pipefail
P=/var/tmp/ownmem/pre
K=7.1.13-3-1-ARCH
S=/var/lib/omarchy-ane/dtbs/$K
test ! -e "$P"
mkdir -p "$P"
date -u +%FT%TZ
echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
cp -p /usr/bin/omarchy-ane-dt "$P/omarchy-ane-dt"
cp -p /usr/lib/omarchy-ane/update-m1n1-dtbs "$P/update-m1n1-dtbs"
cp -a /usr/lib/omarchy-platform/dtb-overlays "$P/old-dtb-overlays"
cp -p /etc/omarchy-platform/dtb-overlays.opt-in "$P/dtb-overlays.opt-in"
cp -p /etc/default/update-m1n1 "$P/etc-default-update-m1n1"
cp -p "$S/t6021-j414c.dtb" "$S/t6021-j414c.dtb.src" "$P/"
cp -p /lib/modules/$K/updates/ane_t6021.ko "$P/ane_t6021-54c1da56.ko"
sudo -n cp /boot/efi/m1n1/boot.bin "$P/boot.bin.62ba3010.bak"
sudo -n chown "$(id -u):$(id -g)" "$P/boot.bin.62ba3010.bak"
{
	echo "== absent before the install (expected):"
	for f in /usr/bin/omarchy-ane-check /usr/bin/omarchy-ane-firmware-fetch /usr/share/omarchy-platform \
		/usr/share/libalpm/hooks/90-omarchy-ane-dt.hook /usr/share/libalpm/hooks/90-omarchy-ane-dt-remove.hook \
		/usr/share/libalpm/hooks/90-omarchy-ane-firmware.hook; do
		[ -e "$f" ] && echo "PRESENT $f" || echo "absent $f"
	done
	echo "== /usr/lib/omarchy-platform"
	find /usr/lib/omarchy-platform -printf '%M %u %p\n'
	echo "== /etc/modprobe.d"
	ls -l /etc/modprobe.d
	echo "== modprobe -c ane_t6021 lines"
	modprobe -c | grep -E '^(options|install|blacklist) ane_t6021' || echo none
	echo "== libalpm hooks"
	ls /usr/share/libalpm/hooks/ /etc/pacman.d/hooks/ 2>&1
} | tee "$P/state.txt"
(cd "$P" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum | tee SHA256SUMS)
grep -q '^54c1da562f869797932403ec6af1f140ed86658142c21287bb2f26dcad128235 ' "$P/SHA256SUMS"
grep -q '^62ba3010847146347ae572987482f04f6443dd80719f0d9fca58af25fe4bd540 ' "$P/SHA256SUMS"
grep -q '^c31a54c3fc917059af9aa4531ec319ef5e438799805b87f0a133834afc6c8a5b ' "$P/SHA256SUMS"
echo BACKUP-OK
