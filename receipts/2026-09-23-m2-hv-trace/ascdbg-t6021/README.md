# T6021 port of ane_ascdbg (staged 2026-09-24, not loaded)

Source: omarchy-ane f90f6c1 ane/h13/ane_ascdbg.c (T6001 core read).
Built on jw14m2-linux at /var/tmp/ascdbg/ane_ascdbg.ko, sha256 909eb1ab...
Driver scripts are copied beside it: ascdbg.sh and ascdbg_regs.py.

## Changes for T6021
- Looks up 284000000.ane by name. The device is unbound, so the legacy
  driver match does not apply.
- No pm_runtime raise. On this box the raise hung a bind. It gates on
  pmgr ane_cpu (0x28e0802e0) ACTUAL=0xf and refuses otherwise.

Loading reads only pmgr+0x2e0 and CPU_STATUS (engine+0x1400048), both
proven safe with the islands on. The CoreSight window (engine+0x1010000)
has not been read on T6021 in Linux. The hv/proxy rung-7 note says it
read all-zero and hung there. On T6001, DTRRX with the OS lock set
hard-reset the box.
