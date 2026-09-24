#!/bin/bash
# ascdbg.sh — drives ane_ascdbg.ko one bounded access at a time.
# Every command is appended to $LOG and fsynced BEFORE it runs, so a
# hard reset leaves the hostile address on disk (and on netconsole via
# the module's pre-print). Usage: ascdbg.sh <sequence> [args]
set -uo pipefail
DIR=${DIR:-/var/tmp/ascdbg}
LOG=$DIR/ascdbg.log
CMD=/sys/kernel/debug/ane_ascdbg/cmd

CPU_CONTROL=0x1400044
CPU_STATUS=0x1400048
RVBAR=0x1050000
EDPRCR=0x1010310
I2A=0x1408114
A2I=0x1408110
RECV0=0x1408830
SCRATCH0=0x1840048
SCRATCH7=0x1840064

cmd() {
  echo "$(date +%T.%N) CMD $*" >> "$LOG"; sync "$LOG"
  echo "$*" | sudo tee "$CMD" > /dev/null
  local rc=$?
  local out
  out=$(sudo cat "$CMD")
  echo "$out" | sed "s/^/$(date +%T.%N) RES /" >> "$LOG"; sync "$LOG"
  echo "$out"
  return $rc
}

load() {
  lsmod | grep -q '^ane_ascdbg' && return 0
  sudo insmod "$DIR/ane_ascdbg.ko" || { echo "insmod failed"; exit 1; }
  sleep 0.5
  dmesg | grep 'ane_ascdbg:' | tail -1 | tee -a "$LOG"
}

unload() { lsmod | grep -q '^ane_ascdbg' && sudo rmmod ane_ascdbg; }

status() {
  cmd r32 $CPU_STATUS
  cmd d32 $SCRATCH0 8
  cmd r32 $I2A
}

# --- sequences ---------------------------------------------------------

seq_baseline() {   # proven reads only
  status
  cmd r32 $A2I
  cmd r64 $RVBAR
}

seq_toplevel() {   # ISP-analog top-level words, one question each
  for off in 0x738 0x798 0x7f8 0x858 0x818 0x81c; do cmd r32 $off; done
  cmd d32 0x1400a00 6
}

seq_edprcr_read() { cmd r32 $EDPRCR; }

seq_isp_reset() {  # isp_reset_coproc analog, then RUN
  cmd r32 $CPU_STATUS
  cmd w32 $EDPRCR 0x2
  cmd r32 $CPU_STATUS
  cmd r32 $EDPRCR
  for off in 0x738 0x798 0x7f8 0x858; do cmd w32 $off 0xff00ff; done
  for i in 0 1 2 3 4 5; do cmd w32 $((0x1400a00 + 4*i)) 0xffffffff; done
  cmd p32 0x818 0xffffffff 0 200
  cmd p32 0x81c 0xffffffff 0 200
  cmd r32 $CPU_STATUS
  cmd d32 $SCRATCH0 8
  cmd r32 $I2A
  cmd w32 $CPU_CONTROL 0
  cmd r32 $CPU_STATUS
  cmd w32 $CPU_CONTROL 0x10
  cmd p32 $CPU_STATUS 0x2 0 3000
  cmd p32 $SCRATCH7 0xffffffff 0x8042006 5000
  status
  cmd r64 $RECV0
}

seq_run() {        # plain release (the campaign sequence) for the stall repro
  cmd r32 $CPU_STATUS
  cmd r32 $I2A
  cmd w32 $CPU_CONTROL 0
  cmd w32 $CPU_CONTROL 0x10
  cmd p32 $CPU_STATUS 0x2 0 3000
  cmd p32 $SCRATCH7 0xffffffff 0x8042006 5000
  status
}

seq_cmds() {       # run commands from stdin, one per line
  while read -r line; do [ -n "$line" ] && cmd $line; done
}

echo "=== $(date -Iseconds) seq=$1 up=$(uptime -p) ===" | tee -a "$LOG"
load
"seq_$1" "${@:2}"
