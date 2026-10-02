#!/bin/bash
# OwnMemGate boot A install: main 73da8f8 as the omarchy-ane package installs it (DefaultOn receipt,
# omarchy-pkgs list): module, /usr/bin tools (no omarchy-ane-m2-enable), update-m1n1-dtbs, the three
# pacman hooks, overlays via build-dtbo / (new directory), the old hand-installed overlay directory
# removed (backup in /var/tmp/ownmem/pre), `ane-t6021` out of the opt-in file, then what the hooks run:
# omarchy-ane-firmware-fetch --hook and omarchy-ane-dt apply. Refuses unless the DTB copy bytes stay
# c31a54c3 (no ESP write; no update-m1n1 here).
# usage: bash install-pkg.sh (on the M2)
set -euo pipefail
R=/var/tmp/ownmem/73da8f8
P=/var/tmp/ownmem/pre
K=7.1.13-3-1-ARCH
S=/var/lib/omarchy-ane/dtbs/$K
O=/usr/lib/omarchy-platform/dtb-overlays
KO=/var/tmp/ownmem/ane_t6021-main-73da8f8.ko
KOSHA=e77c3c75aed66b1ca03c6ab725230458055c4f37ffa887f1e24d5b7365cf2b52
DST=/lib/modules/$K/updates/ane_t6021.ko
date -u +%FT%TZ
grep -qx 73da8f8462ac9f4c0f153bf7f96d370286f1eca1 /var/tmp/ownmem/73da8f8.commit
grep -q BACKUP-OK /var/tmp/ownmem/backup.log
(cd "$P" && sha256sum -c --quiet SHA256SUMS)
echo "$KOSHA  $KO" | sha256sum -c -
[ "$(modinfo -F vermagic "$KO")" = "$(modinfo -F vermagic /var/tmp/ane_t6021-54c1da56.ko)" ]
ls /etc/modprobe.d/

echo "== module"
sudo -n install -m 0644 "$KO" "$DST"
sudo -n depmod -a $K
echo "$KOSHA  $DST" | sha256sum -c -
modinfo -k $K -F version ane_t6021

echo "== tools and hooks"
sudo -n install -Dm755 -t /usr/bin "$R/packaging/omarchy-ane-dt" "$R/packaging/omarchy-ane-check" \
	"$R/packaging/omarchy-ane-firmware-fetch"
sudo -n install -Dm644 "$R/packaging/update-m1n1-dtbs" /usr/lib/omarchy-ane/update-m1n1-dtbs
sudo -n install -Dm644 -t /usr/share/libalpm/hooks "$R/packaging/90-omarchy-ane-dt.hook" \
	"$R/packaging/90-omarchy-ane-dt-remove.hook" "$R/packaging/90-omarchy-ane-firmware.hook"
sudo -n "$R/packaging/build-dtbo" /

echo "== old overlay directory out (backup $P/old-dtb-overlays)"
if [ -d "$O" ]; then
	sudo -n rm -f "$O"/*/*.dtbo
	sudo -n rmdir "$O"/* "$O"
	sudo -n rmdir --ignore-fail-on-non-empty /usr/lib/omarchy-platform
fi

echo "== opt-in: ane-t6021 out"
grep -vx ane-t6021 "$P/dtb-overlays.opt-in" >/var/tmp/ownmem/opt-in.new
sudo -n install -m 0644 /var/tmp/ownmem/opt-in.new /etc/omarchy-platform/dtb-overlays.opt-in
cat /etc/omarchy-platform/dtb-overlays.opt-in

echo "== what 90-omarchy-ane-firmware.hook runs"
sudo -n omarchy-ane-firmware-fetch --hook
omarchy-ane-firmware-fetch --check
echo "== omarchy-ane-dt apply"
sudo -n omarchy-ane-dt apply
cat "$S/t6021-j414c.dtb.src"
sync
sha256sum "$S/t6021-j414c.dtb" /usr/bin/omarchy-ane-dt /usr/bin/omarchy-ane-check /usr/bin/omarchy-ane-firmware-fetch \
	/usr/lib/omarchy-ane/update-m1n1-dtbs /usr/share/libalpm/hooks/90-omarchy-ane-* \
	/usr/share/omarchy-platform/dtb-overlays/*/* /etc/default/update-m1n1 "$DST"
sudo -n sha256sum /boot/efi/m1n1/boot.bin
cmp "$S/t6021-j414c.dtb" "$P/t6021-j414c.dtb" || { echo "DTB-DIFFERS: stop, ESP discipline needed"; exit 4; }
echo "DTB copy bytes unchanged (c31a54c3): no ESP write"
[ ! -e /usr/lib/omarchy-platform/dtb-overlays ]
n=$(modprobe -c | grep -c -E '^(options|install) ane_t6021' || true)
[ "$n" = 0 ]
omarchy-ane-dt status || true
omarchy-ane-check || true
echo INSTALL-OK
