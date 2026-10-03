#!/bin/bash
# OwnMemGate post-boot state (read only; AfBridgeRun postboot.sh plus the boot journal's previous-boot tail,
# the reserved-memory nodes and the fwload/fwalias/refusing lines). usage: postboot.sh OUTDIR
set -euo pipefail
O=${1:?outdir}
mkdir -p "$O"
P=/sys/module/ane_t6021/parameters
{
	date -u +%FT%T.%3NZ
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) up $(cut -d' ' -f1 /proc/uptime)"
	uname -r
	cat /proc/cmdline
	echo "system $(systemctl is-system-running) failed_units $(systemctl --failed --no-legend | wc -l)"
	findmnt -n -o SOURCE,OPTIONS /
	sudo -n sha256sum /boot/efi/m1n1/boot.bin
	echo "os-fw $(tr -d '\0' </proc/device-tree/chosen/asahi,os-fw-version)"
	lsmod | grep -E '^(ane_t6021|netconsole) ' || echo "ane_t6021 not loaded"
	echo "modinfo -n $(modinfo -n ane_t6021)"
	sha256sum "$(modinfo -n ane_t6021)"
	[ -d /sys/module/ane_t6021 ] && echo "sysfs version $(cat /sys/module/ane_t6021/version) srcversion $(cat /sys/module/ane_t6021/srcversion)" || true
	for f in "$P"/*; do echo "param $(basename "$f") $(cat "$f")"; done
	for f in /etc/modprobe.d/*; do echo "== $f"; cat "$f"; done
	ls -l /dev/accel/ 2>&1
	readlink /sys/bus/platform/devices/284000000.ane/driver || echo "284000000.ane unbound"
	sudo -n cat /sys/kernel/debug/devices_deferred 2>&1
	grep -E '285408000' /proc/interrupts || true
	ls /proc/device-tree/reserved-memory/ | grep -E '^ane-' || true
	cat /etc/omarchy-platform/dtb-overlays.opt-in 2>&1 || true
} >"$O/state.txt" 2>&1
sudo -n cat /sys/kernel/debug/pm_genpd/pm_genpd_summary >"$O/genpd.txt" 2>&1
sudo -n dmesg >"$O/dmesg.txt"
grep -E 'ane_t6021|284000000\.ane|AFB|BOOT-PHASE|fwload|fwalias|refusing|OF: reserved mem: .*ane' "$O/dmesg.txt" >"$O/dmesg-ane.txt" || true
journalctl -b -1 -n 30 --no-pager >"$O/prev-boot-tail.txt" 2>&1
echo "== fwload/fwalias/refusing/boot result"
grep -E 'fwload|fwalias|refusing|pollA READY observed|pollB DONE observed|run returned|chman: table|CONFIG_GET|loaded ane_t6021' "$O/dmesg-ane.txt"
cat "$O/state.txt"
grep -E '^(ane_)' "$O/genpd.txt"
