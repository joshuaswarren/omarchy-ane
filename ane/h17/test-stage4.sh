#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp/opencode}/h17-stage4-test.XXXXXX")
trap 'rm -rf -- "$T"' EXIT
mkdir -p "$T/bin" "$T/repo/ane/h17"
printf 'Apple MacBook Neo\0' > "$T/model"
printf 'fixture-boot\n' > "$T/bootid"
: > "$T/modules"
printf 'stale verdict=PASS reason=asc-snapshot\n' > "$T/dmesg"
touch "$T/repo/ane/h17/ane_h17_wake.ko"
cat > "$T/repo/ane/h17/preflight.py" <<'PY'
import os
raise SystemExit(int(os.environ.get('H17_TEST_PREFLIGHT_EXIT', '0')))
PY
for cmd in insmod rmmod modprobe wdctl; do
    cat > "$T/bin/$cmd" <<'SH'
#!/usr/bin/env bash
printf '%s\n' "$0 $*" >> "$H17_TEST_ROOT/hardware.calls"
exit 99
SH
    chmod +x "$T/bin/$cmd"
done
export H17_TEST_ROOT="$T" H17_REPO="$T/repo"
export H17_BOOT_ID_FILE="$T/bootid" H17_MODEL_FILE="$T/model" H17_MODULES_FILE="$T/modules"
export H17_MARKER_FILE="$T/missing/marker" H17_DMESG_FILE="$T/dmesg"
export INSMOD="$T/bin/insmod" RMMOD="$T/bin/rmmod" PATH="$T/bin:$PATH"
check() {
    local name=$1 rc=0
    shift
    : > "$T/hardware.calls"
    cp "$T/modules" "$T/before"
    bash "$here/run-stage4.sh" "$@" > "$T/out" 2>&1 || rc=$?
    if [[ $rc != 2 ]] || ! grep -qx 'H17_STAGE4_REFUSED reason=unqualified-register-map' "$T/out" ||
       [[ -s $T/hardware.calls || -e $H17_MARKER_FILE ]] || ! cmp -s "$T/before" "$T/modules"; then
        cat "$T/out" "$T/hardware.calls"
        printf 'FAIL %s\n' "$name"
        exit 1
    fi
    printf 'PASS %s\n' "$name"
}
check diagnostics
check 'GO refuses despite module file and stale PASS' GO
check 'unknown arguments refuse' anything
printf 'ane_h17_wake 1 0 - Live 0x0\n' > "$T/modules"
check 'loaded module untouched' GO
H17_TEST_PREFLIGHT_EXIT=2 check 'preflight failure' GO
rm "$T/repo/ane/h17/preflight.py"
check 'missing preflight' GO
printf 'test-stage4: ok\n'
