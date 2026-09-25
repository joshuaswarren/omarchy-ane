#!/bin/bash
# Catch the M2 m1n1 proxy on the proxy host and boot a macOS kernelcache
# under the m1n1 hypervisor, with the guest console captured.
#
# m1n1 exposes two ACM ports: the proxy, then the hypervisor's virtual UART.
# m1n1 drops vuart bytes until the host opens that port (DTR), so the guest
# console, panic text included, is lost unless it is logged from launch.
# -d sets /chosen/debug-enabled and m2hv_guest_debug.py (kept next to this
# script) sets /chosen/asmb lp-sip0, the two gates XNU puts on debug=0x14e;
# with them a guest panic stops in the debugger instead of resetting the
# SoC, and wdt=-1 keeps the guest from arming the watchdog. The boot-args
# are the ones the Asahi m1n1-hypervisor guide uses for macOS guests.
#
# Usage: m2hv_catch_and_run.sh PROXYCLIENT_DIR KERNELCACHE TRACE_MODULE OUTDIR [RUN_GUEST_OPTION...]
# Env: M2HV_BOOTARGS overrides the default boot-args; M2HV_PREMOD names an
# extra hv module loaded between m2hv_guest_debug and the trace module
# (e.g. m2hv_ramdisk.py, with M2HV_RDIMG for the dmg path); M2HV_TIMEOUT
# bounds the run in seconds (default 5400). At the deadline a watchdog
# SIGINTs run_guest (upstream's "!" kick: the next guest exception drops
# to the hv shell), types p.reboot() into run_guest's stdin FIFO, and
# then SIGTERMs it, so a parked panic-spin or hung guest reboots the M2
# back to its self-falling-back ESP image instead of needing hands.
# Run it in tmux: after a panic the guest stays parked and the link stays
# up; C-c gives the hv shell, where p.reboot() restarts the M2.
PC=$1 KC=$2 MOD=$3 OUT=$4
DBG=$(cd "$(dirname "$0")" && pwd)/m2hv_guest_debug.py
BOOTARGS="${M2HV_BOOTARGS:-debug=0x14e serial=3 apcie=0xfffffffe -enable-kprintf-spam wdt=-1 clpc=0}"
mkdir -p "$OUT"
LOG=$OUT/catch.log
: > "$LOG"
say() { echo "$(date -u +%T) $*" | tee -a "$LOG"; }

if [ "$(uname)" = Darwin ]; then
    glob='/dev/cu.usbmodem*'
    holders() { lsof -t "$@" 2>/dev/null; }
else
    glob='/dev/ttyACM*'
    holders() { sudo -n fuser "$@" 2>/dev/null; }
fi

say "armed, waiting for two $glob ports"
ports=
for _ in $(seq 1 240); do
    ports=$(ls $glob 2>/dev/null | sort)
    [ "$(printf '%s\n' "$ports" | grep -c .)" -ge 2 ] && break
    sleep 0.5
done
proxy=$(printf '%s\n' "$ports" | sed -n 1p)
vuart=$(printf '%s\n' "$ports" | sed -n 2p)
if [ -z "$vuart" ]; then
    say "proxy and vuart ports not both present within 120 s: $ports"
    exit 1
fi
say "proxy $proxy, vuart $vuart"

for pid in $(holders "$proxy" "$vuart"); do
    say "foreign holder $pid: $(ps -o command= -p "$pid")"
    kill "$pid" 2>/dev/null || sudo -n kill "$pid"
done

python3 -u - "$vuart" "$OUT/vuart.log" <<'EOF' &
import sys, time, serial
port, path = sys.argv[1:]
stamp = lambda: time.strftime("%H:%M:%S ", time.gmtime()).encode()
buf = b""
with open(path, "ab", buffering=0) as f:
    try:
        s = serial.Serial(port, timeout=0.2)
        f.write(stamp() + b"vuart open " + port.encode() + b"\n")
        while True:
            *lines, buf = (buf + s.read(4096)).split(b"\n")
            for line in lines:
                f.write(stamp() + line + b"\n")
    except serial.SerialException as e:
        f.write(stamp() + buf + b"\n" + stamp() + f"vuart lost: {e}\n".encode())
EOF
vpid=$!

cd "$PC" || exit 1
PREMOD=( )
if [ -n "${M2HV_PREMOD:-}" ]; then
    PREMOD=( -m "$M2HV_PREMOD" )
fi
FIFO=$OUT/stdin.fifo
rm -f "$FIFO" && mkfifo "$FIFO"
say "launching run_guest -d, boot-args: $BOOTARGS"
M1N1DEVICE="$proxy" python3 -u tools/run_guest.py -d "${@:5}" -m "$DBG" "${PREMOD[@]}" -m "$MOD" \
    -l "$OUT/trace.log" "$KC" -- "$BOOTARGS" <"$FIFO" 2>&1 | tee -i "$OUT/run.log" &
rgpid=$!
( sleep "${M2HV_TIMEOUT:-5400}"
  if kill -0 "$rgpid" 2>/dev/null; then
      say "timeout: SIGINT to hv shell, then p.reboot()"
      kill -INT "$rgpid"
      sleep 20
      printf 'p.reboot()\n' >"$FIFO"
      sleep 30
      kill -0 "$rgpid" 2>/dev/null && kill "$rgpid"
  fi ) &
wdpid=$!
wait "$rgpid"
rc=$?
kill "$wdpid" "$vpid" 2>/dev/null
say "run_guest exited rc=$rc"
exit "$rc"
