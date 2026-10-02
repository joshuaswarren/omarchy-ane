#!/bin/bash
# Operator side of the return (MacWinRun return-linux.sh; the old boot id is now a parameter, it
# was hardcoded). `sudo reboot` FROM macOS (BootNext is consumed, the default Linux volume boots),
# then up to 6 min for a Linux boot id different from OLD_BOOT. Every macOS ssh runs under
# `timeout` (window 1: the WiFi route hung without it).
# exit 0 = Linux up with a new boot id; 1 = no answer in 6 min (STOP, tell Main); 2 = no reboot issued
#   LINUX=<linux alias> OLD_BOOT=<boot id before the macOS entry> bash return-linux.sh MACALIAS [ssh opts]
set -u
LINUX=${LINUX:?} OLD_BOOT=${OLD_BOOT:?}
MAC=("$@")
mac() { timeout 30 ssh -o ConnectTimeout=8 -o BatchMode=yes "${MAC[@]:1}" "${MAC[0]}" "$@"; }
old=$(mac 'sysctl -n kern.boottime') || { echo "macOS not answering"; exit 2; }
echo "macOS boottime $old; T0 $(date -u +%FT%TZ) sudo reboot"
mac 'sync; sudo -n reboot' || true
w0=$(date +%s)
while (($(date +%s) - w0 < 360)); do
	id=$(timeout 20 ssh -o ConnectTimeout=8 -o BatchMode=yes "$LINUX" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null)
	if [ -n "$id" ] && [ "$id" != "$OLD_BOOT" ]; then
		echo "UP $(date -u +%T) after $(($(date +%s) - w0)) s, boot $id"
		exit 0
	fi
	sleep 10
done
echo "macOS route: $(mac 'hostname; sysctl -n kern.boottime' 2>&1 | tr '\n' ' ')"
echo "NO-ANSWER after 360 s ($(date -u +%T)) on $LINUX: STOP, tell Main (no hard reset)"
exit 1
