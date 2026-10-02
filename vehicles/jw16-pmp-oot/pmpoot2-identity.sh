#!/bin/bash
# pmpoot-identity.sh — identity gate on the oot-variant boot (<=60 s of ssh).
# PASS prints IDENTITY-OK; any gate failure exits nonzero.
set -euo pipefail
K=7.1.13-3-2-ARCH
OOTSHA=$(cat /var/tmp/ane-pmp4/pmp4-variant.sha | cut -d' ' -f1)
echo "-- kernel: $(uname -r) ($(uname -v))"
[ "$(uname -r)" = "$K" ] || { echo "IDENTITY-FAIL: kernel $(uname -r)"; exit 1; }
echo "-- boot id: $(cat /proc/sys/kernel/random/boot_id)"
echo "-- pmp compatible: $(tr -d '\0' < /sys/firmware/devicetree/base/soc/pmp@28e700000/compatible)"
echo "-- pmp driver: $( [ -e /sys/bus/platform/devices/28e700000.pmp/driver ] && basename "$(readlink -f /sys/bus/platform/devices/28e700000.pmp/driver)" || echo NONE )"
echo "-- report driver: $( [ -e /sys/bus/platform/devices/28e3c0000.pmp_report/driver ] && basename "$(readlink -f /sys/bus/platform/devices/28e3c0000.pmp_report/driver)" || echo NONE )"
echo "-- ane drivers: $(for f in /sys/bus/platform/devices/*ane*/driver; do [ -e "$f" ] && basename "$(readlink -f "$f")"; done | sort -u | tr '\n' ' ')"
echo "-- accel: $(ls -la /dev/accel/ 2>/dev/null | tail -n +2 | tr '\n' ' ')"
echo "-- oops: $(dmesg | grep -Ec 'Oops|BUG:|kernel panic' || true)"
echo "-- sync_state pending ane: $(cat /sys/bus/platform/devices/*ane*/state 2>/dev/null | sort | uniq -c | tr '\n' ' ')"
echo "-- root: $(findmnt -n -o SOURCE,FSTYPE /)"
echo "-- failed units: $(systemctl --failed --no-legend | wc -l)"
[ ! -e /sys/bus/platform/devices/28e700000.pmp/driver ] || { echo "IDENTITY-FAIL: pmp node has a driver bound"; exit 1; }
for f in /sys/bus/platform/devices/*ane*/driver; do
	[ -e "$f" ] || continue
	case "$(basename "$(readlink -f "$f")")" in
		ane) ;;
		*) echo "IDENTITY-FAIL: ane bound to $(readlink -f "$f")"; exit 1;;
	esac
done
dmesg | grep -Eq 'Oops|BUG:|kernel panic' && { echo "IDENTITY-FAIL: oops on boot"; exit 1; }
echo "IDENTITY-OK (oot variant $OOTSHA)"
