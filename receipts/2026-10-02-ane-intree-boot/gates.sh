#!/bin/bash
# shellcheck disable=SC2024 # sudo output goes to the caller's own files in STAGEDIR
# Post-boot gates of the in-tree test. Read only, except the ANE jobs, which run one at a time under
# flock /var/tmp/ane-run.lock and stop at the first failure or new bad kernel line (no retry).
# Run inside the host's GPU-lock wrapper (M2 Max: gpu-turn -m 30 --; M1 Max: its maintenance window).
# usage: gates.sh STAGEDIR test|none|intree [OMARCHY_ANE_ROOT]
#   test, none  fallback proof: stock kernel, one-shot marker as expected, grubenv consumed (ANE bind reported)
#   intree      identity, in-tree modules, bind, DRM ABI, firmware, IRQ, dmesg, omarchy-ane-check, ane-run
#               gates (T602x), bit-exact encoder (if its fixture is staged on this host)
#   OMARCHY_ANE_ROOT  omarchy-ane tree with tools/ built on this host (default /var/tmp/ownmem/b6ef8f1)
# Exit 0 PASS, 1 FAIL, 4 INCOMPLETE (a required item could not run on this host).
set -uo pipefail
S=$(realpath "${1:?stagedir}")
WANT=${2:?test|none|intree}
ROOT=${3:-/var/tmp/ownmem/b6ef8f1}
REL=$(cat "$S/release")
O=$S/g-$WANT-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$O"
exec > >(tee -a "$O/console.log") 2>&1
L=/var/tmp/ane-run.lock
ENC16=fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063
BAD='EXCH.*failed|completion wait|Failed to send management|mailbox.*timed out|undiscovered endpoint|ETIMEDOUT|quarantin|translation fault|dart.*(error|fault)|tm completion'
CRASH='Oops|BUG:|Unable to handle|Kernel panic|Call trace'
fails=() notrun=()
check() { # name command...
	local n=$1
	shift
	if "$@"; then echo "PASS $n"; else echo "FAIL $n"; fails+=("$n"); fi
}
skip() { echo "NOT-RUN $1: $2"; notrun+=("$1"); }
finish() {
	(cd "$O" && find . -type f ! -name SHA256SUMS ! -name console.log -print0 | sort -z | xargs -0 -r sha256sum) >"$O/files.sha256"
	echo "== summary $(date -u +%FT%TZ) $O"
	[ ${#notrun[@]} = 0 ] || echo "not run: ${notrun[*]}"
	if [ ${#fails[@]} != 0 ]; then
		echo "GATES FAIL: ${fails[*]}"
		exit 1
	elif [ ${#notrun[@]} != 0 ]; then
		echo "GATES INCOMPLETE"
		exit 4
	fi
	echo "GATES PASS"
	exit 0
}
n0=$(sudo -n dmesg | wc -l)
newbad() { sudo -n dmesg | tail -n +"$((n0 + 1))" | grep -i -E "$BAD|$CRASH"; }
job() { # name log command... : one ANE job; stop all ANE work on failure
	local n=$1 log=$2 rc
	shift 2
	"$@" >"$log" 2>&1
	rc=$?
	echo "job $n rc=$rc $(grep -E '^GATE|exec ms|golden' "$log" | tr '\n' ' ')"
	if [ "$rc" != 0 ] || grep -q -E 'Connection timed out|Input/output error' "$log" || newbad >"$O/newbad-$n.txt"; then
		check "$n" false
		echo "STOP after $n: no further ANE job"
		sudo -n dmesg | tail -n +"$((n0 + 1))" >"$O/dmesg-new.txt"
		finish
	fi
	check "$n" true
}
loadgate() { # wait up to 15 min for load1 < 0.5 and cpu PSI some avg10 = 0 (fleet rule for timing)
	local l1 p
	for _ in $(seq 90); do
		read -r l1 _ </proc/loadavg
		p=$(sed -n 's/^some avg10=\([0-9.]*\).*/\1/p' /proc/pressure/cpu)
		awk -v l="$l1" -v p="$p" 'BEGIN { exit !(l < 0.5 && p == 0) }' && break
		sleep 10
	done
	echo "conditions $(date -u +%T): up $(cut -d' ' -f1 /proc/uptime) s, loadavg $(cut -d' ' -f1-3 /proc/loadavg), cpu $(head -1 /proc/pressure/cpu)" |
		tee "$O/conditions.txt"
	awk -v l="$l1" -v p="$p" 'BEGIN { exit !(l < 0.5 && p == 0) }' || echo "TIMING NOT VALID: load/PSI gate not met in 15 min"
}

echo "== identity $(date -u +%FT%TZ)"
echo "boot_id $(cat /proc/sys/kernel/random/boot_id) uname $(uname -r) up $(cut -d' ' -f1 /proc/uptime)"
cat /proc/cmdline
got=$(grep -o 'ane_intree_oneshot=[a-z]*' /proc/cmdline | cut -d= -f2)
env=$(sudo -n grub-editenv /boot/grub/grubenv list) || env=UNREADABLE
echo "marker ${got:-none} (want $WANT); grubenv [$(printf '%s' "$env" | tr '\n' ' ')]"
check marker [ "${got:-none}" = "$WANT" ]
check grubenv-read [ "$env" != UNREADABLE ]
check grubenv-consumed [ -z "$(printf '%s\n' "$env" | sed -n 's/^next_entry=//p')" ]
if [ "$WANT" = intree ]; then check kernel [ "$(uname -r)" = "$REL" ]; else check kernel [ "$(uname -r)" != "$REL" ]; fi
systemctl --failed --no-legend | tee "$O/failed-units.txt"
check failed-units [ ! -s "$O/failed-units.txt" ]
echo "m1n1 stage 2 $(tr -d '\0' </proc/device-tree/chosen/asahi,m1n1-stage2-version); ESP $(sudo -n sha256sum /boot/efi/m1n1/boot.bin)"
sudo -n dmesg >"$O/dmesg.txt"

soc=$(tr '\0' '\n' </proc/device-tree/compatible | grep -m1 -E '^apple,t[0-9]+$' | cut -d, -f2)
case $soc in
t6020 | t6021 | t6022 | t8112) mod=ane_t6021 other=ane major=2 ;;
t8103 | t6000 | t6001 | t6002) mod=ane other=ane_t6021 major=1 ;;
*) check "soc-$soc" false; finish ;;
esac
dev=$(find /sys/bus/platform/devices/ -maxdepth 1 -name '*.ane' | head -1)
drv=$(basename "$(readlink -f "$dev/driver")")
echo "soc $soc device ${dev##*/} driver $drv (want $mod, ABI major $major)"

# Device tree: the running ANE node and its mailbox against the staged in-tree DTB of this board.
board=$(tr '\0' '\n' </proc/device-tree/compatible | grep -m1 -E '^apple,j[0-9a-z]+$' | cut -d, -f2)
python3 - "$S/dtbs/$soc-$board.dtb" "${dev##*/}" <<'PY' | tee "$O/dt-source.txt"
import glob, os, subprocess, sys
dtb, dev = sys.argv[1:]
B = "/proc/device-tree"
ane = [p[len(B):] for p in glob.glob(f"{B}/**/ane@{dev.split('.')[0]}", recursive=True)]
paths = list(ane)
if ane and os.path.exists(f"{B}{ane[0]}/mboxes"):
    ph = open(f"{B}{ane[0]}/mboxes", "rb").read()[:4]
    paths += [os.path.dirname(f)[len(B):] for f in glob.glob(f"{B}/**/phandle", recursive=True) if open(f, "rb").read() == ph]
same = bool(paths)
for p in paths:
    r = subprocess.run(["fdtget", "-p", dtb, p], capture_output=True, text=True)
    want = {}
    for prop in r.stdout.split():
        h = subprocess.run(["fdtget", "-t", "bx", dtb, p, prop], capture_output=True, text=True).stdout.split()
        want[prop] = bytes(int(x, 16) for x in h)
    live = {f: open(f"{B}{p}/{f}", "rb").read() for f in os.listdir(f"{B}{p}") if f != "name"}
    diff = sorted(k for k in set(want) | set(live) if want.get(k) != live.get(k))
    same &= r.returncode == 0 and not diff
    print(f"{p}: {'equal to the in-tree DTB' if r.returncode == 0 and not diff else 'differs: ' + (', '.join(diff) or 'absent in the DTB')}")
print("dt-source", "in-tree" if same else "other (not the in-tree DTB: boot.bin not swapped)")
PY

if [ "$WANT" != intree ]; then
	# Not a fallback criterion: the stock kernel on the in-tree tree (phase B) may leave the ANE unbound.
	echo "INFO ANE on the stock kernel: driver ${drv:-none}, nodes $(echo /dev/accel/*)"
	finish
fi
check bound [ "$drv" = "$mod" ]

echo "== in-tree modules"
for m in ane ane_t6021; do
	f=$(realpath "$(modinfo -n "$m")")
	echo "$m: $f intree=$(modinfo -F intree "$m") loaded=$([ -d "/sys/module/$m" ] && echo yes || echo no) taint=[$(cat "/sys/module/$m/taint" 2>/dev/null)] version=[$(cat "/sys/module/$m/version" 2>/dev/null)]"
	check "$m-path" [ "$f" = "/usr/lib/modules/$REL/kernel/drivers/accel/ane/$m.ko" ]
	if [ -d "/sys/module/$m" ]; then check "$m-not-oot" grep -qv O "/sys/module/$m/taint"; fi
done
check modules-sha bash -c "cd /usr/lib/modules/$REL && sha256sum -c --quiet $S/ane-modules.sha256"
check no-updates-dir [ ! -e "/usr/lib/modules/$REL/updates" ]
modprobe -c | grep -E '^(options|install|blacklist) (ane|ane_t6021)( |$)' | tee "$O/modprobe-ane.txt"
check no-modprobe-override [ ! -s "$O/modprobe-ane.txt" ]
check autoload [ -d "/sys/module/$mod" ]
grep -E 'ane|accel' "$O/dmesg.txt" | grep -E 'recognized but unqualified|unsupported ANE|Initialized ' | tee "$O/bind-lines.txt"
echo "$other: loaded=$([ -d "/sys/module/$other" ] && echo yes || echo no) bound=$(find "/sys/bus/platform/drivers/$other" -maxdepth 1 -name '*.ane' 2>/dev/null | wc -l)"
node=""
for c in /sys/class/accel/accel*; do
	[ "$(basename "$(readlink -f "$c/device/driver")")" = "$mod" ] && node=/dev/accel/${c##*/}
done
ls -l "${node:-/dev/accel/none}"
check accel-node test -c "${node:-/dev/accel/none}"

echo "== DRM ABI"
flock "$L" timeout 60 python3 - "${node:-/dev/accel/none}" <<'PY' | tee "$O/drm-version.txt"
import ctypes, fcntl, os, sys
class V(ctypes.Structure):  # struct drm_version, LP64
    _fields_ = [("major", ctypes.c_int), ("minor", ctypes.c_int), ("patch", ctypes.c_int),
                ("name_len", ctypes.c_size_t), ("name", ctypes.c_char_p), ("date_len", ctypes.c_size_t),
                ("date", ctypes.c_char_p), ("desc_len", ctypes.c_size_t), ("desc", ctypes.c_char_p)]
name = ctypes.create_string_buffer(64)
v = V(name_len=64, name=ctypes.cast(name, ctypes.c_char_p))
fd = os.open(sys.argv[1], os.O_RDWR)
fcntl.ioctl(fd, 0xC0406400, v)  # DRM_IOCTL_VERSION
print(f"driver {name.value.decode()} version {v.major}.{v.minor}.{v.patch}")
PY
check drm-major grep -qx "driver ane version $major\.[0-9]*\.[0-9]*" "$O/drm-version.txt"

if [ "$mod" = ane_t6021 ]; then
	echo "== firmware"
	grep -E 'fwload' "$O/dmesg.txt" | tee "$O/fwload.txt"
	fw=$(sed -n 's/.*fwload: \(apple\/ane\/[^ ]*\) PRELOAD validated.*/\1/p' "$O/fwload.txt" | head -1)
	sha256sum /lib/firmware/apple/ane/*.macho* 2>&1
	echo "firmware loaded: ${fw:-none}"
	check firmware-load [ -n "$fw" ]
	check firmware-no-error bash -c "! grep -q 'request_firmware(' '$O/fwload.txt'"
fi

echo "== interrupts"
awk 'NR == 1 { n = NF } /[0-9a-f]+\.ane|285408000/ { s = 0; for (i = 2; i <= n + 1; i++) s += $i; printf "%s cpu-sum %d:", $1, s; for (i = n + 2; i <= NF; i++) printf " %s", $i; print "" }' /proc/interrupts |
	tee "$O/interrupts.txt"
check irq-line [ -s "$O/interrupts.txt" ]

echo "== dmesg"
grep -i -E "$BAD" "$O/dmesg.txt" | tee "$O/dmesg-bad.txt"
grep -E "$CRASH" "$O/dmesg.txt" | tee "$O/dmesg-crash.txt"
sudo -n dmesg --level=emerg,alert,crit,err >"$O/dmesg-err.txt"
echo "err-level lines $(wc -l <"$O/dmesg-err.txt") (dmesg-err.txt), WARNING lines $(grep -c 'WARNING:' "$O/dmesg.txt")"
check dmesg-ane-bad [ ! -s "$O/dmesg-bad.txt" ]
check dmesg-crash [ ! -s "$O/dmesg-crash.txt" ]

echo "== omarchy-ane-check"
omarchy-ane-check >"$O/check.txt" 2>&1
rc=$?
cat "$O/check.txt"
check omarchy-ane-check [ "$rc" = 0 ]

# No ANE job on a device that is unbound, faulted or without its firmware.
blockers=()
for c in bound accel-node drm-major firmware-load firmware-no-error dmesg-ane-bad dmesg-crash; do
	case " ${fails[*]} " in *" $c "*) blockers+=("$c") ;; esac
done
if [ ${#blockers[@]} != 0 ]; then
	skip ane-jobs "not started: ${blockers[*]} failed"
	finish
fi

echo "== ane-run gates"
case $soc in
t602*)
	if [ -x "$ROOT/tools/ane-run" ] && [ -f "$ROOT/ane/t6021/gate/gate.sh" ]; then
		for op in add mul relu; do
			job "gate-$op" "$O/gate-$op.log" flock "$L" timeout 120 bash "$ROOT/ane/t6021/gate/gate.sh" "$O/gate-$op" "$op"
		done
		MV=/var/tmp/large-matvec-fixtures/matvec-2048-5120-m1
		if [ -d "$MV" ]; then
			job gate-matvec5120 "$O/gate-matvec5120.log" flock "$L" timeout 120 bash "$ROOT/ane/t6021/gate/gate.sh" \
				"$O/gate-matvec5120" matvec --anec "$MV/out/program-0.anec" --weights "$MV/model/weights.bin"
		else
			skip gate-matvec "no $MV on this host"
		fi
	else
		skip ane-run-gates "no built omarchy-ane tree at $ROOT"
	fi
	;;
*) skip ane-run-gates "omarchy-ane has no H13 ane-run fixture (fixtures/h14-anec only)" ;;
esac

echo "== encoder"
if [ "$mod" = ane_t6021 ] && [ -d /var/tmp/pk-enc ] && [ -f "$ROOT/tools/qwen_prog_run.py" ]; then
	loadgate
	PK=/var/tmp/pk-enc
	mkdir -p "$O/enc"
	# qwen_prog_run.py takes the ane-run lock itself
	job encoder "$O/encoder.log" timeout 600 python3 "$ROOT/tools/qwen_prog_run.py" --prog parakeet_encoder \
		--anec-dir "$PK" --ane-run "$ROOT/tools/ane-run" --timeout 120 --repeat 20 --work "$O/enc" \
		--in attention_mask="$PK/in/attention_mask.npy" --in input_features="$PK/in/input_features.npy" \
		--out linear_217_cast_fp16="$O/enc/hidden.npy" --out output_mask_f="$O/enc/mask.npy" \
		--golden linear_217_cast_fp16="$PK/in/golden_hidden16.npy"
	rm -f "$O"/enc/*.surface
	h=$(python3 -c 'import sys, hashlib, numpy as np
a = np.load(sys.argv[1]); print(hashlib.sha256(np.ascontiguousarray(a.astype("<f2")).tobytes()).hexdigest())' "$O/enc/hidden.npy")
	echo "encoder hidden fp16 sha256 $h (want $ENC16)"
	check encoder-bit-exact [ "$h" = "$ENC16" ]
elif [ "$mod" = ane ] && [ -f /var/tmp/ane-perf/gap7-bench.sh ]; then
	loadgate
	# shellcheck disable=SC2016 # $1 is the inner bash's argument
	job encoder "$O/encoder.log" flock "$L" timeout 600 bash -c 'cd /var/tmp/ane-perf && bash gap7-bench.sh "$1" 1' _ "$O/enc"
	h=$(grep -oE 'hidden=[0-9a-f]+' "$O/encoder.log" | head -1 | cut -d= -f2)
	echo "encoder hidden16 $h (want ${ENC16:0:16})"
	check encoder-bit-exact [ "$h" = "${ENC16:0:16}" ]
else
	skip encoder "no staged encoder fixture on this host"
fi
sudo -n dmesg | tail -n +"$((n0 + 1))" >"$O/dmesg-new.txt"
check dmesg-new-bad bash -c "! grep -q -i -E '$BAD|$CRASH' '$O/dmesg-new.txt'"
finish
