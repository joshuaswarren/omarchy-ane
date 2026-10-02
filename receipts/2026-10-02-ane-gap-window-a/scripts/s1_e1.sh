#!/bin/bash
# GapWinA step 2: E1 Linux leg — P6 (compute-bound conv, 8.59e9 MAC) and P7 (32 MiB add),
# 20 blocks x 16 calls each + one trace_td block each (clock-re receipt commands_linux).
set -uo pipefail
. /var/tmp/gapwin/bin/common.sh
O=$G/runs/s1-$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$O"; cd "$E1D"
exec > >(tee "$O/console.log") 2>&1
state start | tee "$O/state-start.txt"

runblocks() { # TAG PROG INS outs goldens...
	local tag=$1 prog=$2 shift2=$3 log rc mn md gl w=$O/$tag k g
	: >"$O/$tag.blocks.tsv"
	for k in $(seq -w 1 20); do
		g=$(idle_gate) || g="$g FLAG"
		log=$O/$tag-$k.log
		mkdir -p "$w$k"
		case $prog in
		p6_conv64)
			python3 "$QPR" --prog p6_conv64 --anec-dir "$E1D/anec" --ane-run "$R" --timeout 120 --repeat 16 \
				--work "$w$k" --in x=p6/in/x.npy --out y63="$w$k/y63.npy" --golden y63=p6/in/golden.npy >"$log" 2>&1 ;;
		p7_add32m)
			python3 "$QPR" --prog p7_add32m --anec-dir "$E1D/anec" --ane-run "$R" --timeout 120 --repeat 16 \
				--work "$w$k" --in x=p7/in/x.npy --in z=p7/in/z.npy --out y="$w$k/y.npy" --golden y=p7/in/golden.npy >"$log" 2>&1 ;;
		esac
		rc=$?
		stopcheck "$log" "$rc" || return 1
		mn=$(grep -o 'min [0-9.]*' "$log" | head -1 | awk '{print $2}')
		md=$(grep -o 'median [0-9.]*' "$log" | head -1 | awk '{print $2}')
		gl=$(grep -o 'golden.*' "$log" | head -1)
		echo -e "$k\t$mn\t$md\t$g\t$gl" | tee -a "$O/$tag.blocks.tsv"
		rm -f "$w$k"/*.surface
	done
}

for prog in p6_conv64 p7_add32m; do
	echo "== E1 $prog blocks $(date -u +%T)"
	runblocks "$prog" "$prog" || { [ -f STOPPED ] && break; }
done

# trace_td blocks (param flip at runtime; excluded from timing)
if [ ! -f STOPPED ] && [ -w "$P/trace_td" ]; then
	for prog in p6_conv64 p7_add32m; do
		echo "== trace_td block $prog $(date -u +%T)"
		echo 1 >"$P/trace_td"
		w=$O/$prog-trace; mkdir -p "$w"
		log=$O/$prog-trace.log
		case $prog in
		p6_conv64) python3 "$QPR" --prog p6_conv64 --anec-dir "$E1D/anec" --ane-run "$R" --timeout 120 --repeat 16 \
			--work "$w" --in x=p6/in/x.npy --out y63="$w/y63.npy" --golden y63=p6/in/golden.npy >"$log" 2>&1 ;;
		p7_add32m) python3 "$QPR" --prog p7_add32m --anec-dir "$E1D/anec" --ane-run "$R" --timeout 120 --repeat 16 \
			--work "$w" --in x=p7/in/x.npy --in z=p7/in/z.npy --out y="$w/y.npy" --golden y=p7/in/golden.npy >"$log" 2>&1 ;;
		esac
		rc=$?
		stopcheck "$log" "$rc"
		sudo -n cat /sys/kernel/debug/ane_t6021/trace_td >"$O/$prog.trace_td.txt" 2>&1 || true
		echo 0 >"$P/trace_td"
		grep -c . "$O/$prog.trace_td.txt" || true
	done
else
	[ -f STOPPED ] || echo "trace_td param not writable at runtime: skipping trace blocks"
fi

state end | tee "$O/state-end.txt"
sudo -n dmesg | tail -n +"$((N0 + 1))" >"$O/dmesg-new.txt"
echo "dmesg new $(wc -l <"$O/dmesg-new.txt") non-UFW $(grep -vc 'UFW BLOCK' "$O/dmesg-new.txt")"
[ -f STOPPED ] || touch "$O/DONE"
echo "== s1 done $O"
