#!/bin/bash
# Window 4 on the M2 (T6021) macOS: read the ANE op-point token word (0x285869200, round 3) through the
# frozen ANERegDump kext during cold P6' ramps. Read-only: no kext build, no writes, no Linux contact.
#
#   SCRATCH=/Users/<user>/oracle-mint-scratch/window4 caffeinate -dimsu bash macos_window4.sh
#   PHASES="w2base first-opp-idle first-opp-load first-ctx-idle first-ctx-load ramp idle-tail sums" (default)
#   PHASES="w3env ramp-dtrace sums"   (pass B, optional: only after clean first reads)
#
# w2base     window-2 `base w2env`: identity, stage manifest, kext 932d3b9b + CLI check, series self-test
#            on the two proven words (falls back to single dumps of the window-1 CLI)
# first-*-idle   one dump of t1 + the block while the ANE idles: the kext forces the block gated, so the
#            expected result is status "gated" with no read (acceptance proof, zero risk)
# first-*-load   the first real read: P6' runner spawned, single gated dumps until one reports islands up
#            (as window 2's phase_first); the console shows the block and is synced before it
# ramp       3 cold P6' spawns x 3000 calls with the ~2 ms dump loop (window 2 measured 2.0 ms period)
# idle-tail  10 s of 10 ms dumps right after the last ramp (the power-off tokens while still powered)
# ramp-dtrace  the window-3 D script running while a 4th ramp is dumped at ~2 ms: write sequence and
#            read values on one clock (walltimestamp = CLOCK_REALTIME)
# STOP rule: a first read that drops the ssh session or reboots macOS is NOT retried; go to the return.
set -u
S=${SCRATCH:?set SCRATCH to the staged scratch dir}
O=$S/out
W=$O/w4
W2=$O/w2
W3=$O/w3
PHASES=${PHASES:-"w2base first-opp-idle first-opp-load first-ctx-idle first-ctx-load ramp idle-tail sums"}
BIN=$S/ane_inmem_run
CLI=$S/regdump-build/aneregdump
mkdir -p "$W/ranges" "$S/tmp" "$S/w4run"
exec 3>&1

note() { echo "$(date -u +%FT%TZ) $*" | tee -a "$O/run.log" >&3; }
stamp() { printf 'ts=%s load=%s boottime=%s\n' "$(date -u +%FT%TZ)" "$(sysctl -n vm.loadavg)" "$(sysctl -n kern.boottime)"; }
w2() { python3 "$S/w2sample.py" "$@"; }
sw2() { sudo -n env ${W2_SINGLE_CLI:+W2_SINGLE_CLI="$W2_SINGLE_CLI"} python3 "$S/w2sample.py" "$@"; }
alive() { ps -p "$1" >/dev/null 2>&1; }
regs_ok() { [ -f "$W2/regdump.ok" ]; }
W2_SINGLE_CLI=$(cat "$W2/single-cli" 2>/dev/null)

phase() {
  local name=$1 t0 rc=0
  shift
  [ -e "$O/$name.done" ] && { note "skip $name (done)"; return 0; }
  note "start $name"
  t0=$(date +%s)
  "$@" >>"$O/w4-$name.log" 2>&1 || rc=$?
  note "end $name rc=$rc wall=$(($(date +%s) - t0))s"
  [ "$rc" -eq 0 ] && touch "$O/$name.done"
  return "$rc"
}

set_runner() { # set_runner OUTSUB CALLS -> RUN: P6', no warm-up, call 1 cold
  mkdir -p "$S/w4run/$1"
  RUN=(env TMPDIR="$S/tmp" "$BIN" "$S/p6prime/model.mil" "$S/p6prime/weights/weight.bin" "$S/w4run/$1" 0 "$2"
    "in:$S/p6prime/in/p6prime-x.bin:524288" "out:y63:524288")
}

cleared() { # t1 plus every block whose first load read was ok
  local b list=t1
  for b in opp ctx; do [ -f "$W/cleared-$b" ] && list=$list,$b; done
  echo "$list"
}

ranges_file() { # ranges_file NAME BLOCKS POLL_US -> path (always gated: the kext forces it anyway)
  w2 ranges "$S/ranges-w4.txt" --blocks "$2" --mode gated --poll-us "$3" >"$W/ranges/$1.txt" && echo "$W/ranges/$1.txt"
}

phase_w2base() {
  PHASES="base w2env" SCRATCH=$S bash "$S/macos_window2.sh" || return 1
  W2_SINGLE_CLI=$(cat "$W2/single-cli" 2>/dev/null)
  regs_ok && echo "regdump ok (single-cli='${W2_SINGLE_CLI:-}')" || { echo "regdump SKIPPED: $(cat "$W2/regdump.skipped" 2>/dev/null)"; }
  # acceptance proof without any read: the CLI's own rule check (-n: no service call) on the window-4 request;
  # "rejected" means STOP before any first read
  "$CLI" -n "$S/ranges-w4.txt" 2>&1 | tee "$W/accept.txt"
  grep -qE 'rejected|too many' "$W/accept.txt" && { echo "REFUSED by the kext rule" | tee "$W/REFUSED"; return 1; }
  return 0
}

first_idle() { # first_idle BLOCK: one dump while the ANE idles; the block is forced gated -> expected "gated"
  local b=$1 rf d=$W/first-$1-idle
  regs_ok || { echo "SKIP: no regdump"; return 0; }
  [ -f "$W/REFUSED" ] && { echo "SKIP: request refused"; return 0; }
  rf=$(ranges_file "first-$b-idle" "t1,$b" 0xa)
  note "FIRST $b IDLE: $(grep '^range' "$rf" | grep -v -e fabric-ps -e dcs-ps | awk '{printf "%s@%s ", $2, $3}') (expected: gated, no read)"
  sync
  sw2 series --out "$d" --cli "$CLI" --ranges "$rf" --count 1 --period-us 0
  w2 show "$d/s/series.bin" | awk -F'\t' 'NR > 1 {print $4, $6, $8, $9}'
}

first_load() { # first_load BLOCK: the first real read, under a P6' runner, one dump once the islands are up
  local b=$1 rf d=$W/first-$1-load rc=0
  regs_ok || { echo "SKIP: no regdump"; return 0; }
  [ -f "$W/REFUSED" ] && { echo "SKIP: request refused"; return 0; }
  [ "$b" = ctx ] && [ ! -f "$W/cleared-opp" ] && { echo "SKIP ctx: opp not cleared"; return 0; }
  rf=$(ranges_file "first-$b-load" "t1,$b" 0x1e8480)
  note "FIRST $b LOAD: $(grep '^range' "$rf" | grep -v -e fabric-ps -e dcs-ps | awk '{printf "%s@%s ", $2, $3}') -- the first read of this word on macOS"
  sync
  set_runner "first-$b" 2000
  sw2 series --out "$d" --cli "$CLI" --ranges "$rf" --until-up --count 8 -- "${RUN[@]}" || rc=$?
  if w2 check "$d" --block "$b"; then
    touch "$W/cleared-$b"
    note "FIRST $b cleared (status ok): $(w2 show "$d"/a*/series.bin | awk -F"\t" -v b="$b" '$6 ~ "^"b"-" && $8 == "ok" {print $6 "=" $9}' | tail -3 | tr '\n' ' ')"
  else
    note "FIRST $b NOT cleared (rc=$rc): left out of every later phase"
  fi
}

phase_ramp() { # 3 cold P6' ramps, ~2 ms dumps, cleared blocks only
  local i rr
  [ -f "$W/cleared-opp" ] || { echo "SKIP: opp not cleared"; return 0; }
  rr=$(ranges_file ramp "$(cleared)" 0xa)
  for i in 1 2 3; do
    set_runner "ramp-$i" 3000
    sw2 series --out "$W/ramp-$i" --cli "$CLI" --ranges "$rr" --period-us 2000 --pre 2 --post 4 \
      --json "$W/ramp-$i/runner.json" -- "${RUN[@]}"
    note "ramp $i: $(python3 -c "import json;e=json.load(open('$W/ramp-$i/runner.json')).get('exec_ms',[]);print('calls',len(e),'first',[round(x,3) for x in e[:3]],'last',round(e[-1],3) if e else None)" 2>/dev/null)"
    sleep 6
  done
}

phase_idle_tail() { # 10 s at 10 ms right after the ramps: power-off tokens while the islands are still up
  [ -f "$W/cleared-opp" ] || { echo "SKIP: opp not cleared"; return 0; }
  set_runner tail 200
  sw2 series --out "$W/idle-tail" --cli "$CLI" --ranges "$(ranges_file tail "$(cleared)" 0xa)" --period-us 10000 \
    --pre 1 --post 10 --json "$W/idle-tail/runner.json" -- "${RUN[@]}"
}

phase_w3env() { # the window-3 D script for this kernel (fbt listing, generated, dry-compiled)
  PHASES="w3env" SCRATCH=$S bash "$S/macos_window3.sh"
}

phase_ramp_dtrace() { # optional pass B: dtrace writes + ~2 ms reads on one clock around a 4th cold ramp
  local dpid d=$W/ramp-dtrace
  [ -f "$W/cleared-opp" ] || { echo "SKIP: opp not cleared"; return 0; }
  [ -f "$W3/SKIPPED" ] || [ ! -f "$W3/w3.d" ] && { echo "SKIP: no w3.d ($(cat "$W3/SKIPPED" 2>/dev/null))"; return 0; }
  mkdir -p "$d"
  sudo -n dtrace -q -s "$W3/w3.d" -o "$d/trace.out" 2>"$d/dtrace.err" &
  dpid=$!
  sleep 5
  alive "$dpid" || { echo "SKIP: dtrace did not start ($(head -3 "$d/dtrace.err"))"; return 0; }
  sleep 5
  set_runner ramp-dtrace 3000
  sw2 series --out "$d" --cli "$CLI" --ranges "$(ranges_file ramp-dtrace "$(cleared)" 0xa)" --period-us 2000 \
    --pre 2 --post 6 --json "$d/runner.json" -- "${RUN[@]}"
  sleep 5
  sudo -n kill -INT "$dpid" 2>/dev/null
  wait "$dpid"
  sudo -n chown -R "$(id -u)" "$d" 2>/dev/null
  wc -l "$d/trace.out"
  grep -c '^W ' "$d/trace.out" | sed 's/^/W lines: /'
}

phase_sums() {
  sudo -n chown -R "$(id -u)" "$O" 2>/dev/null
  (cd "$O" && find . -type f ! -name 'SHA256SUMS*' -exec shasum -a 256 {} + >SHA256SUMS) && wc -l "$O/SHA256SUMS"
}

rc_all=0
for p in $PHASES; do
  case "$p" in
    w2base) phase w2base phase_w2base ;;
    first-opp-idle | first-ctx-idle) b=${p#first-}; phase "$p" first_idle "${b%-idle}" ;;
    first-opp-load | first-ctx-load) b=${p#first-}; phase "$p" first_load "${b%-load}" ;;
    ramp) phase ramp phase_ramp ;;
    idle-tail) phase idle-tail phase_idle_tail ;;
    w3env) phase w3env phase_w3env ;;
    ramp-dtrace) phase ramp-dtrace phase_ramp_dtrace ;;
    sums) phase_sums ;;
    *) note "unknown phase $p"; exit 2 ;;
  esac || rc_all=$?
done
note "window4 done rc=$rc_all"
exit "$rc_all"
