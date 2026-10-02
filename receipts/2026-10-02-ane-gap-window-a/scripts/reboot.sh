#!/bin/bash
# GapWinA (CT side): queue the M2 reboot in its own gpu-turn ticket (prereboot.sh, IN_TICKET=1),
# wait for T0 or ssh drop, then 6 min for a new boot ID.
# usage: reboot.sh TAG   (0 = new boot up; 3 = locks busy; 4 = ESP not lab; 1 = no answer in 6 min)
set -u
TAG=${1:?tag}
M=/var/tmp/gapwin
S="ssh -o ConnectTimeout=8 -o BatchMode=yes jw14m2-linux"
old=$($S 'cat /proc/sys/kernel/random/boot_id') || { echo "no ssh before reboot"; exit 2; }
echo "old boot $old $(date -u +%T)"
timeout 20 $S -n "setsid nohup env IN_TICKET=1 /home/joshuawarren/bin/gpu-turn -m 5 -- bash $M/bin/prereboot.sh $M/runs/$TAG-pre >$M/log/reboot-$TAG.log 2>&1 </dev/null &"
start=$(date -u +%s)
while (($(date -u +%s) - start < 3600)); do
	out=$($S "cat $M/log/reboot-$TAG.log" 2>/dev/null) || { echo "ssh dropped $(date -u +%T)"; break; }
	if grep -qE 'LOCKS-BUSY|ESP-NOT-LAB' <<<"$out"; then
		echo "$out"
		grep -q ESP-NOT-LAB <<<"$out" && exit 4
		exit 3
	fi
	if grep -q '^T0 ' <<<"$out"; then
		grep '^T0 ' <<<"$out"
		break
	fi
	sleep 10
done
w0=$(date +%s)
while (($(date +%s) - w0 < 360)); do
	id=$($S 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null)
	if [ -n "$id" ] && [ "$id" != "$old" ]; then
		echo "UP $(date -u +%T) after $(($(date +%s) - w0)) s, boot $id"
		exit 0
	fi
	sleep 10
done
echo "NO-ANSWER after 360 s ($(date -u +%T)): STOP, tell Main (no hard reset by GapWinA)"
exit 1
