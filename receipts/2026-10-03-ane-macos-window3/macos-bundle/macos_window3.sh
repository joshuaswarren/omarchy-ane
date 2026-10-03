#!/bin/bash
# Window 3 on the M2 (T6021) macOS: a read-only dtrace of the ANE operating-point write path during
# three cold P6' ramps (AneClockHunt round 2, experiment E1). No register range is read, nothing is
# written, no kext is built or loaded. SIP is already off on this box (window 1 and 2).
#
#   SCRATCH=/Users/<user>/oracle-mint-scratch/window3 caffeinate -dimsu bash macos_window3.sh
#   PHASES="base w3env dtrace sums" (default)
#
# base   = window-1 env + inputs (identity, csrutil, sudo probe, stage manifest)
# w3env  = fbt listing of this kernel -> w3gen.py -> w3.d; dry compile (`dtrace -e`); fallbacks
#          without the proc clause, then without stack(); records probes.json and what is absent
# dtrace = trace running; 10 s idle; 3 x (spawn the window-2 P6' runner, 3000 calls, no warm-up);
#          6 s between spawns; 10 s idle; SIGINT; aggregates print at END
# Every phase leaves out/<phase>.done and is skipped on re-run. Fail-soft: any dtrace refusal leaves
# out/w3/SKIPPED with the reason and the window continues to sums.
set -u
S=${SCRATCH:?set SCRATCH to the staged scratch dir}
O=$S/out
W=$O/w3
PHASES=${PHASES:-"base w3env dtrace sums"}
BIN=$S/ane_inmem_run
IDLE_S=${IDLE_S:-10}
GAP_S=${GAP_S:-6}
CALLS=${CALLS:-3000}
mkdir -p "$W" "$S/tmp" "$S/w3run"
exec 3>&1

note() { echo "$(date -u +%FT%TZ) $*" | tee -a "$O/run.log" >&3; }
stamp() { printf 'ts=%s load=%s boottime=%s\n' "$(date -u +%FT%TZ)" "$(sysctl -n vm.loadavg)" "$(sysctl -n kern.boottime)"; }
sw2() { sudo -n python3 "$S/w2sample.py" "$@"; }
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

set_runner() { # set_runner OUTSUB -> RUN: the window-2 P6' probe, no warm-up, so call 1 is cold
  mkdir -p "$S/w3run/$1"
  RUN=(env TMPDIR="$S/tmp" "$BIN" "$S/p6prime/model.mil" "$S/p6prime/weights/weight.bin" "$S/w3run/$1" 0 "$CALLS"
    "in:$S/p6prime/in/p6prime-x.bin:524288" "out:y63:524288")
}

phase_base() { PHASES="env inputs" SCRATCH=$S bash "$S/macos_window.sh"; }

compile_ok() { # compile_ok FLAGS...: generate with the flags and dry-compile; keeps the first script that compiles
  python3 "$S/w3gen.py" "$W/fbt.txt" --out "$W/w3.d" --probes "$W/probes.json" "$@" 2>"$W/gen.err" || return 1
  sudo -n dtrace -e -s "$W/w3.d" >"$W/compile.out" 2>"$W/compile.err"
}

phase_w3env() {
  { echo "== sw_vers"; sw_vers; echo "== stamp"; stamp; echo "== csrutil"; csrutil status; echo "== dtrace"; which dtrace; } >"$W/env3.txt" 2>&1
  sudo -n true || { echo "SKIP: sudo needs a password" | tee "$W/SKIPPED"; return 0; }
  sudo -n dtrace -l -P fbt >"$W/fbt.txt" 2>"$W/fbt.err" || { echo "SKIP: fbt listing refused ($(head -2 "$W/fbt.err"))" | tee "$W/SKIPPED"; return 0; }
  wc -l "$W/fbt.txt"
  if compile_ok; then echo "w3.d: full script compiles"; echo full >"$W/variant"
  elif compile_ok --no-proc; then echo "w3.d: compiles without the proc clause ($(head -3 "$W/compile.err"))"; echo no-proc >"$W/variant"
  elif compile_ok --no-proc --no-stack; then echo "w3.d: compiles without proc and stack()"; echo no-proc-no-stack >"$W/variant"
  else echo "SKIP: w3.d does not compile ($(head -5 "$W/compile.err"; cat "$W/gen.err"))" | tee "$W/SKIPPED"; return 0; fi
  cat "$W/gen.err"
  grep -c '^fbt' "$W/w3.d"
}

phase_dtrace() {
  local dpid i
  [ -f "$W/SKIPPED" ] && { echo "SKIP: w3env skipped"; return 0; }
  sudo -n dtrace -q -s "$W/w3.d" -o "$W/trace.out" 2>"$W/dtrace.err" &
  dpid=$!
  sleep 5
  alive "$dpid" || { echo "SKIP: dtrace did not start ($(head -3 "$W/dtrace.err"))" | tee "$W/SKIPPED"; return 0; }
  stamp >"$W/trace-start.stamp"
  sleep "$IDLE_S"
  for i in 1 2 3; do
    set_runner "ramp-$i"
    sw2 series --out "$W/run" --json "$W/run/runner-$i.json" -- "${RUN[@]}"
    note "ramp $i rc=$? $(python3 -c "import json;d=json.load(open('$W/run/runner-$i.json'));e=d.get('exec_ms',[]);print('calls',len(e),'first',[round(x,3) for x in e[:3]],'last',round(e[-1],3) if e else None)" 2>/dev/null)"
    [ "$i" -lt 3 ] && sleep "$GAP_S"
  done
  sleep "$IDLE_S"
  sudo -n kill -INT "$dpid" 2>/dev/null
  wait "$dpid"
  stamp >"$W/trace-end.stamp"
  sudo -n chown -R "$(id -u)" "$W" 2>/dev/null
  wc -l "$W/trace.out"
  grep -c '^W ' "$W/trace.out" | sed 's/^/W lines: /'
  grep -c '^A ' "$W/trace.out" | sed 's/^/A lines: /'
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
    dtrace) phase dtrace phase_dtrace ;;
    sums) phase_sums ;;
    *) note "unknown phase $p"; exit 2 ;;
  esac || rc_all=$?
done
note "window3 done rc=$rc_all"
exit "$rc_all"
