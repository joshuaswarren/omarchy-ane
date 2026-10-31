#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Diagnostics (no argument) and the contained stage-4 experiment (GO) for
# the J700/T8140 H17 bring-up.
#
# No argument: print boot/model/commit diagnostics, run the read-only DT
# preflight, print the stage-4 marker tail if any, and exit 2 without any
# module lifecycle.
#
# GO: require a passing preflight and an unloaded wake module, record the
# run header in the marker file, then insmod ane_h17_wake.ko optin=1 once.
# The module's containment defaults cover the run: every pending engine
# access is appended to the O_SYNC marker file, and a containment timer
# on another CPU panics naming the access if one outlives contain_ms --
# recorded FAIL instead of a frozen machine. The panic= cmdline entry and
# the SoC watchdog are the recovery layers above the module's own watch.
# Exit 0 on a PASS verdict, 1 on a FAIL, 2 when the run was refused.
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=${H17_REPO:-$(cd -- "$here/../.." && pwd)}
boot_id_file=${H17_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
model_file=${H17_MODEL_FILE:-/proc/device-tree/model}
modules_file=${H17_MODULES_FILE:-/proc/modules}
marker_file=${H17_MARKER_FILE:-/var/lib/ane-h17/stage4-marker}
dmesg_file=${H17_DMESG_FILE:-}

read_text() {
    if [[ -r $1 ]]; then
        tr -d '\000' < "$1"
    else
        printf 'unavailable'
    fi
}

print_diagnostics() {
    printf 'H17 stage 4: register map qualified (hal-map + qualified-map receipts)\n'
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

if [[ ${1:-} != GO ]]; then
    printf '%s\n' \
        'H17_STAGE4_READY reason=map-qualified' \
        'No hardware access: GO runs the contained experiment once.'
    exit 2
fi

# GO: the contained hardware run.
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
if ! mkdir -p -- "$(dirname -- "$marker_file")"; then
    printf 'H17_STAGE4_REFUSED reason=marker-dir\n'
    exit 2
fi
if ! {
    printf 'run kernel=%s commit=%s boot_id=%s\n' \
        "${UNAME_R:-$(uname -r)}" \
        "$(git -C "$repo" rev-parse HEAD 2>/dev/null || printf unavailable)" \
        "$(read_text "$boot_id_file")"
    } >> "$marker_file"; then
    printf 'H17_STAGE4_REFUSED reason=marker-write\n'
    exit 2
fi
sync -f "$marker_file" 2>/dev/null || true

printf 'insmod %s optin=1\n' "$module_ko"
insmod_ok=1
loaded=0
if "$INSMOD" "$module_ko" optin=1; then
    loaded=1
else
    insmod_ok=0
fi
if [[ -n $dmesg_file ]]; then
    dmesg_out=$(tail -n 200 -- "$dmesg_file")
else
    dmesg_out=$(${DMESG:-dmesg} | tail -n 200)
fi
printf '%s\n' "$dmesg_out" | grep 'ane_h17_wake' | tail -n 20 || true
verdict=1
if printf '%s\n' "$dmesg_out" | grep -q 'verdict=PASS reason=asc-snapshot'; then
    printf 'H17_STAGE4_PASS reason=asc-snapshot\n'
    verdict=0
elif printf '%s\n' "$dmesg_out" | grep -q 'verdict=FAIL'; then
    printf 'H17_STAGE4_FAIL reason=verdict\n'
else
    printf 'H17_STAGE4_FAIL reason=insmod\n'
fi
if [[ $loaded == 1 ]] && ! "$RMMOD" ane_h17_wake; then
    printf 'H17_STAGE4_FAIL reason=rmmod\n'
    verdict=1
fi
exit $verdict
