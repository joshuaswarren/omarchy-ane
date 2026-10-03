#!/bin/bash
set -euo pipefail
# Native macOS ANE run: staged Qwen decode (p001 port dump, 10-prompt chunk) and the whole Parakeet
# encoder, compiled by this host's own ANE compiler through e5rt, plus direct compiles for HWX identity.
# Userspace only; everything stays under $SCRATCH and /tmp/qwen-real-full-1790790164. Resumable: a
# finished phase leaves out/<phase>.done. Between phases a gate waits for load, disk and thermal limits.
#
#   SCRATCH=/path/to/scratch PHASES="env dump chunk50 parakeet hwx chunkpp" bash run.sh
#   dry run: PROMPTS=1 NEW_TOKENS=2 WARMUP=0 HWX_PROGS=001 PHASES="env chunk50 parakeet hwx" ...
S=${SCRATCH:?set SCRATCH}
T=$S/tools
O=$S/out
mkdir -p "$O"
export PYTHONPATH=$S/site-packages ANEFORGE_PATH=$S/aneforge-src ANEFORGE_NO_AUTOBUILD=1 PYTHONDONTWRITEBYTECODE=1
export ANEFORGE_CACHE_DIR=${ANEFORGE_CACHE_DIR:-$S/afcache}
PY="nice -n 15 $S/py/bin/python3"
GGUF=$S/gguf/Qwen3.8-2B-Q4_K_M.gguf
REF=$T/native-macos/chunk_00.json
KEYS=$T/native-macos/qwen38-program-keys.txt
CAP=${HWX_CAPTURE_ROOT:-/tmp/qwen-real-full-1790790164}   # the Studio h14 batch root: equal-length strings
LOAD_MAX=${LOAD_MAX:-8} DISK_MAX=${DISK_MAX:-90}

log() { echo "$(date -u +%FT%TZ) $*" | tee -a "$O/run.log"; }

gate() {
  local load disk speed
  for _ in $(seq 1 10); do
    load=$(sysctl -n vm.loadavg | awk '{print $2}')
    disk=$(df -P /System/Volumes/Data | awk 'NR==2 {print $5 + 0}')
    speed=$(pmset -g therm | awk -F'= *' '/CPU_Speed_Limit/ {print $2 + 0}')
    if awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN {exit !(l < m)}' && [ "$disk" -lt "$DISK_MAX" ] \
       && [ "${speed:-100}" -ge 100 ]; then
      log "gate ok load=$load disk=$disk% cpu_speed_limit=${speed:-none}"
      return 0
    fi
    log "gate wait load=$load disk=$disk% cpu_speed_limit=${speed:-none}"
    sleep 60
  done
  log "gate FAILED, stopping"
  exit 3
}

phase() {
  local name=$1 t0 rc=0
  shift
  if [ -e "$O/$name.done" ]; then log "skip $name (done)"; return 0; fi
  gate
  log "start $name: $*"
  t0=$(date +%s)
  "$@" >> "$O/$name.log" 2>&1 || rc=$?
  log "end $name rc=$rc wall=$(( $(date +%s) - t0 ))s"
  [ "$rc" -eq 0 ] || exit "$rc"
  touch "$O/$name.done"
}

host_facts() {
  sw_vers; uname -a
  sysctl hw.model machdep.cpu.brand_string hw.memsize hw.ncpu kern.boottime kern.osversion
  df -h /System/Volumes/Data; pmset -g therm
  for f in ANECompiler ANEServices AppleNeuralEngine Espresso; do
    for p in /System/Library/PrivateFrameworks/$f.framework/Resources/version.plist \
             /System/Library/PrivateFrameworks/$f.framework/Resources/Info.plist; do
      [ -e "$p" ] && { echo "== $p"; plutil -p "$p" | grep -E 'Version|BuildVersion|ProjectName'; }
    done
  done
  $S/py/bin/python3 -V
  $PY -c 'import numpy, gguf; print("numpy", numpy.__version__, "gguf", gguf.__file__)'
  cat "$S/aneforge-src/COMMIT"
  shasum -a 256 "$S/aneforge-src/aneforge/_lib/libane_e5rt_dispatch.dylib" "$S/bin/ane-compile-hwx" "$S/bin/ane_inmem_run" \
    "$T"/native-macos/* "$T"/staged-qwen/dump_step_ports.py "$T"/hwx_h14_staged_to_anec.py
  ls -la ~/.cache/aneforge ~/Models/.aneforge-cache 2>&1 || true
}

env_phase() { host_facts > "$O/env.txt" 2>&1; cat "$O/env.txt"; }

hwx_phase() {
  local fp=$O/hwx-native-fingerprints.jsonl prog key
  : > "$fp"
  for n in ${HWX_PROGS:-$(seq -f %03g 0 37)}; do
    prog=prog_$n
    key=$(awk -v p="$prog" '$1 == p {print $2}' "$KEYS")
    mkdir -p "$CAP/captures/$prog"
    cp -c "$ANEFORGE_CACHE_DIR/$key/model.mil" "$ANEFORGE_CACHE_DIR/$key/weights.bin" "$CAP/captures/$prog/"
    /usr/bin/time -l nice -n 15 "$S/bin/ane-compile-hwx" "$CAP/captures/$prog" "$CAP/out/$prog.h14" h14
    $PY "$T/native-macos/hwx_sections.py" "$prog=$CAP/out/$prog.h14/model.hwx" >> "$fp"
    rm -rf "${CAP:?}/captures/$prog" "${CAP:?}/out/$prog.h14"
  done
  $PY "$T/native-macos/hwx_sections.py" --compare "$T/native-macos/studio-h14-fingerprints.jsonl" "$fp" \
    | tee "$O/hwx-compare.txt"
  [ -e "$S/parakeet/weights.bin" ] || cp -c "$S/parakeet/weights/weight.bin" "$S/parakeet/weights.bin"
  /usr/bin/time -l nice -n 15 "$S/bin/ane-compile-hwx" "$S/parakeet" "$S/hwx-parakeet" h14
  $PY "$T/native-macos/hwx_sections.py" "parakeet_encoder=$S/hwx-parakeet/model.hwx" > "$O/hwx-parakeet-fingerprint.jsonl"
  rm -rf "${S:?}/hwx-parakeet"
  $PY "$T/native-macos/hwx_sections.py" --compare "$T/native-macos/studio-h14-parakeet-fingerprint.jsonl" \
    "$O/hwx-parakeet-fingerprint.jsonl" | tee -a "$O/hwx-compare.txt"
}

parakeet_phase() {
  local run=(/usr/bin/time -l $PY "$T/native-macos/parakeet_native.py" --dir "$S/parakeet"
             --features "$S/parakeet/in/input_features.npy" --mask "$S/parakeet/in/attention_mask.npy")
  if ! "${run[@]}" --path e5rt --device-mask 4 --out "$O/parakeet"; then
    echo "ANE-only e5rt compile/run failed; fallback device mask 5 (CPU + ANE), NOT like-for-like"
    "${run[@]}" --path e5rt --device-mask 5 --out "$O/parakeet-mask5" || echo "e5rt mask 5 failed too"
  fi
  mkdir -p "$S/tmp"
  TMPDIR=$S/tmp "${run[@]}" --path inmem --inmem-bin "$S/bin/ane_inmem_run" --out "$O/parakeet-inmem" \
    || echo "in-memory path failed"
  ls "$O"/parakeet*/report.json
}

common=(--gguf "$GGUF" --ref "$REF" --keys "$KEYS" --env "$O/env.txt" --new-tokens "${NEW_TOKENS:-32}"
        --warmup "${WARMUP:-1}" --prompts "${PROMPTS:-0}")

for p in ${PHASES:-env dump chunk50 parakeet hwx}; do
  case $p in
    env) phase env env_phase ;;
    dump) phase dump /usr/bin/time -l $PY "$T/staged-qwen/dump_step_ports.py" --gguf "$GGUF" \
            --manifest "$T/native-macos/manifest.json" --ref "$REF" --steps 0,1,2,11,12,13 --new-tokens 3 \
            --out "$O/dump-p001" ;;
    chunk50) phase chunk50 /usr/bin/time -l $PY "$T/native-macos/qwen_chunk.py" "${common[@]}" \
               --max-len 50 --forced --out "$O/chunk" ;;
    parakeet) phase parakeet parakeet_phase ;;
    hwx) phase hwx hwx_phase ;;
    chunkpp) phase chunkpp /usr/bin/time -l $PY "$T/native-macos/qwen_chunk.py" "${common[@]}" \
               --max-len 0 --out "$O/chunk-pp" ;;
    *) log "unknown phase $p"; exit 2 ;;
  esac
done
log "phases done: ${PHASES:-env dump chunk50 parakeet hwx}"
