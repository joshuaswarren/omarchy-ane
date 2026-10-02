#!/bin/bash
# GapWinA step 4: E2 contention — A quiet, B memcpy hog, ABABAB, encoder + matvec per arm.
set -uo pipefail
. /var/tmp/gapwin/bin/common.sh
O=$G/runs/s3-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$O"; cd "$O"
exec > >(tee console.log) 2>&1
state start | tee state-start.txt
up_s=$(cut -d' ' -f1 /proc/uptime)
[ "${up_s%%.*}" -ge 1500 ] || echo "WARN uptime ${up_s}s < 25 min at first arm"

MV_ANEC=$G/tree2/fixtures/h14-anec/matvec/program-0.anec
# one gate-style matvec check (trimmed weights: skip the 128-byte blob header)
dd if=/var/tmp/large-matvec-fixtures/matvec-2048-5120-m1/model/weights.bin of=$O/mv-weights.fp16 bs=128 skip=1 status=none
TRIALS=1 bash $G/tree2/ane/t6021/gate/gate.sh $O/gate-mv matvec --weights $O/mv-weights.fp16 || { echo "STOP gate matvec failed"; touch STOPPED; exit 3; }
IN=$O/gate-mv/in-a-1.fp16
gcc -O2 -o $O/hog $G/bin/hog.c -lpthread && echo "hog built"

: > arms.tsv
for arm in A B A B A B; do
	echo "== arm $arm $(date -u +%T)"
	g=$(idle_gate) || g="$g FLAG"   # gate immediately BEFORE the hog starts
	HOGPID=""
	if [ "$arm" = B ]; then
		$O/hog $O/hog-$arm.log & HOGPID=$!
		sleep 3   # let the controller settle
		echo "hog pid $HOGPID rate $(tail -1 $O/hog-$arm.log)"
	fi
	: >blocks.tsv
	for k in 1 2 3 4; do
		log=$O/enc-$arm-$k.log
		encblock "$O/w-enc-$arm-$k" "$log"
		rc=$?
		l1=$(load1); ps=$(psi)
		stopcheck "$log" "$rc" || { [ -n "$HOGPID" ] && kill $HOGPID; break; }
		mn=$(grep -o 'min [0-9.]*' "$log" | head -1 | awk '{print $2}')
		md=$(grep -o 'median [0-9.]*' "$log" | head -1 | awk '{print $2}')
		gl=$(grep -o 'golden.*' "$log" | head -1)
		echo -e "enc-$arm\t$k\t$mn\t$md\t$l1\t$ps\t$gl" | tee -a blocks.tsv
		rm -f "$O/w-enc-$arm-$k"/*.surface
	done
	for k in 1 2 3 4; do
		log=$O/mv-$arm-$k.log
		flock "$L" timeout 120 "$R" --anec "$MV_ANEC" --in 0="$IN" --out 0="$O/mv-out-$arm-$k.fp16" --repeat 16 --time >"$log" 2>&1
		rc=$?
		l1=$(load1); ps=$(psi)
		stopcheck "$log" "$rc" || { [ -n "$HOGPID" ] && kill $HOGPID; break; }
		mn=$(grep -o 'min [0-9.]*' "$log" | head -1 | awk '{print $2}')
		md=$(grep -o 'median [0-9.]*' "$log" | head -1 | awk '{print $2}')
		echo -e "mv-$arm\t$k\t$mn\t$md\t$l1\t$ps" | tee -a blocks.tsv
	done
	if [ -n "$HOGPID" ]; then
		kill $HOGPID 2>/dev/null; wait $HOGPID 2>/dev/null
		echo "hog end rate $(tail -1 $O/hog-$arm.log)"
	fi
	echo -e "$arm\t$g" >>arms.tsv
	[ -f STOPPED ] && break
done

state end | tee state-end.txt
sudo -n dmesg | tail -n +"$((N0 + 1))" >dmesg-new.txt
echo "dmesg new $(wc -l <dmesg-new.txt) non-UFW $(grep -vc 'UFW BLOCK' dmesg-new.txt)"
[ -f STOPPED ] || touch DONE
echo "== s3 done $O"
