#!/bin/bash
# P1Groups: the default for the next boot = release module 54c1da56, no ane_t6021 option file.
set -euo pipefail
REL=/var/tmp/ane_t6021-54c1da56.ko
RELSHA=54c1da562f869797932403ec6af1f140ed86658142c21287bb2f26dcad128235
DST=/lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
date -u +%FT%T.%3NZ
echo "$RELSHA  $REL" | sha256sum -c -
if ! echo "$RELSHA  $DST" | sha256sum -c - >/dev/null 2>&1; then
	sudo -n install -m 0644 "$REL" "$DST"
	sudo -n depmod -a 7.1.13-3-1-ARCH
fi
sudo -n rm -f /etc/modprobe.d/ane_t6021-p1g.conf
sync
echo "$RELSHA  $DST" | sha256sum -c -
modinfo -k 7.1.13-3-1-ARCH -F version ane_t6021
ls /etc/modprobe.d/
n=$(modprobe -c | grep -c -E '^(options|install) ane_t6021' || true)
[ "$n" = 0 ] || { echo "ane_t6021 option still configured ($n)"; exit 1; }
echo RESTORE-OK
