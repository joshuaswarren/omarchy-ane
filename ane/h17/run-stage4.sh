#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Diagnostics (no argument), the containment rehearsal (REHEARSE), and the
# still-closed engine run (GO) for the J700/T8140 H17 bring-up.
#
# No argument: print boot/model/commit diagnostics, run the read-only DT
# preflight, print the stage-4 marker tail if any, and exit 2 without any
# module lifecycle.
#
# REHEARSE: require a passing preflight and an unloaded wake module,
# record the run header in the marker file (fsynced), raise
# kernel.panic to 30 (saved and restored on the alive path) so the
# containment panic reboots automatically, then insmod the module with
# contain_rehearse=1. The rehearsal touches no ANE register: it verifies
# the SoC watchdog is disowned and ticking, arms it, pins a containment
# timer on another CPU and waits for that timer to panic. If the machine
# reboots, the panic path worked (check the marker and the journal).
# If the runner survives, the containment is broken: the verdict is
# recorded and the exit code is 1.
#
# GO: refused while the gate is closed (exit 2, no lifecycle).
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=${H17_REPO:-$(cd -- "$here/../.." && pwd)}
boot_id_file=${H17_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
model_file=${H17_MODEL_FILE:-/proc/device-tree/model}
modules_file=${H17_MODULES_FILE:-/proc/modules}
marker_file=${H17_MARKER_FILE:-/var/lib/ane-h17/stage4-marker}
dmesg_file=${H17_DMESG_FILE:-}
panic_sysctl=${H17_PANIC_SYSCTL:-/proc/sys/kernel/panic}
INSMOD=${INSMOD:-insmod}
RMMOD=${RMMOD:-rmmod}
DMESG=${DMESG:-dmesg}

read_text() {
    if [[ -r $1 ]]; then
        tr -d '\000' < "$1"
    else
        printf 'unavailable'
    fi
}

print_diagnostics() {
    printf 'H17 stage 4: containment v2; gate closed for engine access\n'
    printf 'boot_id=%s kernel=%s\n' "$(read_text "$boot_id_file")" "${UNAME_R:-$(uname -r)}"
    printf 'model=%s\n' "$(read_text "$model_file")"
    printf 'repo=%s commit=%s\n' "$repo" "$(git -C "$repo" rev-parse HEAD 2>/dev/null || printf unavailable)"
    if [[ -r $modules_file ]]; then
        # Reading /proc/modules does not change an already-loaded probe's state.
        while IFS= read -r line; do
            case "$line" in
                ane_h17_*) printf 'loaded_module=%s\n' "$line" ;;
            esac
        done < "$modules_file"
    fi
    if [[ -f $marker_file ]]; then
        printf 'marker_tail_begin\n'
        tail -n 5 -- "$marker_file"
        printf 'marker_tail_end\n'
    fi
}

preflight_ok=0
print_diagnostics
if python3 "$repo/ane/h17/preflight.py"; then
    printf 'preflight_exit=0\n'
    preflight_ok=1
else
    printf 'preflight_exit=%s\n' "$?"
fi

mode=${1:-}
if [[ $mode != REHEARSE && $mode != GO ]]; then
    printf '%s\n' \
        'H17_STAGE4_READY reason=containment-v2' \
        'No hardware access: REHEARSE runs the containment rehearsal; GO stays refused.'
    exit 2
fi
if [[ $preflight_ok != 1 ]]; then
    printf 'H17_STAGE4_REFUSED reason=preflight\n'
    exit 2
fi
if grep -q '^ane_h17_wake ' "$modules_file" 2>/dev/null; then
    printf 'H17_STAGE4_REFUSED reason=module-loaded\n'
    exit 2
fi
module_ko=$repo/ane/h17/ane_h17_wake.ko
if [[ ! -f $module_ko ]]; then
    printf 'H17_STAGE4_REFUSED reason=module-missing\n'
    exit 2
fi

if [[ $mode == GO ]]; then
    printf 'H17_STAGE4_REFUSED reason=gate-closed\n'
    exit 2
fi

# REHEARSE: the contained rehearsal.
if ! mkdir -p -- "$(dirname -- "$marker_file")"; then
    printf 'H17_STAGE4_REFUSED reason=marker-dir\n'
    exit 2
fi
if ! {
    printf 'rehearsal kernel=%s commit=%s boot_id=%s\n' \
        "${UNAME_R:-$(uname -r)}" \
        "$(git -C "$repo" rev-parse HEAD 2>/dev/null || printf unavailable)" \
        "$(read_text "$boot_id_file")"
    } >> "$marker_file"; then
    printf 'H17_STAGE4_REFUSED reason=marker-write\n'
    exit 2
fi
sync -f "$marker_file" 2>/dev/null || true
sync -f "$(dirname -- "$marker_file")" 2>/dev/null || true

old_panic=$(cat "$panic_sysctl" 2>/dev/null) || old_panic=unknown
printf 30 > "$panic_sysctl" 2>/dev/null || true
if [[ $(cat "$panic_sysctl" 2>/dev/null) != 30 ]]; then
    printf 'H17_STAGE4_REFUSED reason=panic-timeout\n'
    exit 2
fi
printf 'kernel.panic raised to 30 (was %s); the containment panic reboots\n' "$old_panic"

printf 'insmod %s optin=1 contain_rehearse=1 marker_file=%s\n' \
    "$module_ko" "$marker_file"
if "$INSMOD" "$module_ko" optin=1 contain_rehearse=1 marker_file="$marker_file"; then
    # The rehearsal panics before init returns; surviving means the
    # containment did not fire.
    printf 'H17_STAGE4_FAIL reason=rehearsal-survived\n'
    verdict=1
else
    printf 'H17_STAGE4_FAIL reason=rehearsal-no-fire\n'
    verdict=1
fi
if [[ -n $dmesg_file ]]; then
    printf '%s\n' "$(tail -n 20 -- "$dmesg_file")" || true
else
    "$DMESG" | tail -n 20 || true
fi
if [[ $old_panic != unknown ]]; then
    printf '%s' "$old_panic" > "$panic_sysctl" 2>/dev/null || true
fi
exit $verdict
