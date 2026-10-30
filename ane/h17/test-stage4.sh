#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Offline containment regression: use fixture diagnostics and stateful module
# commands that fail the test if the runner attempts any hardware lifecycle.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/h17-stage4-test.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
mkdir -p "$T/bin" "$T/repo/ane/h17"
printf 'Apple MacBook Neo\0' > "$T/model"
printf '0123abcd-4567-89ab-cdef-0123456789ab\n' > "$T/bootid"
: > "$T/modules"
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
printf 'ane_h17_wake 1 0 - Live 0x0\n' > "$H17_TEST_ROOT/modules"
exit 99
SH
cat > "$T/bin/rmmod" <<'SH'
#!/usr/bin/env bash
printf 'rmmod %s\n' "$*" >> "$H17_TEST_ROOT/module.calls"
: > "$H17_TEST_ROOT/modules"
exit 99
SH
cat > "$T/bin/modprobe" <<'SH'
#!/usr/bin/env bash
printf 'modprobe %s\n' "$*" >> "$H17_TEST_ROOT/module.calls"
exit 99
SH
chmod +x "$T/bin/insmod" "$T/bin/rmmod" "$T/bin/modprobe"

# Export the legacy override names too: the retired runner honored these.
export H17_TEST_ROOT="$T" H17_REPO="$T/repo" UNAME_R=7.2.11-6-1-ARCH
export H17_MODEL_FILE="$T/model" H17_BOOT_ID_FILE="$T/bootid" H17_MODULES_FILE="$T/modules"
export INSMOD="$T/bin/insmod" RMMOD="$T/bin/rmmod"
export PATH="$T/bin:$PATH"
# No log directory is created, even if the old logging destination is invalid.
export H17_LOG_DIR="$T/missing-parent/log"
fails=0
expect() {
    local name=$1 rc=0
    shift
    : > "$T/preflight.calls"
    : > "$T/module.calls"
    cp "$T/modules" "$T/modules.before"
    bash "$here/run-stage4.sh" "$@" > "$T/out" 2>&1 || rc=$?
    if [[ $rc == 2 ]] &&
       grep -qx 'H17_STAGE4_REFUSED reason=unqualified-register-map' "$T/out" &&
       [[ $(wc -l < "$T/preflight.calls") == ${H17_TEST_PREFLIGHT_CALLS:-1} ]] &&
       [[ ! -s $T/module.calls ]] &&
       cmp -s "$T/modules.before" "$T/modules" &&
       [[ ! -e $H17_LOG_DIR ]]; then
        printf 'PASS %s\n' "$name"
    else
        printf 'FAIL %s (exit %s)\n' "$name" "$rc"
        cat "$T/out" "$T/module.calls"
        fails=$((fails + 1))
    fi
}

expect 'default diagnostics refuse before hardware'
expect 'former GO argument cannot enable hardware' GO
expect 'unknown arguments cannot enable hardware' anything
printf 'ane_h17_wake 1 0 - Live 0x0\n' > "$T/modules"
expect 'an existing wake module remains loaded' GO
if ! grep -q '^loaded_module=ane_h17_wake ' "$T/out"; then
    printf 'FAIL loaded module missing from diagnostics\n'
    fails=$((fails + 1))
fi
H17_TEST_PREFLIGHT_EXIT=2 expect 'failed preflight still refuses without cleanup'
H17_TEST_PREFLIGHT_EXIT=1 expect 'preflight error still refuses without cleanup' GO
UNAME_R=other-kernel expect 'kernel mismatch cannot bypass containment'
H17_MODEL_FILE="$T/missing-model" expect 'missing model remains diagnostic only'
rm "$T/repo/ane/h17/preflight.py"
H17_TEST_PREFLIGHT_CALLS=0 expect 'missing preflight still refuses without hardware' GO
if [[ $fails != 0 ]]; then
    printf 'test-stage4: %s failures\n' "$fails"
    exit 1
fi
printf 'test-stage4: ok\n'
