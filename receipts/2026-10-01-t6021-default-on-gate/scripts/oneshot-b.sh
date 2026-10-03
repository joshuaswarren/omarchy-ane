#!/bin/bash
set -euo pipefail
# OwnMemGate boot B: a ONE-SHOT fw_alias_reserved=0 for the next ane_t6021 load. The modprobe.d
# `install` line deletes its own file and syncs before it loads the module with the parameter, so any
# reset or hang during that boot falls back to the default (reserved mode) on the next boot.
# The installed module stays the main 73da8f8 build. usage: bash oneshot-b.sh (on the M2)
K=7.1.13-3-1-ARCH
F=/etc/modprobe.d/ane_t6021-ownmem.conf
KOSHA=e77c3c75aed66b1ca03c6ab725230458055c4f37ffa887f1e24d5b7365cf2b52
date -u +%FT%T.%3NZ
echo "$KOSHA  /lib/modules/$K/updates/ane_t6021.ko" | sha256sum -c -
[ "$(modinfo -k $K -F parm ane_t6021 | grep -c '^fw_alias_reserved:')" = 1 ]
ls /etc/modprobe.d/
[ ! -e "$F" ]
n=$(modprobe -c | grep -c -E '^(options|install) ane_t6021' || true)
[ "$n" = 0 ]
printf '%s\n' '# OwnMemGate one-shot (removes itself before the load).' \
	"install ane_t6021 /usr/bin/rm -f $F && /usr/bin/sync && /usr/bin/modprobe --ignore-install ane_t6021 fw_alias_reserved=0 \$CMDLINE_OPTS" |
	sudo -n tee "$F"
sync
modprobe -c | grep '^install ane_t6021'
echo ONESHOT-OK
