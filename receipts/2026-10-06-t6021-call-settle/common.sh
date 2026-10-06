# Sourced by before.sh and after.sh. Runs on the M2 inside one gpu-turn ticket.
set -uo pipefail
S=/var/tmp/keep-ane/settle
R=/var/tmp/keep-ane/libane-fix/tree
L=/var/tmp/ane-run.lock
P=/sys/module/ane_t6021/parameters/call_settle_us
ADD=$R/fixtures/h14-anec/add/program-0.anec
ENC=/var/tmp/keep-ane/pk-enc
Q20=/var/tmp/prog20run
bad='EXCH.*failed|completion wait|Failed to send management|mailbox.*timed out|undiscovered endpoint|ETIMEDOUT|quarantin|translation fault|dart.*(error|fault)'

lock() { flock -w 60 "$L" timeout 120 "$@"; }

state() {
	date -u +%FT%TZ
	uname -r
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
	sha256sum "$(modinfo -n ane_t6021)" "$R/tools/ane-run" "$S/landing"
	for p in call_settle_us trace_td stats fw_perf_mode bo_total_bytes; do
		echo "$p $(cat /sys/module/ane_t6021/parameters/$p 2>/dev/null)"
	done
	uptime
	head -1 /proc/pressure/cpu
	df -h / | tail -1
	cat /sys/class/accel/accel*/device/ane_stats 2>/dev/null | head -5
}

# Timing only at load1 < 0.5 and PSI cpu some avg10 0.00; wait up to 90 s.
quiet() {
	for _ in $(seq 90); do
		awk '{exit !($1 < 0.5)}' /proc/loadavg &&
			grep -q '^some avg10=0.00 ' /proc/pressure/cpu && return 0
		sleep 1
	done
	echo "NOT QUIET: $(cat /proc/loadavg) $(head -1 /proc/pressure/cpu)"
	return 1
}

# Stop rule: any new bad dmesg line since the snapshot $1.
dmesg_ok() {
	sudo -n dmesg >"$1.now"
	tail -n +"$(($(wc -l <"$1") + 1))" "$1.now" >"$1.new"
	if grep -i -E "$bad" "$1.new"; then
		echo "STOP: bad dmesg line"
		return 1
	fi
}

# 7 runs of 200 add calls on the fixed inputs; output of run i in $1-i.f16.
add_runs() {
	for i in 1 2 3 4 5 6 7; do
		lock "$R/tools/ane-run" --anec "$ADD" --in 0="$S/a.f16" --in 1="$S/b.f16" \
			--out 0="$1-$i.f16" --check add --repeat 200 --time || return 1
	done
}

# qwen_prog_run.py takes $L around its own ane-run, so no lock here.
prog20() {
	timeout 150 python3 "$R/tools/qwen_prog_run.py" --prog prog_020 --anec-dir /var/tmp/keep-ane/anec-h14 \
		--in t0=$Q20/input-0-t0.f16 --in t2=$Q20/input-1-t2.f16 --in t7=$Q20/input-2-t7.f16 \
		--golden t15=$Q20/golden-t15.f16 --out t15="$1" --work "$S/prog20-work" \
		--ane-run "$R/tools/ane-run" --timeout 60 --repeat 1
}

encoder() {
	lock "$R/tools/ane-run" --anec $ENC/parakeet_encoder/program-0.anec \
		--ports $ENC/parakeet_encoder/ports.json \
		--in attention_mask=$ENC/in/attention_mask_tilepad.bin \
		--in input_features=$ENC/in/input_features_tilepad.bin \
		--out linear_217_cast_fp16="$1" --out output_mask_f="$1.mask" \
		--repeat 20 --time || return 1
	python3 - "$1" <<'EOF'
import hashlib, sys, numpy as np
b = open(sys.argv[1], "rb").read()[:480000]
g = np.load("/var/tmp/keep-ane/pk-enc/in/golden_hidden16.npy").tobytes()
print(f"encoder golden fp16 fca96f13 bit-exact: {b == g} sha256 {hashlib.sha256(b).hexdigest()}")
sys.exit(b != g)
EOF
}
