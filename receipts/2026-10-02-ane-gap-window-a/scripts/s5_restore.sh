#!/bin/bash
# GapWinA step 6 restore (module + cpufreq), run before the final reboot.
set -uo pipefail
. /var/tmp/gapwin/bin/common.sh
O=$G/runs/s5-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$O"; cd "$O"
exec > >(tee console.log) 2>&1

echo "== cpufreq restore $(date -u +%T)"
cp $G/runs/s2-*/originals.txt originals.txt 2>/dev/null || { echo "no sweep originals found"; exit 3; }
for p in 0 4 8; do
	omin=$(awk -v p=$p '$1=="policy"p{print $3}' originals.txt)
	omax=$(awk -v p=$p '$1=="policy"p{print $5}' originals.txt)
	echo "$omax" >$CPUF/cpu/cpu$p/cpufreq/scaling_max_freq
	echo "$omin" >$CPUF/cpu/cpu$p/cpufreq/scaling_min_freq
done
for p in 0 4 8; do
	echo "policy$p min $(cat $CPUF/cpu/cpu$p/cpufreq/scaling_min_freq) max $(cat $CPUF/cpu/cpu$p/cpufreq/scaling_max_freq)"
done | tee cpufreq-restored.txt

echo "== module restore $(date -u +%T)"
K=$(uname -r)
sha256sum $G/backup/ane_t6021.ko
grep -q '^af2cee6c3962e642d3de485131a118b71852c9d215503757efa626795a9b3b7a  ' $G/backup/ane_t6021.ko.sha256 || { echo "STOP backup module hash mismatch"; exit 4; }
sudo -n cp $G/backup/ane_t6021.ko /lib/modules/$K/updates/ane_t6021.ko
sudo -n depmod -a
sha256sum /lib/modules/$K/updates/ane_t6021.ko | tee module-restored.txt
modinfo -F srcversion ane_t6021
echo "== s5 done (reboot queued separately)"
touch DONE
