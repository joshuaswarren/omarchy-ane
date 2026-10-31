#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Offline regression for run-stage4.sh: diagnostics stay module-lifecycle-
# free for any argument; GO refuses while the gate is closed; REHEARSE
# gates on preflight and an unloaded module, records the run header,
# raises kernel.panic and restores it on the alive path.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp/opencode}/h17-stage4-test.XXXXXX" 2>/dev/null || mktemp -d "/tmp/opencode/h17-stage4-test.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
mkdir -p "$T/bin" "$T/repo/ane/h17"
printf 'Apple MacBook Neo\0' > "$T/model"
printf 'fixture-boot\n' > "$T/bootid"
printf '0\n' > "$T/panic_sysctl"
: > "$T/modules"
: > "$T/dmesg"
printf 'stub-kernel-module\n' > "$T/repo/ane/h17/ane_h17_wake.ko"
cat > "$T/repo/ane/h17/preflight.py" <<'PY'
import os
from pathlib import Path

root = Path(os.environ["H17_TEST_ROOT"])
with (root / "preflight.calls").open("a") as log:
    log.write("preflight\n")
raise SystemExit(int(os.environ.get('H17_TEST_PREFLIGHT_EXIT', '0')))
PY
cat > "$T/bin/insmod" <<'SH'
#!/usr/bin/env bash
printf 'insmod %s\n' "$*" >> "$H17_TEST_ROOT/module.calls"
cr=0
for a in "$@"; do case "$a" in contain_rehearse=*) cr="${a#contain_rehearse=}" ;; esac; done
if [[ ${H17_TEST_INSMOD_EXIT:-0} != 0 ]]; then
    printf 'ane_h17_wake RESULT stage=4 verdict=FAIL reason=rehearsal-no-fire\n' >> "$H17_TEST_ROOT/dmesg"
    exit "$H17_TEST_INSMOD_EXIT"
fi
printf 'ane_h17_wake 1 0 - Live 0x0\n' >> "$H17_TEST_ROOT/modules"
if [[ $cr == 2 ]]; then
    printf 'ane_h17_wake RESULT stage=4 verdict=PASS reason=rehearsal-lifecycle\n' >> "$H17_TEST_ROOT/dmesg"
else
    printf 'ane_h17_wake RESULT stage=4 verdict=PASS reason=asc-snapshot\n' >> "$H17_TEST_ROOT/dmesg"
fi
SH
cat > "$T/bin/rmmod" <<'SH'
#!/usr/bin/env bash
printf 'rmmod %s\n' "$*" >> "$H17_TEST_ROOT/module.calls"
if [[ ${H17_TEST_RMMOD_EXIT:-0} != 0 ]]; then exit "$H17_TEST_RMMOD_EXIT"; fi
: > "$H17_TEST_ROOT/modules"
SH
chmod +x "$T/bin/insmod" "$T/bin/rmmod"

export H17_TEST_ROOT="$T" H17_REPO="$T/repo" UNAME_R=7.2.11-6-1-ARCH
export H17_BOOT_ID_FILE="$T/bootid" H17_MODEL_FILE="$T/model"
export H17_MODULES_FILE="$T/modules" H17_DMESG_FILE="$T/dmesg"
export H17_MARKER_FILE="$T/state/stage4-marker"
export H17_PANIC_SYSCTL="$T/panic_sysctl"
export INSMOD="$T/bin/insmod" RMMOD="$T/bin/rmmod"
export PATH="$T/bin:$PATH"
export H17_LOG_DIR="$T/missing-parent/log"
fails=0
rc=0

run() {
    rc=0
    : > "$T/preflight.calls"
    : > "$T/module.calls"
    cp "$T/modules" "$T/modules.before"
    cp "$T/panic_sysctl" "$T/panic.before"
    bash "$here/run-stage4.sh" "$@" > "$T/out" 2>&1 || rc=$?
}

pass_or_dump() {
    local name=$1
    if [[ $fails == ${fails_last:-0} ]]; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s\n' "$name"
        cat "$T/out" "$T/module.calls" >&2
    fi
    fails_last=$fails
}

# --- diagnostics: no module lifecycle, whatever the argument ---
run
if [[ $rc == 2 ]] &&
   grep -q 'H17_STAGE4_READY reason=containment-v2' "$T/out" &&
   [[ $(wc -l < "$T/preflight.calls") == 1 ]] &&
   [[ ! -s $T/module.calls ]] &&
   [[ ! -e $H17_LOG_DIR ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL default diagnostics\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'default diagnostics stay module-lifecycle-free'

run GO
if [[ $rc == 2 ]] && grep -q 'H17_STAGE4_REFUSED reason=gate-closed' "$T/out" &&
   [[ ! -s $T/module.calls ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL GO refused while gate closed\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'GO refuses while the gate is closed'

# --- REHEARSE gates ---
printf 'ane_h17_wake 1 0 - Live 0x0\n' > "$T/modules"
run REHEARSE
if [[ $rc == 2 ]] && grep -q 'H17_STAGE4_REFUSED reason=module-loaded' "$T/out" &&
   [[ ! -s $T/module.calls ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL REHEARSE refused while loaded\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'REHEARSE refused while the module is loaded'

H17_TEST_PREFLIGHT_EXIT=1 run REHEARSE
if [[ $rc == 2 ]] && grep -q 'H17_STAGE4_REFUSED reason=preflight' "$T/out" &&
   [[ ! -s $T/module.calls ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL REHEARSE refused on failed preflight\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'REHEARSE refused on a failed preflight'

# --- REHEARSE: the lifecycle rung (no panic, no reboot) ---
: > "$T/modules"
: > "$T/dmesg"
run REHEARSE
if [[ $rc == 0 ]] &&
   grep -q 'H17_STAGE4_PASS reason=rehearsal-lifecycle' "$T/out" &&
   grep -q 'insmod .*ane_h17_wake.ko optin=1 contain_rehearse=2' "$T/module.calls" &&
   grep -q 'rmmod ane_h17_wake' "$T/module.calls" &&
   grep -q 'kernel.panic raised to 30 (was 0)' "$T/out" &&
   [[ $(cat "$T/panic_sysctl") == 0 ]] &&
   grep -q '^rehearsal kernel=' "$H17_MARKER_FILE"; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL REHEARSE lifecycle\n' >&2
    cat "$T/out" "$T/module.calls" >&2
fi
pass_or_dump 'REHEARSE lifecycle: single insmod, rmmod, panic timeout restored'

# --- REHEARSE-FIRE: the alive (timer-did-not-fire) path is a recorded FAIL ---
: > "$T/dmesg"
H17_TEST_INSMOD_EXIT=99 run REHEARSE-FIRE
if [[ $rc == 1 ]] &&
   grep -q 'H17_STAGE4_FAIL reason=insmod' "$T/out" &&
   grep -q 'contain_rehearse=1' "$T/module.calls" &&
   ! grep -q rmmod "$T/module.calls" &&
   [[ $(cat "$T/panic_sysctl") == 0 ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL REHEARSE-FIRE alive path\n' >&2
    cat "$T/out" "$T/module.calls" >&2
fi
pass_or_dump 'REHEARSE-FIRE alive path: recorded FAIL, no rmmod'

if [[ $fails != 0 ]]; then
    printf 'test-stage4: %s failures\n' "$fails"
    exit 1
fi
printf 'test-stage4: ok\n'
