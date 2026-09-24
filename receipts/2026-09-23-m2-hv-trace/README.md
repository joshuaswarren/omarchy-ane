# M2 hv guest trace attempt 1 — 2026-09-23 night (UTC 2026-09-24)

Worktree: /tmp/m2kstart/omarchy-ane @ 30cc3a8 (feat/t6021-fw-debug).
M2 image on ESP: 43ec6090 (60 s wait/proxy image, NOT stock a3f533b9).
jwm1: Linux, proxy host. Kernelcache mac14j sha 8304156f (125059072 B).

## Stall-fix finding (CLOSED)

The 37.7 MB stall (4822 x 8192 B dots, twice, process blocked 0 CPU) was
stale proxy session state after killed transfers, NOT a transport or heap
limit. Evidence on a fresh session:
- Full 38.9 MB compressed writemem: 4749 chunks, 38903355 bytes, "writemem
  done", then gzdec -> 125059072 == expect, DONE.
- Heap probe: 1024 MB heap (base 0x1000e710000, top 0x1004e710000), 38.9 MB
  guarded malloc at 0x1000e820240 with end-probe writes ok.
- Fix: fresh proxy session per attempt; never reuse a session after a killed
  transfer. Uncompressed fallback staged, not needed.

## Guest run (PARTIAL — guest started, no ANE writes, proxy deaf after)

run_guest.py with trace_ane.py (asc-wrapper 0x285400000/0x14000, rvbar
0x285050000/0x100, coresight 0x285010000/0x2000, pmgr 0x28e080000/0x10000,
3x dart-ane, writes only):
- Load clean: all segments, SEPFW/TrustCache/preoslog copies, ADT upload,
  12 secondary CPUs started, entrypoint jump 0x1001cd84000.
- Guest executed: `[cpu0] Pass: mrs x13, ACC_CFG_EL1` + Skip msr. No ANE
  window write captured (trace log: 61 lines = PT maps + 2 cpu events).
- run_guest died in hv_start: P_HV_START never replies by design
  (proxy.c calls hv_start(), never returns); client SerialException on ACM0
  is a misread reply, not device death. Guest IS running.
- Proxy deaf after: fresh probe on ACM0 and ACM1 silent (25-50 s timeouts),
  ACM0/1 still enumerated, M2 not on ssh. hv exception loop needs the dead
  run_guest client to service traps; guest alive but stuck on next trap.

## Remote reboot (EXHAUSTED — needs Joshua power-button in the morning)

1. P_REBOOT on ACM0: sent, no reply, no USB change, no Linux. Same on ACM1.
2. ACM1 secondary proxy: same silence.
3. SoC watchdog via proxy: blocked, proxy takes no commands.
4. PD/VDM: macvdmtool is macOS IOKit-only (AppleHPMLib/CoreFoundation);
   jwm1 is on Linux. tps6598x cd321x present (0-0038/0-003f) but no VDM
   userspace for them. No path.
- Lesson: the 60 s fallback never fired because this proxy image has no
  timeout. Next proxy boot must use an image that falls back to Linux.

## Next attempt (morning, after power cycle)

1. M2 to Linux (power button). Re-stage ESP to a timeout image if one
   exists, else stock.
2. Fresh proxy session; trace WITHOUT the CoreSight window (rung 7:
   CoreSight reads all-zero, debug domain unclocked; reading it is known
   to hang) + hv verbose exceptions on (-e hook-exceptions).
3. Keep run_guest alive across hv_start (it never returns — hold the
   session, read the hv console on ACM1 via the live session, do NOT open
   a second client on ACM0; stop the m1n1-proxy-watcher first — it races
   new ACM appearances with its own ane_bringup run).
4. If guest reaches ANE start: capture write list (addr/value/order) for
   AneHunter rung 10.

## Logs

- /tmp/ane-hv-run.log on jwm1 (203 lines, sha on request).
- /tmp/ane-hv-trace.log on jwm1 (61 lines, sha ed68f54f0aedaae2).
- Scripts: m2proxy/probe.py, map_probe.py, chunk_test.py, do_reboot.py,
  fresh_reboot.py (this worktree /tmp/m2kstart/*.py originals).
