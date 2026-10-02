#!/bin/bash
# Operator side of the macOS entry (MacWinRun reboot-mac.sh; aliases and paths are parameters).
# Queues m2/prereboot-mac.sh on the M2 Linux in its own gpu-turn ticket, waits for T0 or the ssh
# drop, then up to 6 min for macOS on each alias (ssh identity, never ping).
# exit 0 = macOS up; 3 = locks busy; 4 = ESP not lab; 5 = BootNext bad; 1 = no answer in 6 min
# (STOP, tell Main; no hard reset); 2 = no Linux ssh before the reboot.
#   LINUX=<linux alias> MACS="<macos alias> <second macos alias>" M=<M2 scratch with bin/> \
#   ESP_SHA=<lab boot.bin sha256> bash reboot-mac.sh
set -u
LINUX=${LINUX:?} MACS=${MACS:?} M=${M:?} ESP_SHA=${ESP_SHA:?}
S="ssh -o ConnectTimeout=8 -o BatchMode=yes $LINUX"
old=$($S 'cat /proc/sys/kernel/random/boot_id') || { echo "no ssh before reboot"; exit 2; }
echo "old boot $old $(date -u +%T)"
timeout 20 $S -n "setsid nohup env IN_TICKET=1 ESP_SHA=$ESP_SHA ~/bin/gpu-turn -m 5 -- bash $M/bin/prereboot-mac.sh $M/runs/mac-pre >$M/log/reboot-mac.log 2>&1 </dev/null &"
start=$(date -u +%s)
while (($(date -u +%s) - start < 1800)); do
	out=$($S "cat $M/log/reboot-mac.log" 2>/dev/null) || { echo "ssh dropped $(date -u +%T)"; break; }
	if grep -qE 'LOCKS-BUSY|ESP-NOT-LAB|BOOTNEXT-BAD' <<<"$out"; then
		echo "$out"
		grep -q ESP-NOT-LAB <<<"$out" && exit 4
		grep -q BOOTNEXT-BAD <<<"$out" && exit 5
		exit 3
	fi
	grep -q '^T0 ' <<<"$out" && { echo "$out"; break; }
	sleep 5
done
w0=$(date +%s)
while (($(date +%s) - w0 < 360)); do
	for h in $MACS; do
		id=$(timeout 20 ssh -o ConnectTimeout=8 -o BatchMode=yes "$h" 'echo "$(hostname) $(sysctl -n hw.model) $(sw_vers -productVersion) $(sw_vers -buildVersion) boottime $(sysctl -n kern.boottime)"' 2>/dev/null)
		if [ -n "$id" ]; then
			echo "UP $(date -u +%T) after $(($(date +%s) - w0)) s via $h: $id"
			exit 0
		fi
	done
	sleep 10
done
echo "NO-ANSWER after 360 s ($(date -u +%T)) on $MACS: STOP, tell Main (no hard reset)"
exit 1
