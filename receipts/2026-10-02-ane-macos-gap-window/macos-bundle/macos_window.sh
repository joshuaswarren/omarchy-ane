#!/bin/bash
# One-window T6021 macOS capture for the ANE 254 ms vs 89 ms gap: E1 probe timings (P6 compute-bound,
# P7 activation stream), the whole Parakeet encoder, ANERegDump snapshots of the fabric/dcs perf-state
# words idle and mid-loop, live iBoot ADT items via ioreg, and firmware perfStats through _ANERequest.
#
# Userspace except the ANERegDump kext route (root plus the already-approved frozen kext; this script
# never builds or loads another kext). Idempotent: every phase leaves out/<phase>.done and is skipped on
# re-run; timing blocks are numbered and reused. Inputs are verified against stage_manifest.sha256
# before anything runs. Every timing block records load and uptime with it. The register phases run
# last, so a kext fault cannot cost the timings (fetch them before the register phases).
#
#   SCRATCH=/Users/<user>/oracle-mint-scratch/gap-window bash macos_window.sh
#   PHASES="env ioreg inputs timings powermetrics regdump-idle regdump-load sums" (this default order)
set -u
S=${SCRATCH:?set SCRATCH to the staged scratch dir}
O=$S/out
BIN=$S/ane_inmem_run
mkdir -p "$O"
PHASES=${PHASES:-"env ioreg inputs timings powermetrics regdump-idle regdump-load sums"}
LOAD_MAX=${LOAD_MAX:-30}            # lower it for strictly quiet-machine timings if the window allows
BLOCKS=${BLOCKS:-20}
WARMUP=${WARMUP:-3}
CALLS=${CALLS:-20}

log() { echo "$(date -u +%FT%TZ) $*" | tee -a "$O/run.log"; }

stamp() { # load + uptime recorded next to every timing artifact
  local boot now
  boot=$(sysctl -n kern.boottime | sed -E 's/.*sec = ([0-9]+).*/\1/')
  case "$boot" in (*[!0-9]*|"") boot="";; esac
  now=$(date +%s)
  printf 'ts=%s load=%s uptime_s=%s\n' "$(date -u +%FT%TZ)" \
    "$(sysctl -n vm.loadavg | awk '{print $2, $3, $4}')" "$([ -n "$boot" ] && echo $((now - boot)) || echo unknown)"
}

gate() { # wait for load < LOAD_MAX; after 10 tries continue and let the stamps say so
  local load i
  for i in $(seq 1 10); do
    load=$(sysctl -n vm.loadavg | awk '{print $2}')
    awk -v l="$load" -v m="$LOAD_MAX" 'BEGIN {exit !(l < m)}' && { log "gate ok load=$load"; return 0; }
    log "gate wait load=$load ($i/10)"
    sleep 60
  done
  log "gate EXHAUSTED load=$load - continuing (stamps carry the load)"
}

phase() {
  local name=$1 t0 rc=0
  shift
  [ -e "$O/$name.done" ] && { log "skip $name (done)"; return 0; }
  log "start $name: $*"
  t0=$(date +%s)
  "$@" >>"$O/$name.log" 2>&1 || rc=$?
  log "end $name rc=$rc wall=$(( $(date +%s) - t0 ))s"
  [ "$rc" -eq 0 ] && touch "$O/$name.done"
  return "$rc"
}

phase_env() { # host identity, security posture, sudo probe (decides the kext route)
  {
    echo "== date"; date -u
    echo "== sw_vers"; sw_vers
    echo "== uname"; uname -a
    echo "== model"; sysctl hw.model machdep.cpu.brand_string hw.memsize hw.ncpu kern.boottime kern.osversion
    echo "== uptime/load"; uptime; sysctl vm.loadavg
    echo "== disk"; df -h /System/Volumes/Data
    echo "== therm"; pmset -g therm
    echo "== csrutil"; csrutil status 2>&1
    echo "== sudo probe"; sudo -n true >/dev/null 2>&1 && echo SUDO_NOPASS || echo SUDO_NEEDS_PASSWORD
    echo "== toolchain"; xcode-select -p 2>&1; clang --version 2>/dev/null | head -1
  } >"$O/00-env.txt" 2>&1
  grep -q "SystemVersion" "$O/00-env.txt" || sw_vers >>"$O/00-env.txt" 2>&1
  grep -q SUDO "$O/00-env.txt"
}

phase_ioreg() { # item 3: live iBoot-filled ADT items
  local n
  for n in mcc iop-pmp-nub ane0 pmgr dart-ane0; do
    ioreg -l -p IODeviceTree -n "$n" >"$O/01-ioreg-$n.txt" 2>&1 || log "ioreg $n rc=$? (recorded)"
  done
  wc -c "$O"/01-ioreg-*.txt
  grep -q '"mcc"' "$O/01-ioreg-mcc.txt" || grep -q "mcc" "$O/01-ioreg-mcc.txt" || log "WARN: mcc empty"
}

phase_inputs() { # staged inputs + goldens verified against the pinned manifest before any run
  (cd "$S" && shasum -a 256 -c stage_manifest.sha256) >"$O/02-input-check.txt" 2>&1
  local rc=$?
  [ "$rc" -ne 0 ] && { log "INPUT HASH MISMATCH - refusing to run"; return "$rc"; }
  grep -c ': OK' "$O/02-input-check.txt"
}

ensure_bin() { # inmem runner: staged binary, else build from the staged source on this box
  [ -x "$BIN" ] && return 0
  log "building ane_inmem_run on-box"
  clang -O2 -fobjc-arc -framework Foundation -framework IOSurface \
    "$S/src/ane_inmem_run.m" -o "$BIN" >>"$O/build.log" 2>&1
}

run_probe() { # run_probe <probe> <mil> <weights> <surface args...> - one numbered block
  local probe=$1 mil=$2 weights=$3 blk json rc
  shift 3
  local dir=$O/timings/$probe
  mkdir -p "$dir"
  blk=$(grep -lE '"rc": ?0[,}]' "$dir"/block-*.json 2>/dev/null | wc -l | tr -d ' ')
  [ "$blk" -ge "$BLOCKS" ] && return 0
  blk=$(printf %02d $((blk + 1)))
  stamp >"$dir/block-$blk.stamp"
  json=$dir/block-$blk.json
  TMPDIR=$S/tmp PERFSTATS=1 "$BIN" "$mil" "$weights" "$dir" "$WARMUP" "$CALLS" "$@" \
    >"$json" 2>"$dir/block-$blk.err"
  rc=$?
  python3 - "$json" "$dir/block-$blk.stamp" "$dir/blocks.tsv" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
ts = open(sys.argv[2]).read().strip()
ex = d.get("exec_ms") or []
med = sorted(ex)[len(ex) // 2] if ex else ""
with open(sys.argv[3], "a") as f:
    f.write(f"{sys.argv[1]}\t{min(ex) if ex else ''}\t{med}\t{len(ex)}\t{ts}\n")
PY
  log "probe $probe block $blk rc=$rc"
  return "$rc"
}

phase_timings() { # items 1 and 4: P6/P7/encoder, 20 blocks each, perfStats on every block
  ensure_bin || return 1
  gate
  local i rc=0
  for i in $(seq 1 "$BLOCKS"); do
    run_probe p6 "$S/p6/model.mil" "$S/p6/weights/weight.bin" \
      "in:$S/p6/in/p6-x.bin:524288" "out:y63:524288" || rc=$?
    [ -d "$S/p6prime" ] && { run_probe p6prime "$S/p6prime/model.mil" "$S/p6prime/weights/weight.bin" \
      "in:$S/p6prime/in/p6prime-x.bin:524288" "out:y63:524288" || rc=$?; }
    run_probe p7 "$S/p7/model.mil" "$S/p7/weights/weight.bin" \
      "in:$S/p7/in/p7-x.bin:33554432" "in:$S/p7/in/p7-z.bin:33554432" "out:y:33554432" || rc=$?
    if [ -f "$S/parakeet/in-in-features.bin" ]; then
      run_probe pk "$S/parakeet/model.mil" "$S/parakeet/weights/weight.bin" \
        "in:$S/parakeet/in-in-mask.bin:6016" "in:$S/parakeet/in-in-features.bin:768000" \
        "out:hidden:480000" "out:mask:768" || rc=$?
    else
      log "pk: encoder input bins absent from the stage - encoder arm skipped"
    fi
  done
  # keep the last block's outputs for host-side golden compare
  cp -p "$O/timings/p6/y63.bin" "$O/timings/p6/y63.last.bin" 2>/dev/null || true
  cp -p "$O/timings/p6prime/y63.bin" "$O/timings/p6prime/y63.last.bin" 2>/dev/null || true
  cp -p "$O/timings/p7/y.bin" "$O/timings/p7/y.last.bin" 2>/dev/null || true
  cp -p "$O/timings/pk/hidden.bin" "$O/timings/pk/hidden.last.bin" 2>/dev/null || true
  stamp >"$O/timings/timings-end.stamp"
  return "$rc"
}

# The installed, approved frozen kext (932d3b9b, sections 16-18 of the T6021 findings) and its CLI.
# Never build or load another kext: a new build needs an Allow click and a reboot (physical hands).
KEXT=/Library/Extensions/ANERegDump.kext
KEXT_SHA=932d3b9b70671a7eb35d6a3c0c3ebc991dac1024d71118efe5749f5ddbefdf2a
REGDUMP_CLI=${REGDUMP_CLI:-$HOME/ane-cap3/aneregdump}

regdump_run() { # $1 = outdir; graceful SKIP when root, the approved kext or its service is unavailable
  local out=$1
  mkdir -p "$out/regdump"
  stamp >"$out/capture.stamp"
  if ! sudo -n true >/dev/null 2>&1; then
    echo "SKIP: sudo needs a password on this host; the kext regdump is not possible user-space" |
      tee "$out/SKIPPED"
    return 0
  fi
  shasum -a 256 "$KEXT/Contents/MacOS/ANERegDump" >"$out/kext.sha256" 2>&1
  if ! grep -q "^$KEXT_SHA " "$out/kext.sha256"; then
    echo "SKIP: installed kext is not the approved frozen build ${KEXT_SHA:0:8} ($(cat "$out/kext.sha256"));" \
      "a rebuilt kext needs an Allow click + reboot" | tee "$out/SKIPPED"
    return 0
  fi
  if [ ! -x "$REGDUMP_CLI" ]; then # CLI source is unchanged since the frozen kext's commit 14620db
    REGDUMP_CLI=$S/regdump-build/aneregdump
    mkdir -p "$S/regdump-build"
    xcrun -sdk macosx clang -target arm64-apple-macosx14.0 -Wall -Werror -o "$REGDUMP_CLI" \
      "$S/src/macos-regdump/aneregdump.c" -framework IOKit >"$out/cli-build.log" 2>&1 &&
      codesign -s - --force "$REGDUMP_CLI" >>"$out/cli-build.log" 2>&1 ||
      { echo "SKIP: aneregdump CLI build failed (cli-build.log)" | tee "$out/SKIPPED"; return 0; }
  fi
  shasum -a 256 "$REGDUMP_CLI" >>"$out/kext.sha256"
  if ! kmutil showloaded --list-only 2>/dev/null | grep -qi ANERegDump; then
    sudo -n kmutil load -p "$KEXT" >"$out/kmutil.err" 2>&1 || {
      echo "SKIP: kmutil load of the approved kext refused: $(cat "$out/kmutil.err")" | tee "$out/SKIPPED"
      return 0
    }
  fi
  kmutil showloaded --list-only 2>/dev/null | grep -i ANERegDump >"$out/kext-loaded.txt"
  sudo -n "$REGDUMP_CLI" "$out/regdump" "$S/ranges-gapwin.txt" </dev/null
  local rc=$?
  sudo -n chown -R "$(id -u)" "$out"
  echo "regdump rc=$rc (0 = islands up, 3 = islands gated)"
  if [ "$rc" = 3 ]; then
    echo "islands gated (ANE power not all up at the moment of poll; gated ranges not read)" >"$out/gated"
  elif [ "$rc" = 0 ]; then
    echo "ok" >"$out/state"
  else
    echo "aneregdump rc=$rc" | tee "$out/SKIPPED"
  fi
  return 0
}

phase_regdump_load() { # item 2 mid-loop: P6/P7 evaluating while the kext samples; up to 8 attempts, stop after 2 passes
  local out=$O/04-regdump-load i ok=0
  mkdir -p "$out"
  [ -f "$O/03-regdump-idle/SKIPPED" ] && { echo "SKIP: idle regdump was skipped" | tee "$out/SKIPPED"; return 0; }
  ensure_bin || return 1
  bash "$S/loopload.sh" >"$out/loop.log" 2>&1 &
  local lpid=$!
  sleep 3
  for i in $(seq 1 8); do
    regdump_run "$out/a$i"
    [ -f "$out/a$i/SKIPPED" ] && break
    [ -f "$out/a$i/state" ] && ok=$((ok + 1))
    [ "$ok" -ge 2 ] && break
    sleep 1
  done
  echo "mid-loop attempts=$i passes=$ok"
  touch "$S/loopload.stop"
  wait "$lpid" || true
  stamp >"$out/loop-end.stamp"
}

phase_powermetrics() { # item 5: cpu_power prints "ANE Power" on this box (2026-09-25); ane_power attempted too
  local pm=$O/05-powermetrics
  mkdir -p "$pm"
  if sudo -n true >/dev/null 2>&1; then
    bash "$S/loopload.sh" >"$pm/loop.log" 2>&1 &
    local lpid=$!
    sleep 2
    sudo -n powermetrics --samplers cpu_power -i 500 -n 10 </dev/null >"$pm/powermetrics.txt" 2>&1
    echo "cpu_power rc=$?"
    sudo -n powermetrics --samplers ane_power -i 500 -n 3 </dev/null >"$pm/powermetrics-ane_power.txt" 2>&1
    echo "ane_power rc=$?"
    touch "$S/loopload.stop"; wait "$lpid" || true
  else
    powermetrics --samplers cpu_power -i 500 -n 3 </dev/null >"$pm/powermetrics-unsudoed.txt" 2>&1
    echo "powermetrics without sudo rc=$? (refusal recorded; no root, no invented options)"
  fi
  [ -s "$pm/powermetrics.txt" ] || [ -s "$pm/powermetrics-unsudoed.txt" ]
}

phase_sums() {
  (cd "$O" && find . -type f ! -name SHA256SUMS -exec shasum -a 256 {} + >SHA256SUMS) || return 1
  wc -l "$O/SHA256SUMS"
}

rc_all=0
for p in $PHASES; do
  case "$p" in
    env) phase env phase_env ;;
    ioreg) phase ioreg phase_ioreg ;;
    inputs) phase inputs phase_inputs ;;
    regdump-idle) phase regdump-idle regdump_run "$O/03-regdump-idle" ;;
    timings) phase timings phase_timings ;;
    regdump-load) phase regdump-load phase_regdump_load ;;
    powermetrics) phase powermetrics phase_powermetrics ;;
    sums) phase sums phase_sums ;;
    *) log "unknown phase $p"; exit 2 ;;
  esac || rc_all=$?
done
log "bundle done rc=$rc_all"
exit "$rc_all"
