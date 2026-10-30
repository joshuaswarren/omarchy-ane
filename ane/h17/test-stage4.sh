#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Off-device check of run-stage4.sh: fake uname, model, boot id, device-tree
# node, preflight, insmod, rmmod, dmesg and lsmod, in a temp dir. Exits 0 iff
# every refusal fires and the rung order holds. It loads no module.
set -u
here=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d /dev/shm/h17-stage4-test.XXXXXX)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/dt/soc/ane-h17-ps-probe" "$T/work"
printf 'omarchy,t8140-ane-ps-probe\0' > "$T/dt/soc/ane-h17-ps-probe/compatible"
printf 'Apple MacBook Neo\0' > "$T/model"
echo "0123abcd-4567-89ab-cdef-0123456789ab" > "$T/bootid"
printf 'module-bytes' > "$T/ane_h17_wake.ko"
printf '#!/bin/bash\necho "$@" > "%s/insmod.args"\nif [ -n "${FAKE_STUCK:-}" ]; then\n  echo "[1.0] ane_h17_wake soc:ane-h17-ps-probe: ane_h17_wake read off=0x1600048 CPU_STATUS" >> "%s/dmesg.txt"\n  echo "[1.1] ane_h17_wake soc:ane-h17-ps-probe: ane_h17_wake RESULT stage=4 verdict=FAIL reason=core-not-stopped status=8" >> "%s/dmesg.txt"\n  echo "[1.2] ane_h17_wake core did not return to STOPPED" >> "%s/dmesg.txt"\nelse\n  echo "[1.0] ane_h17_wake soc:ane-h17-ps-probe: ane_h17_wake read off=0x1600048 CPU_STATUS" >> "%s/dmesg.txt"\n  echo "[1.1] ane_h17_wake soc:ane-h17-ps-probe: ane_h17_wake RESULT stage=4 verdict=${FAKE_VERDICT:-PASS} reason=${FAKE_REASON:-asc-snapshot}" >> "%s/dmesg.txt"\nfi\nexit 0\n' "$T" "$T" "$T" "$T" "$T" "$T" > "$T/insmod"
printf '#!/bin/bash\ntrue\n' > "$T/rmmod"
printf '#!/bin/bash\ncat "%s/dmesg.txt" 2>/dev/null\nexit 0\n' "$T" > "$T/dmesg"
printf '#!/bin/bash\nif [ -e "%s/loaded" ]; then echo "ane_h17_wake 1 0"; fi\n' "$T" > "$T/lsmod"
printf '#!/bin/bash\necho prefetched\n' > "$T/preflight"
chmod +x "$T"/insmod "$T"/rmmod "$T/dmesg" "$T/lsmod" "$T/preflight"
fails=0
run() {
  local xtrace=""
  [ -n "${H17_TEST_XTRACE:-}" ] && xtrace="-x"
  env H17_REPO="$here/../.." UNAME_R=7.2.11-6-1-ARCH H17_MODEL_FILE="$T/model" \
      H17_BOOT_ID_FILE="$T/bootid" H17_PROBE_NODE="$T/dt/soc/ane-h17-ps-probe/compatible" \
      H17_PREFLIGHT="$T/preflight" H17_LOG_DIR="$T/log" \
      INSMOD="$T/insmod" RMMOD="$T/rmmod" DMESG="$T/dmesg" LSMOD="$T/lsmod" \
      bash $xtrace "$here/run-stage4.sh" "$@" > "$T/out" 2>&1
}
expect() { # name, want rc, command...
  local name=$1 want=$2 rc
  shift 2
  rm -f "$T/dmesg.txt" "$T/insmod.args"
  run "$@"
  rc=$?
  if [ "$rc" = "$want" ]; then echo "  PASS $name"; else echo "  FAIL $name (rc $rc, want $want)"; tail -3 "$T/out"; fails=$((fails + 1)); fi
}
expect "snapshot rung passes without GO" 0
grep -q 'ane_h17_wake.ko optin=1$' "$T/insmod.args" && echo "  PASS snapshot loads optin=1 only" || { echo "  FAIL snapshot args: $(cat "$T/insmod.args")"; fails=$((fails + 1)); }
if grep -q 'confirm_wake' "$T/insmod.args"; then echo "  FAIL confirm_wake leaked: $(cat "$T/insmod.args")"; fails=$((fails + 1)); else echo "  PASS no confirm_wake without GO"; fi
FAKE_REASON=actual-not-on FAKE_VERDICT=FAIL expect "a failed snapshot rung gives rc 1" 1
FAKE_REASON=asc-snapshot expect "snapshot rung passes again" 0
FAKE_REASON=asc-snapshot FAKE_STUCK=1 expect "core-not-stopped gives rc 3" 3
env H17_REPO="$here/../.." UNAME_R=other-kernel H17_MODEL_FILE="$T/model" \
    H17_BOOT_ID_FILE="$T/bootid" H17_PROBE_NODE="$T/dt/soc/ane-h17-ps-probe/compatible" \
    H17_PREFLIGHT="$T/preflight" H17_LOG_DIR="$T/log" \
    INSMOD="$T/insmod" RMMOD="$T/rmmod" DMESG="$T/dmesg" LSMOD="$T/lsmod" \
    bash "$here/run-stage4.sh" > "$T/out" 2>&1
[ "$?" = 1 ] && echo "  PASS wrong kernel refused" || { echo "  FAIL wrong kernel not refused"; fails=$((fails + 1)); }
env H17_REPO="$here/../.." UNAME_R=7.2.11-6-1-ARCH H17_MODEL_FILE="$T/model" \
    H17_BOOT_ID_FILE="$T/bootid" H17_PROBE_NODE="$T/missing" \
    H17_PREFLIGHT="$T/preflight" H17_LOG_DIR="$T/log" \
    INSMOD="$T/insmod" RMMOD="$T/rmmod" DMESG="$T/dmesg" LSMOD="$T/lsmod" \
    bash "$here/run-stage4.sh" GO > "$T/out" 2>&1
grep -q 'probe-node-missing' "$T/out" && echo "  PASS missing probe node refused" || { echo "  FAIL probe-node check"; fails=$((fails + 1)); }
rm -f "$T/dmesg.txt"
FAKE_REASON=wake-word FAKE_VERDICT=PASS expect "GO run passes with a wake-word result" 0 GO
grep -q 'confirm_wake=1' "$T/insmod.args" && echo "  PASS GO loads confirm_wake=1" || { echo "  FAIL GO args: $(cat "$T/insmod.args")"; fails=$((fails + 1)); }
printf 'loaded\n' > "$T/loaded"
expect "a loaded module is refused" 1
rm -f "$T/loaded"
printf '#!/bin/bash\nfalse\n' > "$T/preflight.bad"
env H17_REPO="$here/../.." UNAME_R=7.2.11-6-1-ARCH H17_MODEL_FILE="$T/model" \
    H17_BOOT_ID_FILE="$T/bootid" H17_PROBE_NODE="$T/dt/soc/ane-h17-ps-probe/compatible" \
    H17_PREFLIGHT="$T/preflight.bad" H17_LOG_DIR="$T/log" \
    INSMOD="$T/insmod" RMMOD="$T/rmmod" DMESG="$T/dmesg" LSMOD="$T/lsmod" \
    bash "$here/run-stage4.sh" > "$T/out" 2>&1
grep -q 'insmod' "$T/insmod.args" 2>/dev/null && { echo "  FAIL preflight ran after failure"; fails=$((fails + 1)); } || echo "  PASS failed preflight refuses before insmod"

if [ "$fails" = 0 ]; then echo "test-stage4: ok"; else echo "test-stage4: $fails failures"; exit 1; fi
