#!/bin/bash
# Window 3 on the T8103 laptop's macOS (encoder-gap E1a + E1b). Read-only: no kext, no MMIO, no writes.
#
#   SCRATCH=<staged dir> MAC_BUNDLE=<existing mac-reference-bundle dir on this Mac> \
#     caffeinate -dimsu bash macos_window3_t8103.sh
#   PHASES="base w3env perfstats ramp-p6 ramp-enc dtrace sums" (default)
#
# base      window-1 env + inputs (identity, csrutil, sudo probe, stage manifest)
# w3env     fbt listing -> w3gen.py (T8103 rows, runner names encoder_ramp + ane_inmem_run) -> w3.d,
#           `dtrace -e` dry compile with --no-proc / --no-stack fallbacks, else SKIPPED
# perfstats E1a part 1: ane_inmem_run with PERFSTATS=1 on P6' (3 warm-up + 20 calls); the firmware
#           perfStats dict comes back in perf_stats_first/last (empty {} on the M2's 26A428)
# ramp-p6   E1a part 2: 3 cold P6' spawns x 3000 calls (no warm-up), 6 s apart
# ramp-enc  E1a part 3: 3 cold whole-encoder spawns (encoder_ramp, CoreML .cpuAndNeuralEngine,
#           ENC_CALLS calls each, no warm-up) on the bundle that measured 113.27 ms in window 4
# dtrace    E1b: trace running; IDLE_S idle; 3 cold encoder spawns; IDLE_S idle; SIGINT
# Each phase leaves out/<phase>.done and is skipped on re-run. Fail-soft: a dtrace refusal leaves
# out/w3/SKIPPED and the window continues.
set -u
S=${SCRATCH:?set SCRATCH to the staged scratch dir}
MB=${MAC_BUNDLE:?set MAC_BUNDLE to the mac-reference-bundle dir (models/encoder.mlpackage, inputs/)}
O=$S/out
W=$O/w3
PHASES=${PHASES:-"base w3env perfstats ramp-p6 ramp-enc dtrace sums"}
BIN=$S/ane_inmem_run
ENC=$S/encoder_ramp
IDLE_S=${IDLE_S:-10}
GAP_S=${GAP_S:-6}
P6_CALLS=${P6_CALLS:-3000}
ENC_CALLS=${ENC_CALLS:-60}
PKG=$MB/models/encoder.mlpackage
FEAT=$MB/inputs/feat_f32.bin
MASK=$MB/inputs/mask_i32.bin
mkdir -p "$W" "$S/tmp" "$S/w3run"
exec 3>&1

note() { echo "$(date -u +%FT%TZ) $*" | tee -a "$O/run.log" >&3; }
stamp() { printf 'ts=%s load=%s boottime=%s\n' "$(date -u +%FT%TZ)" "$(sysctl -n vm.loadavg)" "$(sysctl -n kern.boottime)"; }
sw2() { sudo -n python3 "$S/w2sample.py" "$@"; }
w2() { python3 "$S/w2sample.py" "$@"; }
alive() { ps -p "$1" >/dev/null 2>&1; }

phase() {
  local name=$1 t0 rc=0
  shift
  [ -e "$O/$name.done" ] && { note "skip $name (done)"; return 0; }
  note "start $name"
  t0=$(date +%s)
  "$@" >>"$O/w3-$name.log" 2>&1 || rc=$?
  note "end $name rc=$rc wall=$(($(date +%s) - t0))s"
  [ "$rc" -eq 0 ] && touch "$O/$name.done"
  return "$rc"
}

summary() { # summary JSON: calls, first three, last
  python3 -c "import json,sys;d=json.load(open(sys.argv[1]));e=d.get('exec_ms',[]);print('calls',len(e),'first',[round(x,3) for x in e[:3]],'last',round(e[-1],3) if e else None,'perf_stats_first',str(d.get('perf_stats_first'))[:80])" "$1" 2>/dev/null
}

set_p6() { # set_p6 OUTSUB WARMUP CALLS -> RUN
  mkdir -p "$S/w3run/$1"
  RUN=(env TMPDIR="$S/tmp" "$BIN" "$S/p6prime/model.mil" "$S/p6prime/weights/weight.bin" "$S/w3run/$1" "$2" "$3"
    "in:$S/p6prime/in/p6prime-x.bin:524288" "out:y63:524288")
}

set_enc() { # set_enc OUTSUB -> RUN: cold whole encoder, CoreML, ANE units
  mkdir -p "$S/w3run/$1"
  RUN=(env TMPDIR="$S/tmp" "$ENC" "$PKG" "$FEAT" "$MASK" ane "$ENC_CALLS" "$S/w3run/$1/hidden.bin")
}

phase_base() {
  PHASES="env inputs" SCRATCH=$S bash "$S/macos_window.sh" || return 1
  { echo "== bundle"; ls -la "$PKG" "$FEAT" "$MASK"; shasum -a 256 "$FEAT" "$MASK"; echo "== sysctl"; sysctl hw.model machdep.cpu.brand_string; } >"$W/bundle.txt" 2>&1
  [ -d "$PKG" ] && [ -f "$FEAT" ] && [ -f "$MASK" ] || { echo "MAC_BUNDLE incomplete"; cat "$W/bundle.txt"; return 1; }
  [ -x "$ENC" ] || { echo "encoder_ramp missing: building"; xcrun swiftc -O -target arm64-apple-macosx14.4 -o "$ENC" "$S/encoder_ramp.swift" >"$W/encoder_ramp-build.log" 2>&1 || { cat "$W/encoder_ramp-build.log"; return 1; }; }
  shasum -a 256 "$ENC" "$BIN" >"$W/runners.sha256"
}

compile_ok() {
  python3 "$S/w3gen.py" "$W/fbt.txt" --out "$W/w3.d" --probes "$W/probes.json" --execname encoder_ramp,ane_inmem_run --tick-s 600 "$@" 2>"$W/gen.err" || return 1
  sudo -n dtrace -e -s "$W/w3.d" >"$W/compile.out" 2>"$W/compile.err"
}

phase_w3env() {
  { echo "== sw_vers"; sw_vers; echo "== stamp"; stamp; echo "== csrutil"; csrutil status; echo "== dtrace"; which dtrace; } >"$W/env3.txt" 2>&1
  sudo -n true || { echo "SKIP: sudo needs a password" | tee "$W/SKIPPED"; return 0; }
  sudo -n dtrace -l -P fbt >"$W/fbt.txt" 2>"$W/fbt.err" || { echo "SKIP: fbt listing refused ($(head -2 "$W/fbt.err"))" | tee "$W/SKIPPED"; return 0; }
  wc -l "$W/fbt.txt"
  if compile_ok; then echo full >"$W/variant"
  elif compile_ok --no-proc; then echo no-proc >"$W/variant"
  elif compile_ok --no-proc --no-stack; then echo no-proc-no-stack >"$W/variant"
  else echo "SKIP: w3.d does not compile ($(head -5 "$W/compile.err"; cat "$W/gen.err"))" | tee "$W/SKIPPED"; return 0; fi
  echo "w3.d variant $(cat "$W/variant")"; cat "$W/gen.err"; grep -c '^fbt' "$W/w3.d"
}

phase_perfstats() { # E1a part 1: the firmware perfStats dict through _ANERequest, P6', 3 warm-up + 20 calls
  local d=$W/perfstats
  mkdir -p "$d"
  set_p6 perfstats 3 20
  stamp >"$d/stamp"
  PERFSTATS=1 "${RUN[@]}" >"$d/p6prime.json" 2>"$d/p6prime.err"
  echo "rc=$? $(summary "$d/p6prime.json")"
  grep -i 'perfstats\|perf_stats' "$d/p6prime.err" "$d/p6prime.json" | cut -c1-200 | head -5
  [ -s "$d/p6prime.json" ]
}

phase_ramp_p6() {
  local i d=$W/ramp-p6
  mkdir -p "$d"
  for i in 1 2 3; do
    set_p6 "ramp-p6-$i" 0 "$P6_CALLS"
    w2 series --out "$d" --json "$d/runner-$i.json" -- "${RUN[@]}"
    note "ramp-p6 $i rc=$? $(summary "$d/runner-$i.json")"
    [ "$i" -lt 3 ] && sleep "$GAP_S"
  done
  [ -s "$d/runner-3.json" ]
}

phase_ramp_enc() {
  local i d=$W/ramp-enc
  mkdir -p "$d"
  for i in 1 2 3; do
    set_enc "ramp-enc-$i"
    w2 series --out "$d" --json "$d/runner-$i.json" -- "${RUN[@]}"
    note "ramp-enc $i rc=$? $(summary "$d/runner-$i.json")"
    shasum -a 256 "$S/w3run/ramp-enc-$i/hidden.bin" >>"$d/hidden.sha256"
    [ "$i" -lt 3 ] && sleep "$GAP_S"
  done
  [ -s "$d/runner-3.json" ]
}

phase_dtrace() { # E1b around three cold whole-encoder spawns
  local dpid i
  [ -f "$W/SKIPPED" ] && { echo "SKIP: w3env skipped"; return 0; }
  sudo -n dtrace -q -s "$W/w3.d" -o "$W/trace.out" 2>"$W/dtrace.err" &
  dpid=$!
  sleep 5
  alive "$dpid" || { echo "SKIP: dtrace did not start ($(head -3 "$W/dtrace.err"))" | tee "$W/SKIPPED"; return 0; }
  stamp >"$W/trace-start.stamp"
  sleep "$IDLE_S"
  for i in 1 2 3; do
    set_enc "dtrace-enc-$i"
    sw2 series --out "$W/run" --json "$W/run/runner-$i.json" -- "${RUN[@]}"
    note "dtrace ramp $i rc=$? $(summary "$W/run/runner-$i.json")"
    [ "$i" -lt 3 ] && sleep "$GAP_S"
  done
  sleep "$IDLE_S"
  sudo -n kill -INT "$dpid" 2>/dev/null
  wait "$dpid"
  stamp >"$W/trace-end.stamp"
  sudo -n chown -R "$(id -u)" "$W" 2>/dev/null
  wc -l "$W/trace.out"
  for k in W P PV A PA SEG; do printf '%s lines: %s\n' "$k" "$(grep -c "^$k " "$W/trace.out")"; done
  grep -E '^(TRACE-END|SEG)' "$W/trace.out"
}

phase_sums() {
  sudo -n chown -R "$(id -u)" "$O" 2>/dev/null
  (cd "$O" && find . -type f ! -name 'SHA256SUMS*' -exec shasum -a 256 {} + >SHA256SUMS) && wc -l "$O/SHA256SUMS"
}

rc_all=0
for p in $PHASES; do
  case "$p" in
    base) phase base phase_base ;;
    w3env) phase w3env phase_w3env ;;
    perfstats) phase perfstats phase_perfstats ;;
    ramp-p6) phase ramp-p6 phase_ramp_p6 ;;
    ramp-enc) phase ramp-enc phase_ramp_enc ;;
    dtrace) phase dtrace phase_dtrace ;;
    sums) phase_sums ;;
    *) note "unknown phase $p"; exit 2 ;;
  esac || rc_all=$?
done
note "window3-t8103 done rc=$rc_all"
exit "$rc_all"
