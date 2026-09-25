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
# Run it in tmux: after a panic the guest stays parked and the link stays
# up; C-c gives the hv shell, where p.reboot() restarts the M2.
set -u
[ $# -ge 4 ] || { sed -n 2,16p "$0"; exit 2; }
PC=$1 KC=$2 MOD=$3 OUT=$4
DBG=$(cd "$(dirname "$0")" && pwd)/m2hv_guest_debug.py
BOOTARGS="debug=0x14e serial=3 apcie=0xfffffffe -enable-kprintf-spam wdt=-1 clpc=0"
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
say "launching run_guest -d, boot-args: $BOOTARGS"
M1N1DEVICE="$proxy" python3 -u tools/run_guest.py -d "${@:5}" -m "$DBG" -m "$MOD" \
    -l "$OUT/trace.log" "$KC" -- "$BOOTARGS" 2>&1 | tee -i "$OUT/run.log"
rc=${PIPESTATUS[0]}
say "run_guest exited rc=$rc"
kill "$vpid" 2>/dev/null
exit "$rc"
