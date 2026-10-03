#!/bin/bash
# AfBridgeRun: one ane_afbridge_probe insmod (read only) + rmmod of the PROBE only.
# Run inside a gpu-turn ticket. usage: probe-run.sh OUTDIR
set -euo pipefail
K=/var/tmp/afbr-d3b8561/ane/t6021/probes/ane_afbridge_probe.ko
O=${1:?outdir}
mkdir -p "$O"
state() {
	date -u +%FT%T.%3NZ
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) up $(cut -d' ' -f1 /proc/uptime)"
	lsmod | grep -E '^(ane_t6021|ane_afbridge_probe) ' || echo "no ane modules"
	for p in fw_start af_bridge_macos; do
		[ -e /sys/module/ane_t6021/parameters/$p ] && echo "$p $(cat /sys/module/ane_t6021/parameters/$p)" || true
	done
	ls /etc/modprobe.d/
	ls /dev/accel/ 2>&1
}
state >"$O/pre.txt"
sha256sum "$K" >>"$O/pre.txt"
sync
n0=$(sudo -n dmesg | wc -l)
rc=0
flock /var/tmp/ane-run.lock timeout 120 sudo -n insmod "$K" || rc=$?
echo "insmod rc=$rc" | tee "$O/rc.txt"
if lsmod | grep -q '^ane_afbridge_probe '; then
	sudo -n rmmod ane_afbridge_probe
	echo "rmmod rc=$?" | tee -a "$O/rc.txt"
fi
sudo -n dmesg | tail -n +"$((n0 + 1))" >"$O/dmesg-new.txt"
grep ane_afbridge_probe "$O/dmesg-new.txt" >"$O/afb-run.log" || true
state >"$O/post.txt"
cat "$O/afb-run.log"
