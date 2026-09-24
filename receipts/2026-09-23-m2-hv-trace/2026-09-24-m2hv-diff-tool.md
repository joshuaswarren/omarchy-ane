# m2hv_diff: macOS hv trace vs Linux write order, 2026-09-24

## Deliverable
- `tools/m2hv_diff.py` turns an m1n1 hv log into an ordered list of events:
  event #, log line, cpu, pc, R/W, width, addr, region, register, value. It
  reads two line types:
  - PrintTracer MMIO lines.
  - PMGR HACK hook lines. m1n1 marks those ps words RESERVED and does not
    trace them. The log records guest accesses to them as
    `PMGR W/R base+off:32`. Their order relative to the ASYNC MMIO lines is
    approximate.

  The tool collapses poll reads and repeated write blocks (period <= 8). It
  names each register from the following sources:
  - ane_t6021.h and ane_t6021_boot.h
  - the m1n1 ASCRegs and DART8110Regs maps
  - the t602x-pmgr.dtsi ps labels

  It reports the first macOS write that Linux never issues, or issues with a
  different value. For that write it gives the ADT device and where it falls
  in the Linux sequence. It also reports:
  - order inversions
  - the first divergence per region
  - the hv holes in the traced windows
  - the full diff table
  - the Linux-only writes
- `tools/m2hv_linux_baseline.txt` lists 251 ordered Linux writes inside the
  trace-v3 windows. It covers these phases:
  - pmgr probe, including the always-on fabric ancestors of ane_sys
  - dart probe reset
  - iommu attach
  - fwload TLB flushes
  - VENC rails
  - P0, P-1, P1, P2, P3, P5 and P6
  - RTKit mgmt
  - cpu_reset
  - ane_obs observer writes

  Each write is tagged live, if-off, gated or observer, and cites its source.
  The config is pinned to the rtb-valid run.

## Run
    python3 tools/m2hv_diff.py /tmp/m2hv            # newest log with MMIO lines
    python3 tools/m2hv_diff.py /tmp/m2hv/trace-135.log

## Validation
- `--selftest` passes: OK (29 events, 16 rows, baseline 251 writes).
- Format check: lines produced by the real m1n1 `PrintTracer.event_mmio`
  (pc161 proxyclient, serial stubbed) were byte-identical to the selftest
  generator on 5/5 lines (W.4, W.8+ and R.4). The hook-line samples in the
  selftest are copied verbatim from the 13.5 trace.
- 13.5 trace, trace-135.log (7795 lines): the tool parsed 3075 MMIO events,
  which equals `grep -c 'MMIO: '` on the same file, and 96 PMGR-hook events
  (36 W, 60 R). It skips `CPU STATE` and `Pass:` lines, which fall outside the
  traced windows. Result: 147 ANE-relevant write rows (MATCH 15, MATCH-C 14,
  VALUE 33, MISSING 85).
- trace-snap.log (76 lines) has 0 MMIO events. The guest was interrupted
  before any ANE access.

## What the 13.5 trace shows (read from the tool output)
- The traced run makes no macOS write to CPU_CONTROL, SCRATCH, RVBAR or the
  ASC mailbox: asc-wrapper, scratch and rvbar each have 0 rows. The ANE comes
  up and is then configured:
  1. ps_ane_sys cycles T=0 -> T=f -> AUTO.
  2. 26 engine-low writes.
  3. ps_ane_cpu goes on.
  4. DART setup on all three instances.

  After that, ane_set4..set1, ane_base, ane_td and ane_cpu are written to
  T=0; ane_sys_mpm is never written. The sequence ends with
  ane_SET+0x2dc = 0xf0040305 and ps_ane_sys T=0.
- Of the 12 Linux P-1 "grant tunable" offsets, macOS writes only two, both
  with other values: eng+0x0 = 0x11 (Linux 0x10) and eng+0x400 = 0xc0f10010
  (Linux 0x40010001). It never writes the other ten. The 26 engine-low
  writes are: eng+0x0; eng+0xc..0x34 (0xd/0xc/1/1/3...); eng+0x108..0x134
  (0x11/0xd/0xc/1/1/3...); eng+0x400; and eng+0xa00 = 0x01ffffff (Linux
  writes that value to eng+0x600).
- macOS sets TCR[15] = 0x2 (BYPASS_DART) on each DART, which Linux never
  does. It also programs dart-ane1/2 tunables at +0x20c..0x310 and
  UNK_TUNABLES, sets ENABLE_STREAMS = 0x1, writes DISABLE_STREAMS, and sets
  ERROR_DISABLE = 0xffffffff.

## Inference
- The DART reset assumes num_streams = 16, from the ENABLE readback of 0xffff.
- The pmgr island and VENC raises are if-off: Linux writes them only when the
  word reads off.

## Replay file (added 2026-09-24 ~15:10)
- `tools/m2hv_replay-trace-135.txt`: every macOS write to an ANE engine, DART
  or pmgr register in trace-135, uncollapsed, in trace order: 151 writes.
  Columns: evt, W<bits>, addr, value, linux, flags, region, register. The
  `linux` column is `same` for the 33 writes the Linux baseline already
  makes with the same value, `diff` for 33 where Linux writes another
  value, and `none` for 85 that Linux never writes.
- `tools/m2hv_replay-trace-135.h`: the same list as a C table
  (`struct m2hv_replay_w`, flags as `M2HV_F_*`), for the ane_obs observer or
  the driver.
- Flags an applier must respect: 13 `ps-off` writes set an ANE-chain ps
  word to TARGET=0 (the s24 kernel-context fatal class), 11 `set-win` writes
  go to the ane0 SET window (external-abort class from Linux), and 3 `hook`
  writes went through m1n1's PMGR HACK hook, which passed only bits [9:0] to
  hardware.
- Regenerate: `python3 tools/m2hv_diff.py TRACE --replay OUT.txt --replay-c OUT.h`.
- trace-135b.log (the second 13.5 run) yields the same 151 writes; only the
  three DART TTBR[0] page-table addresses differ (0x10047a59 vs 0x100488ad).
  Both traces end at the same write, ps_i2c6 (0x290280230) = 0x3f0, at
  event #3170.
