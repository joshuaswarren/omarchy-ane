#!/bin/bash
# OwnMemGate device window (run inside gpu-turn), from P1Groups ab-turn.sh and T6021ReleaseBoot rb-gate.sh.
# Every device process under flock /var/tmp/ane-run.lock timeout 120 (qwen_prog_run.py takes the lock itself).
# Stops at the first stop condition, no retry.
# usage: window.sh TAG [quick]   quick = add, mul, matvec 5120, encoder once (20 calls), burst
set -uo pipefail
TAG=${1:?tag}
MODE=${2:-full}
O=/var/tmp/ownmem/win-$TAG-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
R=/var/tmp/ownmem/73da8f8
RUN=$R/tools/ane-run
QPR=$R/tools/qwen_prog_run.py
PK=/var/tmp/pk-enc
QX=/var/tmp/qwen-real-anec-h14
P20=/var/tmp/prog20run
MV=/var/tmp/large-matvec-fixtures/matvec-2048-5120-m1
L=/var/tmp/ane-run.lock
P=/sys/module/ane_t6021/parameters
BAD='EXCH.*failed|completion wait|Failed to send management|mailbox.*timed out|undiscovered endpoint|ETIMEDOUT|quarantin|translation fault|dart.*(error|fault)'

state() {
	echo "== state $1 $(date -u +%FT%T.%3NZ) $TAG"
	echo "boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) up $(cut -d' ' -f1 /proc/uptime)"
	echo "module $(cat /sys/module/ane_t6021/version) $(cat /sys/module/ane_t6021/srcversion) file $(sha256sum "$(modinfo -n ane_t6021)")"
	for p in fw_alias_reserved fw_extra_ram fw_load fw_start call_settle_us bo_total_bytes bo_total_max_mb; do
		[ -e "$P/$p" ] && echo "$p $(cat "$P/$p")"
	done
	echo "modprobe.d: $(ls /etc/modprobe.d/ | tr '\n' ' ')"
	echo "dmesg_bad $(sudo -n dmesg | grep -c -i -E "$BAD") dmesg_lines $(sudo -n dmesg | wc -l)"
	grep -E '285408000' /proc/interrupts | awk -v n="$(head -1 /proc/interrupts | wc -w)" \
		'{s=0; for (i=2; i<=n+1; i++) s+=$i; print "irq", $1, "cpu-sum", s, $NF}'
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

gate() { # name op [args...]
	local n=$1 op=$2 rc
	shift 2
	flock "$L" timeout 120 bash "$R/ane/t6021/gate/gate.sh" "$O/gate-$n" "$op" "$@" >"$O/gate-$n.log" 2>&1
	rc=$?
	echo "gate $n rc=$rc $(grep -E '^GATE' "$O/gate-$n.log") $(grep -m1 -E 'lanes within' "$O/gate-$n.log")"
	stopcheck "$O/gate-$n.log" "$rc"
}

island() { # op
	local rc
	flock "$L" timeout 120 unshare -rm bash -c \
		'mount --bind /var/tmp/ownmem/73da8f8 /var/tmp/inst && exec bash /var/tmp/inst/ane/t6021/gate/gate.sh "$1" "$2"' \
		_ "$O/gate-$1" "$1" >"$O/gate-$1.log" 2>&1
	rc=$?
	echo "gate $1 rc=$rc $(grep -E '^GATE' "$O/gate-$1.log") seeds_pass=$(grep -c ' PASS' "$O/gate-$1.log")"
	stopcheck "$O/gate-$1.log" "$rc"
}

state start | tee "$O/state-start.txt"
sudo -n dmesg >"$O/dmesg-start.txt"
grep -E 'ane_t6021|284000000\.ane|BOOT-PHASE|fwload|fwalias|dart|DART|EXCH' "$O/dmesg-start.txt" >"$O/dmesg-ane.txt"
{
	omarchy-ane-check
	echo "omarchy-ane-check rc=$?"
	omarchy-ane-dt status
	echo "omarchy-ane-dt status rc=$?"
	omarchy-ane-firmware-fetch --check
	echo "firmware-fetch --check rc=$?"
} >"$O/check.txt" 2>&1
cat "$O/check.txt"

echo "== 1. gates $(date -u +%T)"
if [ "$MODE" = quick ]; then
	ops="add mul"
else
	ops="add mul relu add-scalar mul-scalar real-div-scalar clip-low clip-high"
fi
for op in $ops; do
	gate "$op" "$op"
done
gate matvec5120 matvec --anec "$MV/out/program-0.anec" --weights "$MV/model/weights.bin"
if [ "$MODE" != quick ]; then
	for op in island-c-pv island-a-kt island-a-attn-p1 island-b-select-runtime island-b-select-constfill rms-c2048-gamma; do
		island "$op"
	done
fi

echo "== 2. prog_020 $(date -u +%T)"
python3 "$QPR" --prog prog_020 --anec-dir "$QX" \
	--in t0="$P20/input-0-t0.f16" --in t2="$P20/input-1-t2.f16" --in t7="$P20/input-2-t7.f16" \
	--golden t15="$P20/golden-t15.f16" --out t15="$O/prog20-t15.f16" --work "$O/prog20-work" \
	--ane-run "$RUN" --timeout 120 --repeat 1 >"$O/prog20.log" 2>&1
rc=$?
stopcheck "$O/prog20.log" "$rc"
rm -f "$O"/prog20-work/*.surface
echo "prog_020 rc=$rc $(grep -E '^exec ms|^t15 golden' "$O/prog20.log" | tr '\n' ' ')"

echo "== 3. encoder, one process, 20 calls $(date -u +%T)"
w=$O/enc
mkdir -p "$w"
python3 "$QPR" --prog parakeet_encoder --anec-dir "$PK" --ane-run "$RUN" --timeout 120 --repeat 20 \
	--work "$w" --in attention_mask="$PK/in/attention_mask.npy" --in input_features="$PK/in/input_features.npy" \
	--out linear_217_cast_fp16="$w/hidden.npy" --out output_mask_f="$w/mask.npy" \
	--golden linear_217_cast_fp16="$PK/in/golden_hidden16.npy" >"$O/encoder.log" 2>&1
rc=$?
stopcheck "$O/encoder.log" "$rc"
rm -f "$w"/*.surface
echo "encoder rc=$rc $(grep -E 'exec ms|golden' "$O/encoder.log" | tr '\n' ' ') sha16 $(fp16sha "$w/hidden.npy")" |
	tee "$O/encoder.summary"

echo "== 4. burst 60 s $(date -u +%T)"
flock "$L" timeout 120 env ROOT="$R" GATE="$O/gate-add" bash /var/tmp/rb-burst.sh "$O/burst" 60 >"$O/burst.log" 2>&1
rc=$?
echo "burst rc=$rc $(tr '\n' ' ' <"$O/burst.log")"
stopcheck "$O/burst.log" "$rc"

state end | tee "$O/state-end.txt"
sudo -n dmesg >"$O/dmesg-end.txt"
tail -n +"$((n0 + 1))" "$O/dmesg-end.txt" >"$O/dmesg-new.txt"
echo "dmesg new lines $(wc -l <"$O/dmesg-new.txt"), non-UFW $(grep -vc 'UFW BLOCK' "$O/dmesg-new.txt"), bad new $(grep -c -i -E "$BAD" "$O/dmesg-new.txt"), bad whole boot $(grep -c -i -E "$BAD" "$O/dmesg-end.txt"), refusing whole boot $(grep -c refusing "$O/dmesg-end.txt")"
(cd "$O" && find . -maxdepth 1 -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum >SHA256SUMS)
touch "$O/DONE"
echo "== window done $(date -u +%T) $O"
