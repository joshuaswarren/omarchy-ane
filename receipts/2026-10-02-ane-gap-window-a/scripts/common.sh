# GapWinA common environment (sourced by step scripts on the M2)
G=/var/tmp/gapwin
R=$G/tree2/tools/ane-run
QPR=$G/tree2/tools/qwen_prog_run.py
PK=/var/tmp/pk-enc
E1D=$G/e1-probes
L=/var/tmp/ane-run.lock
P=/sys/module/ane_t6021/parameters
BAD='EXCH.*failed|completion wait|quarantin|DART.*fault|translation fault|mailbox.*timed out|ETIMEDOUT|Failed to send management'
CPUF=/sys/devices/system/cpu

load1() { cut -d' ' -f1 /proc/loadavg; }
psi() { awk '/^some/{print $4}' /proc/pressure/cpu; }

idle_gate() { # waits until load1<0.5 and PSI some avg10==0.00; prints "<load1> <psi> <waited_s>"; IDLE_TIMEOUT=1 on timeout (caller flags NOT-IDLE)
	local t=0
	while :; do
		if awk -v l="$(load1)" 'BEGIN{exit !(l<0.5)}' && [ "$(psi)" = "0.00" ]; then
			echo "$(load1) $(psi) $t"
			return 0
		fi
		[ $t -ge 600 ] && { echo "$(load1) $(psi) $t NOT-IDLE"; return 1; }
		sleep 10; t=$((t + 10))
	done
}

state() {
	echo "== state $1 $(date -u +%FT%T.%3NZ)"
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) up $(cut -d' ' -f1 /proc/uptime)"
	echo "module $(cat /sys/module/ane_t6021/version 2>/dev/null) $(cat /sys/module/ane_t6021/srcversion 2>/dev/null) $(sha256sum "$(modinfo -n ane_t6021)" | cut -c1-16)"
	for prm in trace_td fw_perf_mode call_settle_us bo_total_bytes; do echo "$prm $(cat $P/$prm 2>/dev/null)"; done
	echo "load $(cut -d' ' -f1-3 /proc/loadavg) psi $(head -1 /proc/pressure/cpu)"
	echo "esp $(sudo -n sha256sum /boot/efi/m1n1/boot.bin | cut -c1-16)"
	echo "dmesg_bad $(sudo -n dmesg | grep -c -i -E "$BAD") dmesg_lines $(sudo -n dmesg | wc -l)"
	pgrep -a -x ane-run || echo "no ane-run"
}

N0=$(sudo -n dmesg | wc -l)
stopcheck() { # LOG RC: stops the run on error/dmesg BAD; writes STOPPED
	local new
	new=$(sudo -n dmesg | tail -n +"$((N0 + 1))" | grep -i -E "$BAD")
	if [ "$2" != 0 ] || grep -q -E 'Connection timed out|Input/output error' "$1" || [ -n "$new" ]; then
		echo "STOP after $1: rc=$2"
		echo "$new"
		sudo -n dmesg | tail -n +"$((N0 + 1))" >"$(dirname "$1")/dmesg-new.txt"
		touch "$(dirname "$1")/STOPPED"
		return 1
	fi
	return 0
}

encblock() { # OUTDIR LOG: one 16-call encoder block; echoes "min median golden"
	local w=$1 log=$2
	mkdir -p "$w"
	python3 "$QPR" --prog parakeet_encoder --anec-dir "$PK" --ane-run "$R" --timeout 120 --repeat 16 \
		--work "$w" --in attention_mask="$PK/in/attention_mask.npy" --in input_features="$PK/in/input_features.npy" \
		--out linear_217_cast_fp16="$w/hidden.npy" --out output_mask_f="$w/mask.npy" \
		--golden linear_217_cast_fp16="$PK/in/golden_hidden16.npy" >"$log" 2>&1
}
