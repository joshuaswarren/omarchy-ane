#!/bin/bash
# DartTune shared helpers for the M2 windows (sourced). Needs O (outdir) set.
K=/var/tmp/dart/2f943fe/src/ane/t6021/probes/ane_dart_probe.ko
KSHA=3227a0507dfced0aaa4eb8abd1afacab1f1f5559c4f420b3cf2e0fbe59f6590c
R=/var/tmp/rel-0.4.0r
QPR=$R/tools/qwen_prog_run.py
PK=/var/tmp/pk-enc
L=/var/tmp/ane-run.lock
BAD='EXCH.*failed|completion wait|Failed to send management|mailbox.*timed out|undiscovered endpoint|ETIMEDOUT|quarantin|translation fault|dart.*(error|fault)|SError|Internal error'
BOOT=$(cat /proc/sys/kernel/random/boot_id)
N0=$(sudo -n dmesg | wc -l)
mkdir -p "$O/enc"

state() {
	echo "== state $1 $(date -u +%FT%T.%3NZ)"
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) up $(cut -d' ' -f1 /proc/uptime)"
	echo "module $(cat /sys/module/ane_t6021/version) $(cat /sys/module/ane_t6021/srcversion) $(sha256sum "$(modinfo -n ane_t6021)" | cut -c1-16)"
	lsmod | grep -E '^(ane_t6021|ane_dart_probe) '
	ls /etc/modprobe.d/ | tr '\n' ' '
	echo
	ls /dev/accel/
	echo "dmesg_bad $(sudo -n dmesg | grep -c -i -E "$BAD") dmesg_lines $(sudo -n dmesg | wc -l)"
	pgrep -a -x ane-run || echo "no ane-run"
	uptime
}

stop() {
	echo "STOP: $*"
	state stop
	sudo -n dmesg | tail -n +"$((N0 + 1))" >"$O/dmesg-new.txt"
	touch "$O/STOPPED"
	exit 3
}

badcheck() {
	local new
	new=$(sudo -n dmesg | tail -n +"$((N0 + 1))" | grep -i -E "$BAD")
	[ -z "$new" ] || stop "bad dmesg: $new"
	[ "$(cat /proc/sys/kernel/random/boot_id)" = "$BOOT" ] || stop "boot_id changed"
}

# probe TAG [insmod params...]: one insmod of the probe under the ANE lock (refused if an
# ane-run runs), rmmod of the PROBE only, its dmesg lines to $O/TAG.log.
probe() {
	local t=$1 m0 rc
	shift
	echo "$KSHA  $K" | sha256sum -c --quiet - || stop "probe sha"
	sync
	m0=$(sudo -n dmesg | wc -l)
	echo "probe $t $* start $(date -u +%T.%3N)"
	flock "$L" timeout 120 bash -c '! pgrep -x ane-run >/dev/null || { echo "ane-run running"; exit 9; }; sudo -n insmod "$0" "$@"' "$K" "$@"
	rc=$?
	echo "probe $t insmod rc=$rc end $(date -u +%T.%3N)"
	if lsmod | grep -q '^ane_dart_probe '; then
		sudo -n rmmod ane_dart_probe
		echo "probe $t rmmod rc=$?"
	fi
	sudo -n dmesg | tail -n +"$((m0 + 1))" | grep 'ane_dart_probe' >"$O/$t.log"
	tail -1 "$O/$t.log"
	[ "$rc" = 0 ] || stop "probe $t rc=$rc"
	badcheck
}

fp16sha() {
	python3 -c 'import sys, hashlib, numpy as np
a = np.load(sys.argv[1]); h = np.ascontiguousarray(a.astype("<f2")).tobytes()
print(hashlib.sha256(h).hexdigest())' "$1"
}

# enc TAG REPEAT: one encoder process (qwen_prog_run takes the ANE lock), golden check.
enc() {
	local w=$O/enc/$1 log=$O/enc/$1.log rc
	mkdir -p "$w"
	echo "start $(date -u +%T.%3N)" >"$log"
	python3 "$QPR" --prog parakeet_encoder --anec-dir "$PK" --ane-run "$R/tools/ane-run" --timeout 120 --repeat "$2" \
		--work "$w" --in attention_mask="$PK/in/attention_mask.npy" --in input_features="$PK/in/input_features.npy" \
		--out linear_217_cast_fp16="$w/hidden.npy" --out output_mask_f="$w/mask.npy" \
		--golden linear_217_cast_fp16="$PK/in/golden_hidden16.npy" >>"$log" 2>&1
	rc=$?
	echo "rc=$rc end $(date -u +%T.%3N)" >>"$log"
	[ "$rc" = 0 ] || stop "enc $1 rc=$rc"
	rm -f "$w"/*.surface
	echo "enc $1 $(grep -h -o 'min [0-9.]*\|median [0-9.]*' "$log" | tr '\n' ' ')$(grep -h -o 'golden.*' "$log") sha16 $(fp16sha "$w/hidden.npy")" |
		tee -a "$O/enc.summary"
	badcheck
}
