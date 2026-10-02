#!/bin/bash
# GapWinA step 3: cpufreq sweep — pin policies 0/4/8, 7 points, 3 blocks x 16 calls each.
# ane_boost: not present in ane_t6021.ko (no param, no QoS); verified separately.
set -uo pipefail
. /var/tmp/gapwin/bin/common.sh
O=$G/runs/s2-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$O"; cd "$O"
exec > >(tee console.log) 2>&1
state start | tee state-start.txt

pol() { echo "$CPUF/cpu/cpu$1/cpufreq"; }
: > originals.txt
for p in 0 4 8; do
	echo "policy$p min $(cat "$(pol $p)/scaling_min_freq") max $(cat "$(pol $p)/scaling_max_freq") gov $(cat "$(pol $p)/scaling_governor")" | tee -a originals.txt
done

PP=(702000 948000 1452000 1968000 2400000 3000000 3264000)
PE=(912000 912000 1284000 2004000 2424000 2424000 2424000)
: > points.tsv
for i in "${!PP[@]}"; do
	# points are strictly increasing: max first, then min
	for p in 0 4 8; do
		case $p in 0) echo "${PE[$i]}" >"$(pol 0)/scaling_max_freq"; echo "${PE[$i]}" >"$(pol 0)/scaling_min_freq";;
			*) echo "${PP[$i]}" >"$(pol $p)/scaling_max_freq"; echo "${PP[$i]}" >"$(pol $p)/scaling_min_freq";; esac
	done
	echo "== point $((i + 1)) P=${PP[$i]} E=${PE[$i]} $(date -u +%T) pinned"
	echo "${PP[$i]} ${PE[$i]}" >>points.tsv
	sleep 20
	# frequency sampler: 25 s at 20 ms
	( end=$((SECONDS + 25))
		while [ $SECONDS -lt $end ]; do
			echo "$(date +%s.%3N) $(cat $CPUF/cpu0/cpufreq/scaling_cur_freq) $(cat $CPUF/cpu4/cpufreq/scaling_cur_freq) $(cat $CPUF/cpu8/cpufreq/scaling_cur_freq)"
			sleep 0.02
		done ) >freq$i.tsv &
	SAMPLER=$!
	: >blocks_$i.tsv
	for k in 1 2 3; do
		g=$(idle_gate) || g="$g FLAG"
		log=$O/pt$i-$k.log
		encblock "$O/w-pt$i-$k" "$log"
		rc=$?
		stopcheck "$log" "$rc" && { mn=$(grep -o 'min [0-9.]*' "$log" | head -1 | awk '{print $2}'); md=$(grep -o 'median [0-9.]*' "$log" | head -1 | awk '{print $2}'); gl=$(grep -o 'golden.*' "$log" | head -1); echo -e "$k\t$mn\t$md\t$g\t$gl" | tee -a blocks_$i.tsv; } || break
		rm -f "$O/w-pt$i-$k"/*.surface
	done
	wait $SAMPLER 2>/dev/null
	[ -f STOPPED ] && break
done

echo "== restore $(date -u +%T)"
for p in 0 4 8; do
	omin=$(awk -v p=$p '$1=="policy"p{print $2}' originals.txt)
	omax=$(awk -v p=$p '$1=="policy"p{print $4}' originals.txt)
	echo "$omax" >"$(pol $p)/scaling_max_freq"
	echo "$omin" >"$(pol $p)/scaling_min_freq"
done
: > restored.txt
for p in 0 4 8; do
	echo "policy$p min $(cat "$(pol $p)/scaling_min_freq") max $(cat "$(pol $p)/scaling_max_freq")" | tee -a restored.txt
done

echo "== ane_boost evidence $(date -u +%T)"
modinfo ane_t6021 | grep -c 'parm=.*boost' || echo "no boost param"
ls "$P" | grep -i boost || echo "no boost sysfs param"
lsmod | grep -E '^ane ' || echo "ane.ko (H13, owns ane_boost) not loaded"

state end | tee state-end2.txt
sudo -n dmesg | tail -n +"$((N0 + 1))" >dmesg-new.txt
echo "dmesg new $(wc -l <dmesg-new.txt) non-UFW $(grep -vc 'UFW BLOCK' dmesg-new.txt)"
[ -f STOPPED ] || touch DONE
echo "== s2 done $O"
