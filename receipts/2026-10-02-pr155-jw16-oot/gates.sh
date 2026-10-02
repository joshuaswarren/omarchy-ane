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
#        ANE_FIXTURE_DIR         (default /var/tmp/abi-verify/bundle; must
#                                 hold the h13-explicit-chain-add-mul package:
#                                 manifest.json + program-0/1.anec)
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
restored=0
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
# Covers every state the window can leave behind: kit module loaded, kit
# insmod FAILED after a clean rmmod (packaged module left unloaded), or
# packaged module already back.
restore_once() {
	[ "$restored" -eq 0 ] || return 0
	restored=1
	local cur
	cur=$(cat /sys/module/ane/srcversion 2>/dev/null)
	if [ -n "$base_srcversion" ] && [ "$cur" = "$base_srcversion" ]; then
		say "restore: packaged module already in place"
		return 0
	fi
	if [ -n "$cur" ]; then
		say "restore: rmmod loaded ane (srcversion '$cur')"
		sudo -n rmmod ane || { say "restore: rmmod FAILED"; return 1; }
	fi
	say "restore: modprobe packaged ane"
	sudo -n modprobe ane || { say "restore: modprobe FAILED"; return 1; }
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

# ---- op gates (pad-fixed ane-run + the H13 abi-verify fixtures) ---------------
# ane-run's --check OP needs the fixture inputs at channels 0/1 and assumes
# the H14 packed [1,512,1,1] checker layout, so it does not apply here.
# ane-run's --in/--out IDX are libane PORT POSITIONS, not driver channel
# numbers: ane.c INDEX_CHECKs idx against ane_src_count/ane_dst_count and
# indexes chans[src_bdx(idx)], where ane_bind_init orders each direction's
# roles by ASCENDING CHANNEL (ane_bind.h). The H13 fixtures put their
# sources on channels 5,6 and the destination on 4 (the manifest records
# those channel numbers; it does not record ane-run indices), so the
# positional contract is --in 0=a --in 1=b --out 0=y. Hardware-evidenced on
# jwm1 (boot H220): 20/20 bit-exact with those indices; --in 5 refuses with
# "tried to index 5 but max is 2" (src_count = 2).
# The gate drives the runner per program and direct-compares each output
# surface against an exact fp16 oracle built from the manifest (add and
# mul-by-0.5 are exact in fp16: no rounding anywhere in the chain; padding
# lanes must stay zero, so the whole surface is compared byte-for-byte).
step=ops
command -v python3 >/dev/null || fail "python3 needed to read the fixture manifest and build the fp16 oracle"
ANE_FIXTURE_DIR=${ANE_FIXTURE_DIR:-/var/tmp/abi-verify/bundle}
[ -f "$ANE_FIXTURE_DIR/manifest.json" ] || fail "fixture manifest missing: $ANE_FIXTURE_DIR/manifest.json"
ops_work=$(mktemp -d /var/tmp/ane-ops.XXXXXX)
python3 - "$ANE_FIXTURE_DIR/manifest.json" "$ops_work" <<'PY' || fail "fixture manifest is not the H13 add->mul chain"
import json, struct, sys

man, work = sys.argv[1], sys.argv[2]
man = json.load(open(man))
progs = man["programs"]
ops = [p["operation"] for p in progs]
if ops != ["add", "mul"]:
    sys.exit("manifest ops %r, want ['add', 'mul']" % ops)
# Both programs share the channel plan (H13: sources on ch5,ch6; result on
# ch4; ch5 carries `sum` into the mul program). ane-run indices are the
# per-direction POSITIONS of those roles, hence 0,1 in and 0 out.
for p in progs:
    ins = sorted(x["index"] for x in p["inputs"])
    outs = [x["index"] for x in p["outputs"]]
    if len(ins) != 2 or len(outs) != 1:
        sys.exit("program %s: want 2 inputs + 1 output" % p["file"])
plan_ins = sorted(x["index"] for x in progs[0]["inputs"])
plan_out = progs[0]["outputs"][0]["index"]
for p in progs:
    if sorted(x["index"] for x in p["inputs"]) != plan_ins \
            or [x["index"] for x in p["outputs"]] != [plan_out]:
        sys.exit("program %s deviates from the channel plan" % p["file"])
alloc, count = progs[0]["inputs"][0]["allocationBytes"], progs[0]["inputs"][0]["logicalBytes"] // 2

def half(x):
    return struct.unpack("<H", struct.pack("<e", x))[0]

# Floats in, fp16 bit patterns out; every value below is a multiple of
# 0.25 in [0.75, 3.25], so add and mul-by-0.5 are exact in fp16.
af = [1.0 + (i % 8) * 0.25 for i in range(count)]
a = [half(v) for v in af]
b = [half(0.5)] * count
s = [half(v + 0.5) for v in af]
y = [half(v * 0.5) for v in [x + 0.5 for x in af]]

def surface(vals):
    buf = bytearray(alloc)
    for i, v in enumerate(vals):
        struct.pack_into("<H", buf, i * 64, v)  # one fp16 per 64-byte plane
    return bytes(buf)

open(work + "/a.fp16", "wb").write(surface(a))
open(work + "/b.fp16", "wb").write(surface(b))
open(work + "/want-sum.fp16", "wb").write(surface(s))
open(work + "/want-y.fp16", "wb").write(surface(y))
open(work + "/chan.json", "w").write(json.dumps({"in0": 0, "in1": 1, "out": 0}))
PY
read -r CH_IN0 CH_IN1 CH_OUT <<< "$(python3 -c 'import json;d=json.load(open("'"$ops_work"'/chan.json"));print(d["in0"],d["in1"],d["out"])')"
"$ANE_RUN_BIN" --anec "$ANE_FIXTURE_DIR/program-0.anec" \
	--in "$CH_IN0=$ops_work/a.fp16" --in "$CH_IN1=$ops_work/b.fp16" \
	--out "$CH_OUT=$ops_work/sum.fp16" || fail "ane-run add fixture (program-0) failed"
cmp -s "$ops_work/sum.fp16" "$ops_work/want-sum.fp16" \
	|| fail "add output surface mismatch vs exact fp16 oracle"
say "fixture add (program-0): exact surface match"
"$ANE_RUN_BIN" --anec "$ANE_FIXTURE_DIR/program-1.anec" \
	--in "$CH_IN0=$ops_work/sum.fp16" --in "$CH_IN1=$ops_work/b.fp16" \
	--out "$CH_OUT=$ops_work/y.fp16" || fail "ane-run mul fixture (program-1) failed"
cmp -s "$ops_work/y.fp16" "$ops_work/want-y.fp16" \
	|| fail "mul output surface mismatch vs exact fp16 oracle"
say "fixture mul (program-1): exact surface match"
rm -rf "$ops_work"
say "SKIP relu (no H13 relu fixture exists on this host - named missing prerequisite)"
say "SKIP matvec (no H13 matvec fixture; if you hold one, run it separately as: ane-run --anec <prog> --in/--out per its manifest, compared against --check matvec --weights <fp16 [N,K]>)"

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
