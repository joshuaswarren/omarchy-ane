#!/bin/bash
# AfBridgeRun S3: install the d3b8561 module and a ONE-SHOT af_bridge_macos=1 option for the
# next load. The modprobe.d `install` line deletes its own file (and syncs) before it loads the
# module with the parameter, so a reset during the bridge writes falls back to a default load
# on the next boot instead of a reset loop.
set -euo pipefail
K=/var/tmp/ane_t6021-160f3ebd.ko
SHA=160f3ebda8123c23fac1d711b1aac470d6beb77506b6aab16f1f24e70c4a1009
REL=/var/tmp/ane_t6021-54c1da56.ko
RELSHA=54c1da562f869797932403ec6af1f140ed86658142c21287bb2f26dcad128235
DST=/lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko
date -u +%FT%T.%3NZ
[ -e "$K" ] || cp /var/tmp/afbr-d3b8561/ane/t6021/ane_t6021.ko "$K"
echo "$SHA  $K" | sha256sum -c -
echo "$RELSHA  $REL" | sha256sum -c -
[ "$(modinfo -F vermagic "$K")" = "$(modinfo -F vermagic "$DST")" ]
[ "$(modinfo -F parm "$K" | grep -c '^af_bridge_macos:')" = 1 ]
ls /etc/modprobe.d/
[ ! -e /etc/modprobe.d/ane_t6021-nofw.conf ]
sudo -n install -m 0644 "$K" "$DST"
sudo -n depmod -a 7.1.13-3-1-ARCH
printf '%s\n' '# AfBridgeRun one-shot (removes itself before the load).' \
	'install ane_t6021 /usr/bin/rm -f /etc/modprobe.d/ane_t6021-afb.conf && /usr/bin/sync && /usr/bin/modprobe --ignore-install ane_t6021 af_bridge_macos=1 $CMDLINE_OPTS' |
	sudo -n tee /etc/modprobe.d/ane_t6021-afb.conf
sync
echo "$SHA  $DST" | sha256sum -c -
modinfo -k 7.1.13-3-1-ARCH -F version ane_t6021
modinfo -k 7.1.13-3-1-ARCH -n ane_t6021
modprobe -c | grep '^install ane_t6021'
echo INSTALL-OK
