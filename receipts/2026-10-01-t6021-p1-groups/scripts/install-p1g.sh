#!/bin/bash
set -euo pipefail
# P1Groups: install the 46636d3 module and a ONE-SHOT p1_groups=MASK option for the next load.
# The modprobe.d `install` line deletes its own file (and syncs) before it loads the module with
# the parameter, so a reset during the boot falls back to p1_groups=0 (today's P-1) on the next
# boot instead of a reset loop. usage: install-p1g.sh MASK   (0x1..0x1f)
MASK=${1:?mask}
[[ "$MASK" =~ ^0x(1[0-9a-f]|[1-9a-f])$ ]] || { echo "bad mask $MASK"; exit 2; }
K=/var/tmp/ane_t6021-e865dec3.ko
SHA=e865dec352db2c7eea757ce397a226023a9c22128132a7a3f07a3dcce4b675aa
REL=/var/tmp/ane_t6021-54c1da56.ko
RELSHA=54c1da562f869797932403ec6af1f140ed86658142c21287bb2f26dcad128235
DST=/lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
date -u +%FT%T.%3NZ
[ -e "$K" ] || cp /var/tmp/p1g/46636d3/src/ane/t6021/ane_t6021.ko "$K"
echo "$SHA  $K" | sha256sum -c -
echo "$RELSHA  $REL" | sha256sum -c -
[ "$(modinfo -F vermagic "$K")" = "$(modinfo -F vermagic "$DST")" ]
[ "$(modinfo -F parm "$K" | grep -c '^p1_groups:')" = 1 ]
ls /etc/modprobe.d/
[ ! -e /etc/modprobe.d/ane_t6021-nofw.conf ]
[ ! -e /etc/modprobe.d/ane_t6021-afb.conf ]
if ! echo "$SHA  $DST" | sha256sum -c - >/dev/null 2>&1; then
	sudo -n install -m 0644 "$K" "$DST"
	sudo -n depmod -a 7.1.13-3-1-ARCH
fi
printf '%s\n' '# P1Groups one-shot (removes itself before the load).' \
	"install ane_t6021 /usr/bin/rm -f /etc/modprobe.d/ane_t6021-p1g.conf && /usr/bin/sync && /usr/bin/modprobe --ignore-install ane_t6021 p1_groups=$MASK \$CMDLINE_OPTS" |
	sudo -n tee /etc/modprobe.d/ane_t6021-p1g.conf
sync
echo "$SHA  $DST" | sha256sum -c -
modinfo -k 7.1.13-3-1-ARCH -F version ane_t6021
modinfo -k 7.1.13-3-1-ARCH -n ane_t6021
modprobe -c | grep '^install ane_t6021'
echo INSTALL-OK
