#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Diagnostics only. No argument or environment override enables hardware.
set -uo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=${H17_REPO:-$(cd -- "$here/../.." && pwd)}
boot_id_file=${H17_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
model_file=${H17_MODEL_FILE:-/proc/device-tree/model}
modules_file=${H17_MODULES_FILE:-/proc/modules}
read_text() {
    if [[ -r $1 ]]; then tr -d '\000' < "$1"; else printf unavailable; fi
}
printf 'H17 stage 4: quarantined; diagnostics only\n'
printf 'boot_id=%s kernel=%s\n' "$(read_text "$boot_id_file")" "${UNAME_R:-$(uname -r)}"
printf 'model=%s\n' "$(read_text "$model_file")"
printf 'repo=%s commit=%s\n' "$repo" "$(git -C "$repo" rev-parse HEAD 2>/dev/null || printf unavailable)"
if [[ -r $modules_file ]]; then
    while IFS= read -r line; do
        case "$line" in ane_h17_*) printf 'loaded_module=%s\n' "$line" ;; esac
    done < "$modules_file"
fi
if python3 "$repo/ane/h17/preflight.py"; then
    printf 'preflight_exit=0\n'
else
    printf 'preflight_exit=%s\n' "$?"
fi
printf '%s\n' 'H17_STAGE4_REFUSED reason=unqualified-register-map' \
    'Containment is unqualified. GO does not enable hardware access.'
exit 2
