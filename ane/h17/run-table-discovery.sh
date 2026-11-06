#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Separate staged rung. Nothing runs without --run AND its one-shot marker.
# Stops before channel registration. Never unload ane_h16 after CPU release.
set -euo pipefail
if [[ ${1:-} != --run || $# != 1 || $EUID != 0 ]]; then
    echo 'usage (root, reviewed fresh-boot rung only): run-table-discovery.sh --run' >&2
    exit 2
fi
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
marker=/var/lib/omarchy-ane/h17-table.pending
manifest=/var/lib/omarchy-ane/h17-table.manifest.json
[[ -f $marker ]] || { echo 'REFUSED: table-discovery marker absent' >&2; exit 2; }
boot_id=$(cat /proc/sys/kernel/random/boot_id)
attempt_root=/var/lib/omarchy-ane/h17-table-attempts
log_dir=$attempt_root/$boot_id
cd "$repo"
GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0="$repo" \
    bash ane/h17/prepare-run.sh ane/h16/ane_h16.ko
if ! preflight_result=$(GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory \
        GIT_CONFIG_VALUE_0="$repo" python3 ane/h17/table_preflight.py \
        "$manifest" "$repo/ane/h16/ane_h16.ko" --repo "$repo" --claim); then
    printf '%s\n' "$preflight_result" >&2
    exit 2
fi
printf '%s\n' "$preflight_result"
exec > >(tee "$log_dir/run.log") 2>&1
printf '%s\n' "$preflight_result" > "$log_dir/preflight.json"
echo '{"rung":"FAIL","reason":"runner-incomplete"}' > "$log_dir/verdict.json"
echo "boot_id=$boot_id kernel=$(uname -r) date=$(date -Is) head=$(git -c safe.directory="$repo" rev-parse HEAD)"
sha256sum "$manifest" ane/h16/ane_h16.ko /usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho \
    /boot/efi/EFI/Linux/omarchy_linux-aurora.efi /boot/efi/m1n1/boot.bin
before=$(dmesg --color=never | wc -l)
# insmod is synchronous: RESULT is emitted before probe returns. Even a
# failed/refused post-release rung keeps the module bound and pinned.
status=0
insmod ane/h16/ane_h16.ko optin=t8140 stage=boot confirm_read=1 chanreg=1 || status=$?
dmesg --color=never | tail -n +"$((before + 1))" > "$log_dir/dmesg-rung.log"
if [[ $status != 0 ]]; then
    echo '{"rung":"FAIL","reason":"insmod-failed"}' > "$log_dir/verdict.json"
    exit 1
fi
status=0
python3 ane/h17/table_discovery_verdict.py "$log_dir/dmesg-rung.log" > "$log_dir/verdict.json" || status=$?
cat "$log_dir/verdict.json"
exit "$status"
