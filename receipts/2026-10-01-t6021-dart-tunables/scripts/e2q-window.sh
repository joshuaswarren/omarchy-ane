#!/bin/bash
# DartTune E2' window (one gpu-turn ticket): read; A arm; apply GROUPS in the macOS order; read;
# G arm; undo GROUPS (not for 0); read. GROUPS=final: read and A arm only.
# A failed G arm (fault or check) is recorded and undone (pre-registered rule a); for G0, a failed
# undo, a failed A arm or a failed probe the window stops (rule b) and writes nothing more.
# usage: e2q-window.sh OUTDIR GROUPS
set -uo pipefail
O=${1:?outdir}
G=${2:?groups}
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
. /var/tmp/dart/lib.sh
K=/var/tmp/dart/3ec8a28/src/ane/t6021/probes/ane_dart_probe.ko
KSHA=50de061c8659452321f89543858e16b7d86b454c429509861a2ee52cb3a1b862
# E1 r0 values, probe table order (0x20c, 0x220, 0x224, 0x800..0x83c), equal on BRD and BWR.
ORIG=0x1e000048,0x00020202,0x00000000$(printf ',0x000f0000%.0s' $(seq 16))
# The probe's own lines print DART register names ("error 0x..."); only kernel lines count as bad.
# The kernel journal, not dmesg: the probes fill the dmesg ring, which then drops old lines and
# breaks a line-count offset.
T0=$(date +%s)
badlines() {
	journalctl -k -b --no-pager -o short-monotonic --since "@$T0" 2>/dev/null |
		grep -v 'ane_dart_probe:' | grep -i -E "$BAD"
}
badcheck() {
	local new
	new=$(badlines)
	[ -z "$new" ] || stop "bad dmesg: $new"
	[ "$(cat /proc/sys/kernel/random/boot_id)" = "$BOOT" ] || stop "boot_id changed"
}

arm() {
	local rc
	/var/tmp/dart/ab-turn.sh "$1"
	rc=$?
	echo "arm $1 rc=$rc $(date -u +%T)"
	return "$rc"
}

state start | tee "$O/state-start.txt"
probe pre
arm "A-g$G" || stop "control arm A-g$G failed"
badcheck
if [ "$G" != final ]; then
	probe apply apply=1 groups="$G"
	probe readG
	if arm "G$G"; then
		echo "GROUP G$G arm passed"
	else
		echo "GROUP-FAULT G$G $(date -u +%T)"
		badlines | tee "$O/group-fault.txt"
		[ "$G" = 0 ] && stop "G0 (procedure control) failed"
		T0=$(($(date +%s) + 1))
	fi
	if [ "$G" != 0 ]; then
		probe undo apply=2 groups="$G" orig_brd="$ORIG" orig_bwr="$ORIG"
		probe readU
	fi
fi
state end | tee "$O/state-end.txt"
echo "== window G=$G done $(date -u +%T)"
touch "$O/DONE"
