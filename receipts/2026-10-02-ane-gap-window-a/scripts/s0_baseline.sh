#!/bin/bash
# GapWinA step 1: baseline, 20 blocks x 16 calls, whole encoder (NativeVsCross method).
set -uo pipefail
. /var/tmp/gapwin/bin/common.sh
O=$G/runs/s0-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$O"; cd "$O"
exec > >(tee console.log) 2>&1
state start | tee state-start.txt
bo=$(cat $P/bo_total_bytes)
[ "$bo" -lt 1610612736 ] || { echo "STOP bo_total_bytes $bo >= 1.5 GiB"; touch STOPPED; exit 5; }
: > blocks.tsv
for k in $(seq -w 1 20); do
	g=$(idle_gate) || g="$g FLAG"
	log=$O/enc$k.log
	encblock "$O/w$k" "$log"
	rc=$?
	stopcheck "$log" "$rc" || break
	mn=$(grep -o 'min [0-9.]*' "$log" | head -1 | awk '{print $2}')
	md=$(grep -o 'median [0-9.]*' "$log" | head -1 | awk '{print $2}')
	gl=$(grep -o 'golden.*' "$log" | head -1)
	echo -e "$k\t$mn\t$md\t$g\t$gl" | tee -a blocks.tsv
	rm -f "$O/w$k"/*.surface
done
state end | tee state-end.txt
sudo -n dmesg | tail -n +"$((N0 + 1))" >dmesg-new.txt
echo "dmesg new $(wc -l <dmesg-new.txt) non-UFW $(grep -vc 'UFW BLOCK' dmesg-new.txt)"
[ -f STOPPED ] || touch DONE
echo "== s0 done $O"
