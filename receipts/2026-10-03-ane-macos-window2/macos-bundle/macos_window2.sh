#!/bin/bash
# Window 2 on the M2 (T6021) macOS: where does macOS hold the ANE clock operating point?
# Read-only. Named pmgr words through the approved frozen ANERegDump kext (932d3b9b, runtime ranges
# only), sampled idle / across a P6' ramp-up and ramp-down / mid-load; the live IOService plane idle
# and mid-load; powermetrics, a log stream and (last, optional) a dtrace of the ANE perf path.
#
#   SCRATCH=/Users/<user>/oracle-mint-scratch/window2 caffeinate -dimsu bash macos_window2.sh
#   pass A (default): base w2env ioservice-idle tier1-idle first-set first-clk first-perf1
#                     first-dvfm logstream-start ramp load logstream-stop sums
#   pass B: PHASES="idle-raw sums"      pass C: PHASES="dtrace sums"      (fetch after each pass)
#
# Each phase leaves out/<phase>.done and is skipped on a re-run (sums always runs). Every first read
# of a never-read block is logged to the console and synced before it happens, so the operator's
# console shows which block was in flight if the machine resets. A tier-2 block joins the ramp,
# load and idle-raw phases only after its first exposure read status ok.
set -u
S=${SCRATCH:?set SCRATCH to the staged scratch dir}
O=$S/out
W=$O/w2
PHASES=${PHASES:-"base w2env ioservice-idle tier1-idle first-set first-clk first-perf1 first-dvfm logstream-start ramp load logstream-stop sums"}
BIN=$S/ane_inmem_run
CLI=$S/regdump-build/aneregdump
KEXT=/Library/Extensions/ANERegDump.kext
KEXT_SHA=932d3b9b70671a7eb35d6a3c0c3ebc991dac1024d71118efe5749f5ddbefdf2a
CLASS_RE='ANE|PMGR|CLPC|PerformanceController|PerfControl|ApplePMP'
LOGPRED='subsystem CONTAINS[c] "ane" OR process == "aned" OR senderImagePath CONTAINS "H11ANE" OR senderImagePath CONTAINS "PMGR" OR senderImagePath CONTAINS "CLPC"'
mkdir -p "$W/ranges" "$S/tmp" "$S/w2run"
exec 3>&1 # the operator's console: log lines reach it from inside phases too

note() { echo "$(date -u +%FT%TZ) $*" | tee -a "$O/run.log" >&3; }
stamp() { printf 'ts=%s load=%s boottime=%s\n' "$(date -u +%FT%TZ)" "$(sysctl -n vm.loadavg)" "$(sysctl -n kern.boottime)"; }
w2() { python3 "$S/w2sample.py" "$@"; }
sw2() { sudo -n env ${W2_SINGLE_CLI:+W2_SINGLE_CLI="$W2_SINGLE_CLI"} python3 "$S/w2sample.py" "$@"; }
alive() { ps -p "$1" >/dev/null 2>&1; } # works on root-owned pids (kill -0 does not)
regs_ok() { [ -f "$W/regdump.ok" ]; }
W2_SINGLE_CLI=$(cat "$W/single-cli" 2>/dev/null) # set by w2env when the series mode fails its self-test

phase() {
  local name=$1 t0 rc=0
  shift
  [ -e "$O/$name.done" ] && { note "skip $name (done)"; return 0; }
  note "start $name"
  t0=$(date +%s)
  "$@" >>"$O/w2-$name.log" 2>&1 || rc=$?
  note "end $name rc=$rc wall=$(($(date +%s) - t0))s"
  [ "$rc" -eq 0 ] && touch "$O/$name.done"
  return "$rc"
}

set_runner() { # set_runner OUTSUB CALLS -> RUN: P6' (the window-1 timing probe), no warm-up, so call 1 is cold
  mkdir -p "$S/w2run/$1"
  RUN=(env TMPDIR="$S/tmp" "$BIN" "$S/p6prime/model.mil" "$S/p6prime/weights/weight.bin" "$S/w2run/$1" 0 "$2"
    "in:$S/p6prime/in/p6prime-x.bin:524288" "out:y63:524288")
}

cleared() { # t1 plus every tier-2 block whose first exposure read ok
  local b list=t1
  for b in set clk perf1 dvfm; do [ -f "$W/cleared-$b" ] && list=$list,$b; done
  echo "$list"
}

ranges_file() { # ranges_file NAME BLOCKS MODE POLL_US -> path
  w2 ranges "$S/ranges-w2.txt" --blocks "$2" --mode "$3" --poll-us "$4" >"$W/ranges/$1.txt" && echo "$W/ranges/$1.txt"
}

phase_base() { # window-1 env (identity, csrutil, sudo probe) and the stage manifest check
  PHASES="env inputs" SCRATCH=$S bash "$S/macos_window.sh"
}

phase_w2env() {
  {
    echo "== sw_vers"; sw_vers
    echo "== stamp"; stamp
    echo "== sysctl hw"; sysctl hw
    echo "== sysctl machdep"; sysctl machdep
    echo "== sysctl kern"; sysctl kern.bootargs kern.osversion kern.boottime
    echo "== sysctl ane/clpc/perf"; sysctl -a 2>/dev/null | grep -iE 'ane|clpc|perf'
    echo "== pmset -g"; pmset -g
    echo "== pmset -g therm"; pmset -g therm
    echo "== pmset -g assertions"; pmset -g assertions
    echo "== csrutil"; csrutil status
    echo "== python3"; python3 --version
  } >"$W/env2.txt" 2>&1
  powermetrics -h >"$W/powermetrics-help.txt" 2>&1
  sudo -n true || { echo "SKIP regdump: sudo needs a password" | tee "$W/regdump.skipped"; return 0; }
  shasum -a 256 "$KEXT/Contents/MacOS/ANERegDump" >"$W/kext.sha256" 2>&1
  grep -q "^$KEXT_SHA " "$W/kext.sha256" ||
    { echo "SKIP regdump: kext is not the approved 932d3b9b build (a rebuild needs an Allow click)" | tee "$W/regdump.skipped"; return 0; }
  if ! kmutil showloaded --list-only 2>/dev/null | grep -qi ANERegDump; then
    sudo -n kmutil load -p "$KEXT" >"$W/kmutil.err" 2>&1 ||
      { echo "SKIP regdump: kmutil load refused" | tee "$W/regdump.skipped"; return 0; }
  fi
  kmutil showloaded --list-only 2>/dev/null | grep -i ANERegDump >"$W/kext-loaded.txt"
  if [ ! -x "$CLI" ]; then # staged Studio build missing: build the same source on-box
    mkdir -p "$S/regdump-build"
    xcrun -sdk macosx clang -target arm64-apple-macosx14.0 -Wall -Werror -o "$CLI" \
      "$S/src/macos-regdump/aneregdump.c" -framework IOKit >"$W/cli-build.log" 2>&1 &&
      codesign -s - --force "$CLI" >>"$W/cli-build.log" 2>&1 ||
      { echo "SKIP regdump: CLI build failed" | tee "$W/regdump.skipped"; return 0; }
  fi
  shasum -a 256 "$CLI" >>"$W/kext.sha256"
  # Self-test of the new series mode on the two proven words only: 3 dumps, all must parse ok.
  mkdir -p "$W/selftest"
  sudo -n "$CLI" "$W/selftest" "$(ranges_file selftest t1 raw 0xa)" 3 0 >"$W/selftest.out" 2>&1
  if [ "$(w2 show "$W/selftest/series.bin" 2>>"$W/selftest.out" | awk -F'\t' '$6 == "fabric-ps" && $8 == "ok"' | wc -l)" -ne 3 ]; then
    (cd "$HOME/ane-cap3" && shasum -a 256 -c SHA256SUMS 2>&1 | grep 'aneregdump: OK') >>"$W/selftest.out" ||
      { echo "SKIP regdump: series self-test failed and the window-1 CLI does not verify" | tee "$W/regdump.skipped"; return 0; }
    echo "$HOME/ane-cap3/aneregdump" >"$W/single-cli"
    echo "series self-test FAILED: falling back to single dumps of the window-1 CLI (slower cadence)"
  fi
  touch "$W/regdump.ok"
  W2_SINGLE_CLI=$(cat "$W/single-cli" 2>/dev/null)
}

phase_ioservice() { # ioservice TAG: full IOService plane + one plist per ANE/PMGR/CLPC/PMP class
  local d=$W/ioservice-$1 c
  mkdir -p "$d"
  stamp >"$d/stamp"
  ioreg -l -p IOService -w0 >"$d/ioservice-full.txt"
  grep -oE '<class [A-Za-z0-9_]+' "$d/ioservice-full.txt" | sed 's/<class //' | sort -u >"$d/classes-all.txt"
  grep -E "$CLASS_RE" "$d/classes-all.txt" >"$d/classes-w2.txt"
  while read -r c; do
    ioreg -r -a -l -w0 -c "$c" >"$d/$c.plist" 2>&1
  done <"$d/classes-w2.txt"
  wc -l "$d/classes-w2.txt"
}

phase_tier1_idle() { # proven words + the island words, 25 samples 200 ms apart, idle
  regs_ok || { echo "SKIP: no regdump"; return 0; }
  sw2 series --out "$W/tier1-idle" --cli "$CLI" --ranges "$(ranges_file tier1-idle t1 raw 0xa)" \
    --count 25 --period-us 200000
}

phase_first() { # first BLOCK: first ever read of a tier-2 block, gated, under P6' load, one dump
  local b=$1 rf d=$W/first-$1 rc=0
  regs_ok || { echo "SKIP: no regdump"; return 0; }
  rf=$(ranges_file "first-$b" "t1,$b" gated 0x1e8480)
  note "FIRST $b: $(grep '^range' "$rf" | grep -v -e fabric-ps -e dcs-ps | awk '{printf "%s@%s ", $2, $3}')"
  sync
  set_runner "first-$b" 2000
  sw2 series --out "$d" --cli "$CLI" --ranges "$rf" --until-up --count 8 -- "${RUN[@]}" || rc=$?
  if w2 check "$d" --block "$b"; then
    touch "$W/cleared-$b"
    note "FIRST $b cleared (status ok)"
  else
    note "FIRST $b NOT cleared (rc=$rc): left out of every later phase"
  fi
}

phase_ramp() { # three P6' ramps: 2 s idle, 3000 calls, 4 s ramp-down; 6 s apart; ~2 ms sampling
  local i rr
  regs_ok || { echo "SKIP: no regdump"; return 0; }
  rr=$(ranges_file ramp "$(cleared)" gated 0xa)
  for i in 1 2 3; do
    set_runner "ramp-$i" 3000
    sw2 series --out "$W/ramp-$i" --cli "$CLI" --ranges "$rr" --period-us 2000 --pre 2 --post 4 \
      --json "$W/ramp-$i/runner.json" -- "${RUN[@]}"
    sleep 6
  done
}

powermetrics_run() { # the requested sampler set; on refusal, every sampler that runs alone
  local pm=$1 s ok=()
  sudo -n powermetrics --samplers ane_power,cpu_power,thermal,smc -i 200 -n 20 </dev/null \
    >"$pm/powermetrics.txt" 2>"$pm/powermetrics.err" && return 0
  echo "requested set refused rc=$?: $(head -3 "$pm/powermetrics.err")"
  for s in ane_power cpu_power thermal smc; do
    sudo -n powermetrics --samplers "$s" -i 200 -n 1 </dev/null >/dev/null 2>&1 && ok+=("$s")
  done
  echo "accepted alone: ${ok[*]:-none}" | tee "$pm/samplers-accepted.txt"
  [ ${#ok[@]} -gt 0 ] || return 0
  sudo -n powermetrics --samplers "$(IFS=,; echo "${ok[*]}")" -i 200 -n 20 </dev/null >"$pm/powermetrics.txt" 2>&1
}

phase_load() { # loopload.sh (window 1: P6 2000 calls / P7 500 calls, alternating) running throughout
  local d=$W/load lpid
  mkdir -p "$d"
  bash "$S/loopload.sh" >"$d/loop.log" 2>&1 &
  lpid=$!
  sleep 3
  if regs_ok; then
    sw2 series --out "$d/regs" --cli "$CLI" --ranges "$(ranges_file load "$(cleared)" gated 0xa)" \
      --count 20 --period-us 500000
  fi
  phase_ioservice load
  if sudo -n true; then powermetrics_run "$d"; else echo "SKIP powermetrics: no sudo"; fi
  touch "$S/loopload.stop"
  wait "$lpid"
  stamp >"$d/loop-end.stamp"
}

phase_logstream_start() { # debug level if this build streams it, else info; log settings never changed
  local d=$W/log lv
  mkdir -p "$d"
  for lv in debug info; do
    sudo -n log stream --level "$lv" --style ndjson --predicate "$LOGPRED" >"$d/stream.ndjson" 2>"$d/stream-$lv.err" &
    echo $! >"$d/pid"
    sleep 2
    alive "$(cat "$d/pid")" && { echo "log stream level=$lv" | tee "$d/level"; return 0; }
  done
  echo "log stream refused at both levels"
}

phase_logstream_stop() {
  [ -f "$W/log/pid" ] && sudo -n kill -INT "$(cat "$W/log/pid")" 2>/dev/null
  sleep 1
  wc -l "$W/log/stream.ndjson"
}

phase_idle_raw() { # pass B: cleared tier-2 blocks read with no gate while the ANE idles
  local b rf
  regs_ok || { echo "SKIP: no regdump"; return 0; }
  sleep 5
  for b in set clk perf1 dvfm; do
    [ -f "$W/cleared-$b" ] || { echo "idle-raw $b: not cleared, skipped"; continue; }
    rf=$(ranges_file "idle-raw-$b" "t1,$b" raw 0xa)
    note "IDLE-RAW $b (ungated)"
    sync
    sw2 series --out "$W/idle-raw-$b" --cli "$CLI" --ranges "$rf" --count 1 --period-us 0
  done
}

phase_dtrace() { # pass C, optional and fail-soft: fbt on the PMGR/CLPC/ANE perf path around one ramp
  local d=$W/dtrace dpid
  mkdir -p "$d"
  sudo -n true || { echo "SKIP dtrace: no sudo" | tee "$d/SKIPPED"; return 0; }
  csrutil status >"$d/csrutil.txt" 2>&1
  sudo -n dtrace -l -P fbt >"$d/fbt.txt" 2>"$d/fbt.err" ||
    { echo "SKIP dtrace: fbt listing refused" | tee "$d/SKIPPED"; return 0; }
  w2 dtrace-gen "$d/fbt.txt" >"$d/w2.d" 2>"$d/gen.err" ||
    { echo "SKIP dtrace: no matching probes ($(cat "$d/gen.err"))" | tee "$d/SKIPPED"; return 0; }
  sudo -n dtrace -q -s "$d/w2.d" -o "$d/trace.out" 2>"$d/dtrace.err" &
  dpid=$!
  sleep 5
  alive "$dpid" || { echo "SKIP dtrace: did not start ($(head -3 "$d/dtrace.err"))" | tee "$d/SKIPPED"; return 0; }
  set_runner dtrace 3000
  sw2 series --out "$d/run" --pre 5 --post 5 --json "$d/run/runner.json" -- "${RUN[@]}"
  sudo -n kill -INT "$dpid" 2>/dev/null
  wait "$dpid"
  wc -l "$d/trace.out"
}

phase_sums() {
  sudo -n chown -R "$(id -u)" "$O" 2>/dev/null
  (cd "$O" && find . -type f ! -name 'SHA256SUMS*' -exec shasum -a 256 {} + >SHA256SUMS) && wc -l "$O/SHA256SUMS"
}

rc_all=0
for p in $PHASES; do
  case "$p" in
    base) phase base phase_base ;;
    w2env) phase w2env phase_w2env ;;
    ioservice-idle) phase ioservice-idle phase_ioservice idle ;;
    tier1-idle) phase tier1-idle phase_tier1_idle ;;
    first-set | first-clk | first-perf1 | first-dvfm) phase "$p" phase_first "${p#first-}" ;;
    logstream-start) phase logstream-start phase_logstream_start ;;
    ramp) phase ramp phase_ramp ;;
    load) phase load phase_load ;;
    logstream-stop) phase logstream-stop phase_logstream_stop ;;
    idle-raw) phase idle-raw phase_idle_raw ;;
    dtrace) phase dtrace phase_dtrace ;;
    sums) phase_sums ;;
    *) note "unknown phase $p"; exit 2 ;;
  esac || rc_all=$?
done
note "window2 done rc=$rc_all"
exit "$rc_all"
