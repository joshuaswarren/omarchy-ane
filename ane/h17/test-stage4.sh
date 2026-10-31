#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Offline regression for run-stage4.sh: fixture diagnostics and stateful
# module commands. Diagnostics (no argument) must stay module-lifecycle-
# free; GO must stay contained: preflight gate, unloaded-module gate,
# marker record, a single insmod, rmmod on success, recorded verdict.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/h17-stage4-test.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
mkdir -p "$T/bin" "$T/repo/ane/h17"
printf 'Apple MacBook Neo\0' > "$T/model"
printf '0123abcd-4567-89ab-cdef-0123456789ab\n' > "$T/bootid"
: > "$T/modules"
: > "$T/dmesg"
printf 'stub-kernel-module\n' > "$T/repo/ane/h17/ane_h17_wake.ko"
cat > "$T/repo/ane/h17/preflight.py" <<'PY'
import json
import os
from pathlib import Path

root = Path(os.environ["H17_TEST_ROOT"])
with (root / "preflight.calls").open("a") as log:
    log.write("preflight\n")
status = int(os.environ.get("H17_TEST_PREFLIGHT_EXIT", "0"))
print(json.dumps({"verdict": "PASS" if status == 0 else "REFUSED"}))
raise SystemExit(status)
PY
cat > "$T/bin/insmod" <<'SH'
#!/usr/bin/env bash
printf 'insmod %s\n' "$*" >> "$H17_TEST_ROOT/module.calls"
if [[ ${H17_TEST_INSMOD_EXIT:-0} != 0 ]]; then exit "$H17_TEST_INSMOD_EXIT"; fi
printf 'ane_h17_wake 1 0 - Live 0x0\n' >> "$H17_TEST_ROOT/modules"
printf 'ane_h17_wake RESULT stage=4 verdict=PASS reason=asc-snapshot\n' >> "$H17_TEST_ROOT/dmesg"
SH
cat > "$T/bin/rmmod" <<'SH'
#!/usr/bin/env bash
printf 'rmmod %s\n' "$*" >> "$H17_TEST_ROOT/module.calls"
if [[ ${H17_TEST_RMMOD_EXIT:-0} != 0 ]]; then exit "$H17_TEST_RMMOD_EXIT"; fi
: > "$H17_TEST_ROOT/modules"
SH
cat > "$T/bin/modprobe" <<'SH'
#!/usr/bin/env bash
printf 'modprobe %s\n' "$*" >> "$H17_TEST_ROOT/module.calls"
exit 99
SH
chmod +x "$T/bin/insmod" "$T/bin/rmmod" "$T/bin/modprobe"

export H17_TEST_ROOT="$T" H17_REPO="$T/repo" UNAME_R=7.2.11-6-1-ARCH
export H17_MODEL_FILE="$T/model" H17_BOOT_ID_FILE="$T/bootid" H17_MODULES_FILE="$T/modules"
export H17_DMESG_FILE="$T/dmesg" H17_MARKER_FILE="$T/state/stage4-marker"
export INSMOD="$T/bin/insmod" RMMOD="$T/bin/rmmod"
export PATH="$T/bin:$PATH"
# No log directory is created, even if the old logging destination is invalid.
export H17_LOG_DIR="$T/missing-parent/log"
fails=0
rc=0

run() {
    rc=0
    : > "$T/preflight.calls"
    : > "$T/module.calls"
    cp "$T/modules" "$T/modules.before"
    bash "$here/run-stage4.sh" "$@" > "$T/out" 2>&1 || rc=$?
}

pass_or_dump() {
    local name=$1
    if [[ $fails == ${fails_last:-0} ]]; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s\n' "$name"
        cat "$T/out" "$T/module.calls"
    fi
    fails_last=$fails
}

# --- diagnostics: no module lifecycle, whatever the argument ---
run
if [[ $rc == 2 ]] &&
   grep -q 'H17_STAGE4_READY reason=map-qualified' "$T/out" &&
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

run anything
if [[ $rc == 2 ]] && grep -q 'H17_STAGE4_READY reason=map-qualified' "$T/out" &&
   [[ ! -s $T/module.calls ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL unknown argument\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'unknown arguments stay module-lifecycle-free'

# --- GO: refused before hardware when the gates say so ---
printf 'ane_h17_wake 1 0 - Live 0x0\n' > "$T/modules"
run GO
if [[ $rc == 2 ]] && grep -q 'H17_STAGE4_REFUSED reason=module-loaded' "$T/out" &&
   [[ ! -s $T/module.calls ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL GO refused while loaded\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'GO refused while the module is loaded'

H17_TEST_PREFLIGHT_EXIT=1 run GO
if [[ $rc == 2 ]] && grep -q 'H17_STAGE4_REFUSED reason=preflight' "$T/out" &&
   [[ ! -s $T/module.calls ]]; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL GO refused on failed preflight\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'GO refused on a failed preflight'

# --- GO: the contained run ---
: > "$T/modules"
run GO
if [[ $rc == 0 ]] &&
   grep -q 'H17_STAGE4_PASS reason=asc-snapshot' "$T/out" &&
   grep -q 'insmod .*ane_h17_wake.ko optin=1' "$T/module.calls" &&
   grep -q 'rmmod ane_h17_wake' "$T/module.calls" &&
   [[ $(grep -c insmod "$T/module.calls") == 1 ]] &&
   grep -q '^run kernel=' "$H17_MARKER_FILE"; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL GO pass\n' >&2
    cat "$T/out" "$T/module.calls" >&2
fi
pass_or_dump 'GO pass: single insmod, rmmod, marker recorded'

# --- GO: a failed insmod is a recorded FAIL without rmmod ---
: > "$T/dmesg"
H17_TEST_INSMOD_EXIT=99 run GO
if [[ $rc == 1 ]] && grep -q 'H17_STAGE4_FAIL reason=insmod' "$T/out" &&
   ! grep -q rmmod "$T/module.calls"; then
    :
else
    fails=$((fails + 1))
    printf 'FAIL GO insmod failure\n' >&2
    cat "$T/out" >&2
fi
pass_or_dump 'GO insmod failure is a recorded FAIL'

if [[ $fails != 0 ]]; then
    printf 'test-stage4: %s failures\n' "$fails"
    exit 1
fi
printf 'test-stage4: ok\n'
