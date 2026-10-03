#!/bin/bash
set -euo pipefail
# P1Groups device window (run inside gpu-turn): optional read-only probes (bridge 26 + the ten
# P-1 words; the three ANE DARTs, apply=0), then one timing arm (ab-turn.sh).
# usage: window.sh TAG [noprobe]
set -uo pipefail
TAG=${1:?tag}
O=/var/tmp/p1g/win-$TAG-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
L=/var/tmp/ane-run.lock
PB=/var/tmp/p1g/46636d3/src/ane/t6021/probes
AFBSHA=8fb43182b634cb688ba52fb47dad759c7a5ec8779c8e8f013a3bbfc911e9817e
DARTSHA=cc40de4612a7966df7d21537687e7ffcab9f7a7fda564e6fe018e55c3785ec04

probe() { # module ko sha
	local m0 rc
	echo "$3  $2" | sha256sum -c --quiet - || { echo "STOP: probe sha $1"; touch "$O/STOPPED"; exit 3; }
	sync
	m0=$(sudo -n dmesg | wc -l)
	echo "probe $1 start $(date -u +%T.%3N)"
	flock "$L" timeout 120 bash -c '! pgrep -x ane-run >/dev/null || { echo "ane-run running"; exit 9; }; sudo -n insmod "$0"' "$2"
	rc=$?
	echo "probe $1 insmod rc=$rc end $(date -u +%T.%3N)"
	if lsmod | grep -q "^$1 "; then
		sudo -n rmmod "$1"
		echo "probe $1 rmmod rc=$?"
	fi
	sudo -n dmesg | tail -n +"$((m0 + 1))" | grep "$1" >"$O/$1.log"
	tail -1 "$O/$1.log"
	[ "$rc" = 0 ] || { echo "STOP: probe $1 rc=$rc"; touch "$O/STOPPED"; exit 3; }
}

echo "== window $TAG boot $(cat /proc/sys/kernel/random/boot_id) up $(cut -d' ' -f1 /proc/uptime) $(date -u +%FT%T.%3NZ)"
echo "p1_groups $(cat /sys/module/ane_t6021/parameters/p1_groups 2>/dev/null || echo absent) module $(cat /sys/module/ane_t6021/version)"
if [ "${2:-}" != noprobe ]; then
	probe ane_afbridge_probe "$PB/ane_afbridge_probe.ko" "$AFBSHA"
	probe ane_dart_probe "$PB/ane_dart_probe.ko" "$DARTSHA"
fi
bash /var/tmp/p1g/ab-turn.sh "$TAG"
echo "== window $TAG end $(date -u +%T) $O"
