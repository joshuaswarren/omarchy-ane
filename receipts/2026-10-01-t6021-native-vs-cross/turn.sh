#!/bin/bash
# NativeVsCross device window on the M2 (run inside one gpu-turn ticket).
# Arms: X = Mac Studio h14 cross-compiled ANEC, N = the M2's native ANEC.
# 1. correctness: one process per arm (encoder golden + fp16 sha; Qwen
#    prog_020 / prog_006 outputs kept for the CT comparison);
# 2. encoder timing: 20 blocks per arm, interleaved X N X N, --repeat 16,
#    golden check and hidden output per block;
# 3. Qwen timing: 20 blocks per arm and program, interleaved, --repeat 16.
# Each ane-run: flock /var/tmp/ane-run.lock timeout 120 (qwen_prog_run takes
# the flock itself). Stops at the first stop condition, no retry.
set -uo pipefail
O=/var/tmp/nvc/run-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$O/enc" "$O/q"
exec > >(tee -a "$O/console.log") 2>&1
R=/var/tmp/rel-0.4.0r/tools
RUN=$R/ane-run
QPR=$R/qwen_prog_run.py
PK=/var/tmp/pk-enc
NAT=/var/tmp/native-m2-hwx
QX=/var/tmp/qwen-real-anec-h14
GS=/var/tmp/qwen38-step-goldens/step11
L=/var/tmp/ane-run.lock
P=/sys/module/ane_t6021/parameters
BAD='EXCH.*failed|completion wait|quarantin|DART.*fault|translation fault|mailbox.*timed out|ETIMEDOUT|Failed to send management'
BLOCKS=${BLOCKS:-20}

encdir() { [ "$1" = X ] && echo "$PK" || echo "$NAT"; }
qdir() { [ "$1" = X ] && echo "$QX" || echo "$NAT"; }

state() {
	echo "== state $1 $(date -u +%FT%T.%3NZ)"
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r)"
	echo "module $(cat /sys/module/ane_t6021/version) $(cat /sys/module/ane_t6021/srcversion) $(sha256sum "$(modinfo -n ane_t6021)" | cut -c1-16)"
	for p in trace_td fw_perf_mode call_settle_us bo_total_bytes bo_total_max_mb; do echo "$p $(cat $P/$p)"; done
	echo "governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
	echo "esp boot.bin $(sudo -n sha256sum /boot/efi/m1n1/boot.bin | cut -c1-16)"
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

enc() { # arm tag repeat
	local d w log rc
	d=$(encdir "$1")
	w=$O/enc/$2
	log=$O/enc/$2.log
	mkdir -p "$w"
	echo "start $(date -u +%T.%3N)" >"$log"
	python3 "$QPR" --prog parakeet_encoder --anec-dir "$d" --ane-run "$RUN" --timeout 120 --repeat "$3" \
		--work "$w" --in attention_mask="$PK/in/attention_mask.npy" --in input_features="$PK/in/input_features.npy" \
		--out linear_217_cast_fp16="$w/hidden.npy" --out output_mask_f="$w/mask.npy" \
		--golden linear_217_cast_fp16="$PK/in/golden_hidden16.npy" >>"$log" 2>&1
	rc=$?
	echo "rc=$rc end $(date -u +%T.%3N)" >>"$log"
	stopcheck "$log" "$rc"
	rm -f "$w"/*.surface
	echo "enc $2 $(grep -h -o 'min [0-9.]*\|median [0-9.]*' "$log" | tr '\n' ' ')$(grep -h -o 'golden.*' "$log") sha16 $(fp16sha "$w/hidden.npy")" |
		tee -a "$O/enc.summary"
}

qins() { # prog: --in NAME=FILE for each golden input, table order by name
	local f n
	for f in "$GS/$1"/in_*.f16; do
		n=$(basename "$f" .f16)
		printf -- '--in\n%s=%s\n' "${n#in_}" "$f"
	done
}

qcorrect() { # arm prog: one process, outputs to <work>/<name>.f16
	local d w log rc
	d=$(qdir "$1")
	w=$O/q/$2-$1
	log=$O/q/$2-$1.log
	mkdir -p "$w"
	mapfile -t ins < <(qins "$2")
	python3 "$QPR" --prog "$2" --anec-dir "$d" --ane-run "$RUN" --timeout 120 --repeat 1 --work "$w" "${ins[@]}" >"$log" 2>&1
	rc=$?
	echo "rc=$rc" >>"$log"
	stopcheck "$log" "$rc"
	echo "qcorrect $2 $1 $(grep -h -o 'min [0-9.]*' "$log")"
}

qpack() { # arm prog: pack the inputs once for the timing blocks
	local w
	w=$O/q/$2-$1-pack
	mkdir -p "$w"
	mapfile -t ins < <(qins "$2")
	python3 "$QPR" --prog "$2" --anec-dir "$(qdir "$1")" --ane-run "$RUN" --work "$w" "${ins[@]}" --pack-only >"$w.log" 2>&1 ||
		{ echo "pack $2 $1 failed"; touch "$O/STOPPED"; exit 4; }
}

qblock() { # arm prog k
	local d w log rc s packs=()
	d=$(qdir "$1")
	w=$O/q/$2-$1-pack
	log=$O/q/$2-$1-$3.log
	for s in "$w"/in-*.surface; do
		s=${s##*/}
		s=${s%.surface}
		packs+=(--in "${s#in-}=$w/$s.surface")
	done
	flock "$L" timeout 120 "$RUN" --anec "$d/$2/program-0.anec" --ports "$d/$2/ports.json" "${packs[@]}" \
		--repeat 16 --time >"$log" 2>&1
	rc=$?
	stopcheck "$log" "$rc"
	echo "$1 $3 $(grep -h -o 'min [0-9.]*\|median [0-9.]*' "$log" | awk '{printf "%s ", $2}')" >>"$O/q/$2.blocks"
}

state start | tee "$O/state-start.txt"
bo=$(cat $P/bo_total_bytes)
if [ "$bo" -ge 1610612736 ]; then
	echo "STOP: bo_total_bytes $bo >= 1.5 GiB"
	touch "$O/STOPPED"
	exit 5
fi

echo "== 1. correctness $(date -u +%T)"
enc X w-X 1
enc N w-N 1
for prog in prog_020 prog_006; do
	qcorrect X "$prog"
	qcorrect N "$prog"
done

echo "== 2. encoder timing, $BLOCKS blocks per arm, X N X N $(date -u +%T)"
for k in $(seq -w 1 "$BLOCKS"); do
	enc X "X-$k" 16
	enc N "N-$k" 16
done

echo "== 3. Qwen timing $(date -u +%T)"
for prog in prog_020 prog_006; do
	qpack X "$prog"
	qpack N "$prog"
	: >"$O/q/$prog.blocks"
	for k in $(seq -w 1 "$BLOCKS"); do
		qblock X "$prog" "$k"
		qblock N "$prog" "$k"
	done
	echo "q $prog blocks done $(date -u +%T)"
done

state end | tee "$O/state-end.txt"
sudo -n dmesg | tail -n +"$((n0 + 1))" >"$O/dmesg-new.txt"
echo "dmesg new lines $(wc -l <"$O/dmesg-new.txt"), non-UFW $(grep -vc 'UFW BLOCK' "$O/dmesg-new.txt")"
touch "$O/DONE"
echo "== window done $(date -u +%T) $O"
