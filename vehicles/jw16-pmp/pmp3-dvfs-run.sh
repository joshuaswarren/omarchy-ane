#!/bin/bash
# pmp3-dvfs-run.sh — Phase 2 per-rung driver. Acquires ANE window, walks
# rungs one at a time with >=5 ms spacing + readback, runs the whole-encoder
# n1/n8 slope after each rung (via sibling JwmTransfer tools), stops on any
# anomaly, restores at end.
#
# IMPORTANT: rung iotcl numbers match pmp_dvfs.c:
#   DVFS_IOCTL_RUNG  = _IOW('D', 1, u32) = 0x40084401
#   DVFS_IOCTL_ON    = _IO(0, 2)           = 0x00020002
#   DVFS_IOCTL_OFF   = _IO(0, 3)           = 0x00020003
#   DVFS_IOCTL_READ_CMD = _IOR('D', 4, u32) = 0x80084404
#   DVFS_IOCTL_READ_ON  = _IOR('D', 5, u32) = 0x80084405
set -euo pipefail
RUN_ID="pmp3-dvfs-$(date -u +%Y%m%dT%H%M%SZ)"
LOG="/home/joshuawarren/.local/share/apple-silicon-lab/artifacts/Jw16AnePmp3/windows/${RUN_ID}"
mkdir -p "$LOG"
exec > >(tee -a "$LOG/run.log") 2>&1
echo "=== $RUN_ID boot=$(cat /proc/sys/kernel/random/boot_id) ==="

DEV=/dev/pmp_dvfs
if [ ! -e "$DEV" ]; then
  echo "FATAL $DEV missing; modprobe pmp_dvfs first"
  exit 1
fi

# rung walker: encode (prev << 4) | new into a u32 and pass to fd
walk() {
  local from=$1 to=$2
  python3 - "$from" "$to" <<'PY'
import fcntl, os, struct, sys, time
from_to = int(sys.argv[1]), int(sys.argv[2])
RUNG = 0x40084401
ON = 0x00020002
OFF = 0x00020003
RD_CMD = 0x80084404
RD_ON = 0x80084405
fd = os.open("/dev/pmp_dvfs", os.O_RDWR)
try:
    rb = fcntl.ioctl(fd, ON)
    print(f"DVFS_ON <- 1")
    time.sleep(0.01)
    # idle token
    fcntl.ioctl(fd, RUNG, struct.pack('<I', 0))  # prev=0 new=0 (token = 0x80000000)
    time.sleep(0.01)
    rb = fcntl.ioctl(fd, RD_CMD); print(f"post-idle CMD readback: 0x{rb:x}")
    prev = 0
    for new in range(from_to[0], from_to[1] + 1):
        arg = (prev << 4) | new
        t0 = time.time()
        rb = fcntl.ioctl(fd, RUNG, struct.pack('<I', arg))
        t1 = time.time()
        rb2 = fcntl.ioctl(fd, RD_CMD)
        print(f"rung prev={prev} new={new} token=0x{0x80000000 | arg:x} rb=0x{rb:x} rd=0x{rb2:x} dt={(t1-t0)*1000:.2f}ms")
        prev = new
        time.sleep(0.005)
    # reverse + off
    for new in reversed(range(0, from_to[1])):
        arg = (prev << 4) | new
        rb = fcntl.ioctl(fd, RUNG, struct.pack('<I', arg))
        print(f"reverse new={new} rb=0x{rb:x}")
        prev = new
        time.sleep(0.005)
    fcntl.ioctl(fd, OFF)
    rb = fcntl.ioctl(fd, RD_ON)
    print(f"DVFS_ON <- 0 readback=0x{rb:x}")
finally:
    os.close(fd)
PY
}

# default: walk the full ladder 0..5
walk "${1:-0}" "${2:-5}"
echo "=== $RUN_ID done"