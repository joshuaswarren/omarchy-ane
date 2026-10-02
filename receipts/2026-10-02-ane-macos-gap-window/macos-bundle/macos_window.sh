#!/bin/bash
# One-window T6021 macOS capture for the ANE 254 ms vs 89 ms gap: E1 probe timings (P6 compute-bound,
# P7 activation stream), the whole Parakeet encoder, ANERegDump snapshots of the fabric/dcs perf-state
# words idle and mid-loop, live iBoot ADT items via ioreg, and firmware perfStats through _ANERequest.
#
# Userspace except the ANERegDump kext route (needs root, a prior user approval, and one reboot the
# first time the kext is allowed). Idempotent: every phase leaves out/<phase>.done and is skipped on
# re-run; timing blocks are numbered and reused. Inputs are verified against stage_manifest.sha256
# before anything runs. Every timing block records load and uptime with it.
#
#   SCRATCH=/Users/<user>/oracle-mint-scratch/gap-window bash macos_window.sh
#   PHASES="env ioreg inputs regdump-idle timings regdump-load powermetrics sums" (this default order)
set -u
S=${SCRATCH:?set SCRATCH to the staged scratch dir}
O=$S/out
BIN=$S/ane_inmem_run
mkdir -p "$O"
PHASES=${PHASES:-"env ioreg inputs regdump-idle timings regdump-load powermetrics sums"}
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
  blk=$(grep -l '"rc": 0' "$dir"/block-*.json 2>/dev/null | wc -l | tr -d ' ')
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
  cp -p "$O/timings/p7/y.bin" "$O/timings/p7/y.last.bin" 2>/dev/null || true
  cp -p "$O/timings/pk/hidden.bin" "$O/timings/pk/hidden.last.bin" 2>/dev/null || true
  stamp >"$O/timings/timings-end.stamp"
  return "$rc"
}

regdump_run() { # $1 = outdir; kext built on-box on first use; graceful when root is unavailable
  local out=$1
  mkdir -p "$out"
  stamp >"$out/capture.stamp"
  if ! sudo -n true >/dev/null 2>&1; then
    echo "SKIP: sudo needs a password on this host; the kext regdump is not possible user-space" |
      tee "$out/SKIPPED"
    return 0
  fi
  if [ ! -x "$S/regdump-build/aneregdump" ]; then
    log "building ANERegDump on-box (kext + CLI + filter self-test)"
    zsh "$S/src/macos-regdump/build.sh" "$S/regdump-build" >"$out/regdump-build.log" 2>&1 || {
      echo "regdump build FAILED - see regdump-build.log" | tee "$out/SKIPPED"
      return 0
    }
  fi
  cp "$S/ranges-gapwin.txt" "$S/regdump-build/ranges.txt" 2>/dev/null || true
  if ! kmutil showloaded --list-only 2>/dev/null | grep -qi ANERegDump; then
    echo "kext not loaded; attempting kmutil load (needs a prior user approval)" | tee "$out/kmutil-attempt.txt"
    sudo -n kmutil load -p "$S/regdump-build/ANERegDump.kext" >"$out/kmutil.err" 2>&1 || {
      tee -a "$out/kmutil-attempt.txt" <"$out/kmutil.err"
      echo "If the error asks for approval: System Settings > Privacy & Security > Allow, reboot once," \
        "re-run this phase. Skipping the register capture this window." | tee -a "$out/kmutil-attempt.txt"
      return 0
    }
  fi
  sudo -n "$S/regdump-build/aneregdump" "$out/regdump" "$S/regdump-build/ranges.txt" </dev/null
  local rc=$?
  echo "regdump rc=$rc (0 = islands up, 3 = islands gated)"
  if [ "$rc" = 3 ]; then
    echo "islands gated (ANE power not all up at the moment of poll; this is the expected idle state for engine ranges)" >"$out/gated"
  else
    echo "ok" >"$out/state"
    [ -f "$out/regdump/fabric-ps.bin" ] && wc -c "$out/regdump"/fabric-ps.bin "$out/regdump"/dcs-ps.bin "$out/regdump"/dsid.bin 2>/dev/null
  fi
  return 0
}

phase_regdump_load() { # item 2 mid-loop: keep P6 evaluating while the kext samples the words
  local out=$O/04-regdump-load
  mkdir -p "$out"
  [ -f "$O/03-regdump-idle/SKIPPED" ] && { echo "SKIP: idle regdump was skipped" | tee "$out/SKIPPED"; return 0; }
  ensure_bin || return 1
  bash "$S/loopload.sh" >"$out/loop.log" 2>&1 &
  local lpid=$!
  sleep 3
  regdump_run "$out"
  touch "$S/loopload.stop"
  wait "$lpid" || true
  stamp >"$out/loop-end.stamp"
}

phase_powermetrics() { # item 5: whatever the tool prints on this build; no invented options
  mkdir -p "$O/05-powermetrics"
  if sudo -n true >/dev/null 2>&1; then
    bash "$S/loopload.sh" >"$O/05-powermetrics/loop.log" 2>&1 &
    local lpid=$!
    sleep 2
    sudo -n powermetrics --samplers ane_power -i 500 -n 10 </dev/null \
      >"$O/05-powermetrics/powermetrics.txt" 2>&1
    touch "$S/loopload.stop"; wait "$lpid" || true
  else
    powermetrics --samplers ane_power -i 500 -n 3 </dev/null \
      >"$O/05-powermetrics/powermetrics-unsudoed.txt" 2>&1
    echo "powermetrics without sudo rc=$? (refusal recorded; no root, no invented options)"
  fi
  [ -s "$O/05-powermetrics/powermetrics.txt" ] || [ -s "$O/05-powermetrics/powermetrics-unsudoed.txt" ]
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
  esac
  [ $? -ne 0 ] && rc_all=$?
done
log "bundle done rc=$rc_all"
exit "$rc_all"
