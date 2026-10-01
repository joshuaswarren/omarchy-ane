#!/usr/bin/env bash
# A/B of the default-off TM parameters of ane.ko on the whole-encoder harness (jwm1, T8103). Run as the normal user on the target.
# Arms alternate (ABAB...) over ROUNDS rounds; each arm = rmmod ane, modprobe ane <params>, one harness invocation (3 reps).
# Pass rule per arm vs the default arm: median ane_exec_ms ratio; digest/transcript must match every rep. Target <= 116 ms.
# Usage: ab-tm-params.sh [ROUNDS]   (default 3).  tm_route3 is NOT in the default arm list: it touches an unproven block.
set -u
ROUNDS=${1:-3}
OUT=${OUT:-$HOME/ab-tm/$(date -u +%Y%m%dT%H%M%SZ)}
HARNESS=${PK_HARNESS:-/var/tmp/combined-parakeet.sh}
export COMBINED_SRC=${PK_SRC:-/var/tmp/IslandsExecJwm1/encoder-source} COMBINED_WHEEL_ALLOW=1
mkdir -p "$OUT"
ARMS=("default:" "extra2000:tm_tq_en_extra=0x2000" "prtykeep:tm_prty_keep=1" "both:tm_tq_en_extra=0x2000 tm_prty_keep=1")
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$OUT/run.log"; }
log "boot $(cat /proc/sys/kernel/random/boot_id) ane $(cat /sys/module/ane/version 2>/dev/null) rounds $ROUNDS out $OUT"
for r in $(seq 1 "$ROUNDS"); do
  for arm in "${ARMS[@]}"; do
    name=${arm%%:*}; params=${arm#*:}
    sudo -n rmmod ane || { log "rmmod ane failed"; exit 2; }
    sudo -n dmesg -C
    # shellcheck disable=SC2086
    sudo -n modprobe ane tm_log=1 $params || { log "modprobe failed for $name"; exit 3; }
    sleep 3
    log "round $r arm $name params '$params': $(sudo -n dmesg | grep -m2 'tm: TM_TQ_EN' | sed 's/.*tm: //' | tr '\n' ' ')"
    bash "$HARNESS" t8103-host > "$OUT/r$r-$name.log" 2>&1
    python3 - "$OUT/r$r-$name.log" "$r" "$name" <<'PYEOF' | tee -a "$OUT/results.tsv"
import json, sys
ms, ok = [], 0
for l in open(sys.argv[1]):
    if l.startswith("{"):
        d = json.loads(l)
        ms.append(d["ane_exec_ms"])
        ok += bool(d.get("transcript_match"))
print(f"{sys.argv[2]}\t{sys.argv[3]}\t{ok}/{len(ms)} match\t" + " ".join(f"{x:.2f}" for x in ms))
PYEOF
  done
done
python3 - "$OUT/results.tsv" <<'PYEOF'
import collections, statistics, sys
by = collections.defaultdict(list)
for l in open(sys.argv[1]):
    p = l.rstrip("\n").split("\t")
    by[p[1]] += [float(x) for x in p[3].split()]
base = statistics.median(by["default"])
for k, v in by.items():
    print(f"{k:10s} n={len(v):2d} median {statistics.median(v):7.2f} ms  ratio vs default {statistics.median(v) / base:.4f}")
PYEOF
log "done; restore: the last arm's params stay loaded until the next rmmod/modprobe ane (or reboot)"
