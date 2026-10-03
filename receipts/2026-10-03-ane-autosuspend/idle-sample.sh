#!/usr/bin/env bash
set -euo pipefail
# Sample idle power and ANE power state once per second.
#   sudo HW=/sys/class/hwmon/hwmonN ./idle-sample.sh ARM SECONDS > ARM.tsv
# HW: the hwmon whose power1_input is total system power (macsmc).
# Columns: epoch, total_uW, ANE runtime_status, ANE genpd states.
arm=$1 n=$2
: "${HW:?set HW to the macsmc hwmon directory}"
GENPD=${GENPD:-/sys/kernel/debug/pm_genpd/pm_genpd_summary}
dev=$(readlink -f /sys/class/accel/accel0/device 2>/dev/null || true)
for ((i = 0; i < n; i++)); do
	st=unbound
	[ -n "$dev" ] && st=$(cat "$dev/power/runtime_status") || st=unbound
	pd=$(awk '/^ane/ { printf "%s=%s,", $1, $2 }' "$GENPD" 2>/dev/null) ||
		pd=none
	printf '%s\t%s\t%s\t%s\t%s\n' "$arm" "$EPOCHREALTIME" \
		"$(cat "$HW/power1_input")" "$st" "$pd"
	sleep 1
done
