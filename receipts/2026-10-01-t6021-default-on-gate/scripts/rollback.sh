#!/bin/bash
# OwnMemGate rollback: the pre-gate state for the next boot (release module 54c1da56, the old dt tool,
# the old overlay directory, the old opt-in file, the old DTB copy; no ane_t6021 option file; the files
# that the package install added removed). Does not touch the ESP (the install never wrote it).
# usage: bash rollback.sh (on the M2)
set -euo pipefail
P=/var/tmp/ownmem/pre
K=7.1.13-3-1-ARCH
S=/var/lib/omarchy-ane/dtbs/$K
N=/usr/share/omarchy-platform/dtb-overlays
date -u +%FT%TZ
(cd "$P" && sha256sum -c --quiet SHA256SUMS)
sudo -n install -m 0644 "$P/ane_t6021-54c1da56.ko" /lib/modules/$K/updates/ane_t6021.ko
sudo -n depmod -a $K
sudo -n rm -f /etc/modprobe.d/ane_t6021-ownmem.conf
sudo -n install -m 0755 "$P/omarchy-ane-dt" /usr/bin/omarchy-ane-dt
sudo -n install -m 0644 "$P/update-m1n1-dtbs" /usr/lib/omarchy-ane/update-m1n1-dtbs
sudo -n rm -f /usr/bin/omarchy-ane-check /usr/bin/omarchy-ane-firmware-fetch \
	/usr/share/libalpm/hooks/90-omarchy-ane-dt.hook /usr/share/libalpm/hooks/90-omarchy-ane-dt-remove.hook \
	/usr/share/libalpm/hooks/90-omarchy-ane-firmware.hook
if [ -d "$N" ]; then
	sudo -n rm -f "$N"/*/*.dtbo
	sudo -n rmdir "$N"/* "$N"
	sudo -n rmdir --ignore-fail-on-non-empty /usr/share/omarchy-platform
fi
for d in "$P"/old-dtb-overlays/*; do
	sudo -n install -d /usr/lib/omarchy-platform/dtb-overlays/"${d##*/}"
	sudo -n install -m 0644 "$d"/*.dtbo /usr/lib/omarchy-platform/dtb-overlays/"${d##*/}"/
done
sudo -n install -m 0644 "$P/dtb-overlays.opt-in" /etc/omarchy-platform/dtb-overlays.opt-in
sudo -n install -m 0644 "$P/etc-default-update-m1n1" /etc/default/update-m1n1
sudo -n install -m 0644 "$P/t6021-j414c.dtb" "$S/t6021-j414c.dtb"
sudo -n install -m 0644 "$P/t6021-j414c.dtb.src" "$S/t6021-j414c.dtb.src"
sync
sha256sum /lib/modules/$K/updates/ane_t6021.ko /usr/bin/omarchy-ane-dt /usr/lib/omarchy-ane/update-m1n1-dtbs \
	/usr/lib/omarchy-platform/dtb-overlays/*/* /etc/omarchy-platform/dtb-overlays.opt-in "$S"/t6021-j414c.dtb*
sudo -n sha256sum /boot/efi/m1n1/boot.bin
n=$(modprobe -c | grep -c -E '^(options|install) ane_t6021' || true)
[ "$n" = 0 ] || { echo "ane_t6021 option still configured ($n)"; exit 1; }
echo ROLLBACK-OK
