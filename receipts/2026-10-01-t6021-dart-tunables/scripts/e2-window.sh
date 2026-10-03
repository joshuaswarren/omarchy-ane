#!/bin/bash
set -euo pipefail
# DartTune E2 window (one gpu-turn ticket, one boot): A-before; apply the 38 bulk-DART RMWs;
# read; B; write back the E1 values; read; A-after; read. Stops at the first stop condition
# and then writes nothing more (a reboot restores the DART reset state).
# usage: e2-window.sh OUTDIR
set -uo pipefail
O=${1:?outdir}
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
. /var/tmp/dart/lib.sh
# E1 r0 values in probe table order (0x20c, 0x220, 0x224, 0x800..0x83c), equal on BRD and BWR.
ORIG=0x1e000048,0x00020202,0x00000000$(printf ',0x000f0000%.0s' $(seq 16))
arm() {
	local rc=0
	/var/tmp/dart/ab-turn.sh "$1" || rc=$?
	echo "arm $1 rc=$rc $(date -u +%T)"
	[ "$rc" = 0 ] || stop "arm $1 rc=$rc"
	badcheck
}
state start | tee "$O/state-start.txt"
arm A-before
probe apply apply=1
probe readB
arm B
probe restore apply=2 orig_brd="$ORIG" orig_bwr="$ORIG"
probe readA
arm A-after
probe readEnd
state end | tee "$O/state-end.txt"
sudo -n dmesg | tail -n +"$((N0 + 1))" >"$O/dmesg-new.txt"
echo "dmesg new $(wc -l <"$O/dmesg-new.txt"), bad $(grep -c -i -E "$BAD" "$O/dmesg-new.txt")"
touch "$O/DONE"
echo "== E2 done $(date -u +%T)"
