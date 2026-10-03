#!/bin/bash
set -euo pipefail
# AfBridgeRun device window, reused by DartTune (only change: output under /var/tmp/dart): gates, correctness, timing, burst
# for one arm on the running boot. Method = NativeVsCross turn.sh (X arm only) plus
# the T6021ReleaseBoot gates (rb-gate.sh: add, mul, matvec 2048x5120) and burst.
# Each ane-run under flock /var/tmp/ane-run.lock timeout 120 (qwen_prog_run takes it
# itself). Stops at the first stop condition, no retry.
# usage: ab-turn.sh ARMTAG
set -uo pipefail
TAG=${1:?arm tag}
O=/var/tmp/dart/run-$TAG-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$O/enc" "$O/q"
exec > >(tee -a "$O/console.log") 2>&1
R=/var/tmp/rel-0.4.0r
RUN=$R/tools/ane-run
QPR=$R/tools/qwen_prog_run.py
PK=/var/tmp/pk-enc
QX=/var/tmp/qwen-real-anec-h14
GS=/var/tmp/qwen38-step-goldens/step11
P20=/var/tmp/prog20run
X006=/var/tmp/nvc/run-20261001T171138Z/q/prog_006-X
MV=/var/tmp/large-matvec-fixtures/matvec-2048-5120-m1
L=/var/tmp/ane-run.lock
P=/sys/module/ane_t6021/parameters
BAD='EXCH.*failed|completion wait|Failed to send management|mailbox.*timed out|undiscovered endpoint|ETIMEDOUT|quarantin|translation fault|dart.*(error|fault)'
BLOCKS=${BLOCKS:-20}
b=$(cut -c1-8 /proc/sys/kernel/random/boot_id)

state() {
	echo "== state $1 $(date -u +%FT%T.%3NZ) arm $TAG"
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) up $(cut -d' ' -f1 /proc/uptime)"
	echo "module $(cat /sys/module/ane_t6021/version) $(cat /sys/module/ane_t6021/srcversion) $(sha256sum "$(modinfo -n ane_t6021)" | cut -c1-16)"
	for p in fw_start af_bridge_macos trace_td fw_perf_mode call_settle_us bo_total_bytes bo_total_max_mb; do
		[ -e "$P/$p" ] && echo "$p $(cat "$P/$p")" || true
	done
	ls /etc/modprobe.d/ | tr '\n' ' '; echo
	echo "governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
	echo "dmesg_bad $(sudo -n dmesg | grep -c -i -E "$BAD") dmesg_lines $(sudo -n dmesg | wc -l)"
	grep -E '285408000' /proc/interrupts | tr -s ' '
	pgrep -a -x ane-run || echo "no ane-run"
	uptime
}

n0=$(sudo -n dmesg | wc -l)
stopcheck() { # log rc
	local new
	new=$(sudo -n dmesg | tail -n +"$((n0 + 1))" | grep -i -E "$BAD")
	if [ "$2" != 0 ] || grep -q -E 'Connection timed out|Input/output error' "$1" || [ -n "$new" ]; then
		echo "STOP after $1: rc=$2"
		echo "$new"
		state stop
		sudo -n dmesg | tail -n +"$((n0 + 1))" >"$O/dmesg-new.txt"
		touch "$O/STOPPED"
		exit 3
	fi
}

fp16sha() {
	python3 -c 'import sys, hashlib, numpy as np
a = np.load(sys.argv[1]); h = np.ascontiguousarray(a.astype("<f2")).tobytes()
print(hashlib.sha256(h).hexdigest())' "$1"
}

enc() { # tag repeat
	local w=$O/enc/$1 log=$O/enc/$1.log rc
	mkdir -p "$w"
	echo "start $(date -u +%T.%3N)" >"$log"
	python3 "$QPR" --prog parakeet_encoder --anec-dir "$PK" --ane-run "$RUN" --timeout 120 --repeat "$2" \
		--work "$w" --in attention_mask="$PK/in/attention_mask.npy" --in input_features="$PK/in/input_features.npy" \
		--out linear_217_cast_fp16="$w/hidden.npy" --out output_mask_f="$w/mask.npy" \
		--golden linear_217_cast_fp16="$PK/in/golden_hidden16.npy" >>"$log" 2>&1
	rc=$?
	echo "rc=$rc end $(date -u +%T.%3N)" >>"$log"
	stopcheck "$log" "$rc"
	rm -f "$w"/*.surface
	echo "enc $1 $(grep -h -o 'min [0-9.]*\|median [0-9.]*' "$log" | tr '\n' ' ')$(grep -h -o 'golden.*' "$log") sha16 $(fp16sha "$w/hidden.npy")" |
		tee -a "$O/enc.summary"
}

qins() { # prog
	local f n
	for f in "$GS/$1"/in_*.f16; do
		n=$(basename "$f" .f16)
		printf -- '--in\n%s=%s\n' "${n#in_}" "$f"
	done
}

qpack() { # prog
	local w=$O/q/$1-pack
	mkdir -p "$w"
	mapfile -t ins < <(qins "$1")
	python3 "$QPR" --prog "$1" --anec-dir "$QX" --ane-run "$RUN" --work "$w" "${ins[@]}" --pack-only >"$w.log" 2>&1 ||
		{ echo "pack $1 failed"; touch "$O/STOPPED"; exit 4; }
}

qblock() { # prog k
	local w=$O/q/$1-pack log=$O/q/$1-$2.log rc s packs=()
	for s in "$w"/in-*.surface; do
		s=${s##*/}
		s=${s%.surface}
		packs+=(--in "${s#in-}=$w/$s.surface")
	done
	rc=0
	flock "$L" timeout 120 "$RUN" --anec "$QX/$1/program-0.anec" --ports "$QX/$1/ports.json" "${packs[@]}" \
		--repeat 16 --time >"$log" 2>&1 || rc=$?
	stopcheck "$log" "$rc"
	echo "$2 $(grep -h -o 'min [0-9.]*\|median [0-9.]*' "$log" | awk '{printf "%s ", $2}')" >>"$O/q/$1.blocks"
}

state start | tee "$O/state-start.txt"

echo "== 1. gates $(date -u +%T)"
for op in add mul; do
	rc=0
	flock "$L" timeout 120 bash "$R/ane/t6021/gate/gate.sh" "$O/gate-$op" "$op" >"$O/gate-$op.log" 2>&1 || rc=$?
	echo "gate $op rc=$rc $(grep -E '^GATE' "$O/gate-$op.log")"
	stopcheck "$O/gate-$op.log" "$rc"
done
rc=0
flock "$L" timeout 120 bash "$R/ane/t6021/gate/gate.sh" "$O/gate-matvec5120" matvec \
	--anec "$MV/out/program-0.anec" --weights "$MV/model/weights.bin" >"$O/gate-matvec5120.log" 2>&1 || rc=$?
echo "gate matvec5120 rc=$rc $(grep -E '^GATE' "$O/gate-matvec5120.log") $(grep -m1 -E 'lanes within' "$O/gate-matvec5120.log")"
stopcheck "$O/gate-matvec5120.log" "$rc"

echo "== 2. correctness $(date -u +%T)"
enc w1 1
rc=0
python3 "$QPR" --prog prog_020 --anec-dir "$QX" \
	--in t0="$P20/input-0-t0.f16" --in t2="$P20/input-1-t2.f16" --in t7="$P20/input-2-t7.f16" \
	--golden t15="$P20/golden-t15.f16" --out t15="$O/q/prog20-t15.f16" --work "$O/q/prog20-work" \
	--ane-run "$RUN" --timeout 120 --repeat 1 >"$O/q/prog20-correct.log" 2>&1 || rc=$?
stopcheck "$O/q/prog20-correct.log" "$rc"
echo "prog_020 $(grep -E 'exec ms|golden' "$O/q/prog20-correct.log" | tr '\n' ' ')"
w=$O/q/prog_006-correct
mkdir -p "$w"
mapfile -t ins < <(qins prog_006)
rc=0
python3 "$QPR" --prog prog_006 --anec-dir "$QX" --ane-run "$RUN" --timeout 120 --repeat 1 --work "$w" "${ins[@]}" \
	>"$w.log" 2>&1 || rc=$?
stopcheck "$w.log" "$rc"
same=0 diff=0
for f in "$X006"/t*.f16; do
	if cmp -s "$f" "$w/$(basename "$f")"; then same=$((same + 1)); else diff=$((diff + 1)); echo "prog_006 differs: $(basename "$f")"; fi
done
echo "prog_006 vs NativeVsCross X outputs: $same byte-identical, $diff different"

echo "== 3. encoder timing, $BLOCKS blocks x 16 $(date -u +%T)"
for k in $(seq -w 1 "$BLOCKS"); do
	enc "b$k" 16
done

echo "== 4. Qwen timing $(date -u +%T)"
for prog in prog_020 prog_006; do
	qpack "$prog"
	: >"$O/q/$prog.blocks"
	for k in $(seq -w 1 "$BLOCKS"); do
		qblock "$prog" "$k"
	done
	echo "q $prog blocks done $(date -u +%T)"
done

echo "== 5. burst 60 s $(date -u +%T)"
rc=0
flock "$L" timeout 120 env ROOT="$R" GATE="$O/gate-add" bash /var/tmp/rb-burst.sh "$O/burst" 60 >"$O/burst.log" 2>&1 || rc=$?
echo "burst rc=$rc $(tr '\n' ' ' <"$O/burst.log")"
stopcheck "$O/burst.log" "$rc"

state end | tee "$O/state-end.txt"
sudo -n dmesg | tail -n +"$((n0 + 1))" >"$O/dmesg-new.txt"
echo "dmesg new lines $(wc -l <"$O/dmesg-new.txt"), non-UFW $(grep -vc 'UFW BLOCK' "$O/dmesg-new.txt"), bad $(grep -c -i -E "$BAD" "$O/dmesg-new.txt")"
touch "$O/DONE"
echo "== window done $(date -u +%T) $O"
