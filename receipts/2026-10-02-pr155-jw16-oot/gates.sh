#!/bin/bash
# JW16 gate run for the PR155 out-of-tree ane module (H13/ABI 1 on T6001).
#
# Runs ON jw16, inside the gpuwin window (llm-inference stopped by the
# gpuwin trap), under the lane's JW16_MAINTENANCE protocol:
#   - the studio-side JW16_MAINTENANCE line and the START announcement are
#     the LEAD's steps, before this script;
#   - on jw16, /var/tmp/JW16_MAINTENANCE must be ABSENT (presence = an
#     unresolved prior window: stop);
#   - /var/tmp/ane-run.lock is read-opened on fd 8 (never for write) and
#     flocked for the whole window;
#   - any check failure stops the window: the script attempts the packaged
#     module restore ONCE, reports, and exits nonzero. No retries.
#
# Usage: gates.sh <kit-dir>       # the extracted pr155-jw16-oot/
# Env:   ANE_RUN_BIN             (default /var/tmp/abi-verify/bundle/ane-run,
#                                 the pad-fixed ane-run from omarchy-ane main)
#        GAP7_BENCH              (default /var/tmp/ane-perf/gap7-bench.sh)
#        ANE_MATVEC_WEIGHTS      (fp16 [N,K]; without it matvec is skipped)
set -uo pipefail

KIT=${1:?usage: gates.sh <kit-dir>}
KIT=$(cd "$KIT" && pwd)
ANE_RUN_BIN=${ANE_RUN_BIN:-/var/tmp/abi-verify/bundle/ane-run}
GAP7_BENCH=${GAP7_BENCH:-/var/tmp/ane-perf/gap7-bench.sh}
GOLD=fca96f1355485ec3
N1_LO=1055
N1_HI=1219
BAD_RE='tm completion|quarantin|EXCH|DART fault|translation fault|Oops|BUG:|kernel panic'

boot_id_0="" boot_id_now=""
base_srcversion="" base_installed_sha=""
swapped=0 restored=0
fails=0 step=init

say() { echo "[gates] $*"; }

boot_id() { cat /proc/sys/kernel/random/boot_id; }
dmesg_bad() { sudo -n dmesg 2>/dev/null || journalctl -k 2>/dev/null; }
driver_bound() { compgen -G "/sys/bus/platform/drivers/ane/*.ane" >/dev/null; }

fail() {
	echo "[gates] FAIL at step '$step': $*" >&2
	fails=$((fails + 1))
	finish
	exit 1
}

# Restore the packaged module. Runs once per exit path, never retried.
restore_once() {
	[ "$restored" -eq 0 ] || return 0
	restored=1
	[ "$swapped" -eq 1 ] || return 0
	say "restore: rmmod kit ane, modprobe packaged ane"
	sudo -n rmmod ane || { say "restore: rmmod kit ane FAILED"; return 1; }
	sudo -n modprobe ane || { say "restore: modprobe packaged ane FAILED"; return 1; }
	if [ "$(cat /sys/module/ane/srcversion 2>/dev/null)" = "$base_srcversion" ] \
		&& [ "$(sha256sum "/lib/modules/$(uname -r)/updates/dkms/ane.ko" 2>/dev/null | awk '{print $1}')" \
			= "$base_installed_sha" ]; then
		say "restore: packaged module back (srcversion $base_srcversion, installed sha unchanged)"
		return 0
	fi
	say "restore: VERIFY MANUALLY - packaged module state not confirmed"
	return 1
}

finish() {
	local rc=0
	restore_once || rc=1
	if [ -n "$boot_id_now" ] && [ "$boot_id_now" != "$boot_id_0" ]; then
		echo "[gates] FAIL: boot_id changed during the window"
		rc=1
	fi
	echo "dmesg bad-line count now: $(dmesg_bad | grep -cE "$BAD_RE")"
	if [ "$fails" -eq 0 ] && [ "$rc" -eq 0 ]; then
		echo "JW16-GATES: PASS"
	else
		echo "JW16-GATES: FAIL ($fails check failures)"
	fi
}

# ---- window open -----------------------------------------------------------
step=window
[ "$(id -u)" -ne 0 ] || fail "run as a normal user (sudo -n is used for module ops)"
if [ -e /var/tmp/JW16_MAINTENANCE ]; then
	fail "/var/tmp/JW16_MAINTENANCE present on jw16 - unresolved prior window, stop"
fi
[ -e /var/tmp/ane-run.lock ] || fail "/var/tmp/ane-run.lock missing"
[ -f "$KIT/ane/ane.ko" ] || fail "$KIT/ane/ane.ko missing (run build.sh first)"
[ -f "$KIT/ane_get_caps.c" ] || fail "$KIT/ane_get_caps.c missing"

exec 8</var/tmp/ane-run.lock
flock 8 || fail "cannot flock /var/tmp/ane-run.lock"

# ---- pre-record -------------------------------------------------------------
step=pre-record
boot_id_0=$(boot_id)
base_srcversion=$(cat /sys/module/ane/srcversion)
base_installed_sha=$(sha256sum "/lib/modules/$(uname -r)/updates/dkms/ane.ko" | awk '{print $1}')
bad_0=$(dmesg_bad | grep -cE "$BAD_RE")
[ -n "$base_srcversion" ] || fail "packaged ane not loaded (/sys/module/ane/srcversion empty)"
driver_bound || fail "ane driver not bound to a platform device"
[ -c /dev/accel/accel0 ] || fail "/dev/accel/accel0 missing"
if [ ! -x "$KIT/ane_get_caps" ]; then
	say "building GET_CAPS probe"
	cc -O2 -Wall -I "$KIT/ane/uapi" -I /usr/include/drm \
		"$KIT/ane_get_caps.c" -o "$KIT/ane_get_caps" || fail "probe build failed"
fi
[ -x "$ANE_RUN_BIN" ] || fail "ANE_RUN_BIN '$ANE_RUN_BIN' missing (set it to the pad-fixed ane-run from omarchy-ane main)"
[ -f "$GAP7_BENCH" ] || fail "GAP7_BENCH '$GAP7_BENCH' missing"
say "baseline: boot $boot_id_0 srcversion $base_srcversion installed-sha $base_installed_sha bad-dmesg $bad_0"

# ---- swap in the kit module --------------------------------------------------
step=swap
sudo -n rmmod ane || fail "rmmod packaged ane failed (device held open?)"
sudo -n insmod "$KIT/ane/ane.ko" || fail "insmod kit ane.ko failed"
swapped=1
kit_file_src=$(modinfo -F srcversion "$KIT/ane/ane.ko")
[ "$(cat /sys/module/ane/srcversion)" = "$kit_file_src" ] \
	|| fail "loaded srcversion '$(cat /sys/module/ane/srcversion)' != built file srcversion '$kit_file_src'"
case "$kit_file_src" in
"")
	say "kit module carries no srcversion; identity = insmod rc + sha256 $(sha256sum "$KIT/ane/ane.ko" | awk '{print $1}')"
	;;
*)
	[ "$kit_file_src" != "$base_srcversion" ] \
		|| fail "kit srcversion equals the packaged module's"
	;;
esac
[ "$(modinfo -F vermagic "$KIT/ane/ane.ko" | awk '{print $1}')" = "$(uname -r)" ] \
	|| fail "kit ane.ko vermagic does not match the running kernel - wrong build tree"
driver_bound || fail "kit ane did not bind to the platform device"
[ -c /dev/accel/accel0 ] || fail "/dev/accel/accel0 missing after kit load"
say "kit module loaded: srcversion '$kit_file_src', bound"

# ---- ABI gate ----------------------------------------------------------------
step=abi
"$KIT/ane_get_caps" || fail "GET_CAPS probe failed"

# ---- op gates (pad-fixed ane-run) ---------------------------------------------
step=ops
for op in add mul relu; do
	"$ANE_RUN_BIN" --check "$op" || fail "ane-run --check $op failed"
	say "ane-run --check $op: ok"
done
if [ -n "${ANE_MATVEC_WEIGHTS:-}" ]; then
	"$ANE_RUN_BIN" --check matvec --weights "$ANE_MATVEC_WEIGHTS" \
		|| fail "ane-run --check matvec failed"
	say "ane-run --check matvec: ok"
else
	say "SKIP matvec (set ANE_MATVEC_WEIGHTS to run it)"
fi

# ---- whole-encoder bit-exact on the kit module --------------------------------
step=encoder
enc_out=$(bash "$GAP7_BENCH" OUT 1) || fail "gap7-bench n1 failed"
echo "$enc_out" | tail -n 8
echo "$enc_out" | grep -q "$GOLD" || fail "encoder hidden16 != $GOLD on the kit module"
elapsed=$(echo "$enc_out" | grep -oE '[0-9]+ ms' | head -1 | grep -oE '[0-9]+')
if [ -n "$elapsed" ]; then
	if [ "$elapsed" -ge "$N1_LO" ] && [ "$elapsed" -le "$N1_HI" ]; then
		say "encoder: hidden16 $GOLD, n1 ${elapsed} ms (in band)"
	else
		say "WARN: encoder hidden16 $GOLD but n1 ${elapsed} ms outside band $N1_LO-$N1_HI (load?)"
	fi
else
	say "WARN: could not parse n1 elapsed ms from gap7 output"
fi

# ---- restore the packaged module -----------------------------------------------
step=restore
restore_once || fail "packaged module restore did not verify"
[ "$(cat /sys/module/ane/srcversion)" = "$base_srcversion" ] \
	|| fail "packaged srcversion not restored"
driver_bound || fail "packaged ane not bound after restore"

# ---- post-record + final smoke on the packaged module ---------------------------
step=post
boot_id_now=$(boot_id)
bad_1=$(dmesg_bad | grep -cE "$BAD_RE")
[ "$bad_1" -eq "$bad_0" ] || fail "dmesg bad-line count changed: $bad_0 -> $bad_1"
post_out=$(bash "$GAP7_BENCH" OUT 1) || fail "post-restore gap7-bench n1 failed"
echo "$post_out" | tail -n 4
echo "$post_out" | grep -q "$GOLD" || fail "post-restore encoder hidden16 != $GOLD"

finish
