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

## SerialException root cause (added 2026-09-24 ~15:40)
Both 13.5 runs died 35 s after launch (catch log: launched 19:58:54, exited
19:59:29) with pyserial "device reports readiness to read but returned no
data" inside `Uart.readfull()`. That is a USB disconnect of the ACM. What
the guest was doing at that moment, read from the traces: the last ~500
lines of both logs are only ps_i2c6 (0x290280230) power cycles, 58
transactions in run a and 53 in run b, and nothing else. The J414c ADT
(DeviceTree.j414cap.im4p, decoded here) has exactly three children on
/arm-io/i2c6: atcrt0, atcrt1, atcrt2, the Type-C retimers (i2c 0x18-0x1a).
The macOS driver is com.apple.driver.AppleTypeCRetimer (strings: matches
"atcrt", apCommsTransaction, enablePolling, getRetimerState). The proxy port
is left-back = hpm0 = atc-phy0 = usb-drd0 (shared dock id 0x183) and the hv
runs over iodev USB0. m1n1's `setup_adt()` scrubs that port's dart-usb,
atc-phy, usb-drd, acio, hpm and dp nodes, but not its retimer, so macOS
reprogrammed the retimer on the proxy port and the link dropped.

Fix: `tools/m1n1-patches/0001-hv-scrub-atcrt.patch` (against upstream
b4654b3; the proxy-host copy is byte-identical there). It removes every
/arm-io/i2c*/atcrt* node when the hv runs over USB, per Main's decision to
scrub all three on the first rerun. Verified with the m1n1 ADT parser on
the real J414c ADT: three "Removing ADT node /arm-io/i2c6/atcrtN" lines,
the tree rebuilds and reparses. The patch applies to the hvproxy copy with
`patch -p1 --dry-run`.

## Retimer rerun negative + what the three deaths share (added 2026-09-24 ~16:00)
The atcrt0/1/2 scrub applied and logged (run.log lines 169-171), and the new
run shows no i2c6 burst at all, but it died at the same point: guest-launch
34 s, SerialException at the `hv_start` reply. Retimer writes were a
correlate, not the cause. Status of the earlier fix: keep the patch (it
removes a real hazard on the proxy port), but the 35 s death is open.

What all three deaths share (trace-135, trace-135b, trace-atcrt):
- The last traced guest writes are an ISP power-down walk: ps_ispsens1
  (0x290280130), ps_ispsens0 (0x290280128) T=0x4f,0 then 0x340, then
  ps_isp_sys (0x2902801c8) = 0x3f0. Runs a/b ended the same way on an
  i2c6 cycle; the retimer-free run ends here with only five stray device
  writes after it (ane islands/SET/ane_sys at fixed PCs).
- No PMGR HACK hook word is written after line ~4717 in any run: the guest
  never tried to gate the hooked UART0/ATC parents.
- No `CPU W` lines at all (that hv code path is dead in these runs).
- UART0 is NOT touched by the guest in any traced run: mapping
  /arm-io/uart0 is benign read-only attention evidence, so no UART0
  patch is proposed. The sure-visible UART0 register window never
  appears in the traced pmgr windows.
- I checked the PMGR hook semantics: `wh` forces the hardware write to
  `(v|0xf)&~0x400` but records the guest's raw value; AUTO/PS_RESET/TARGET
  bits in the guest value reach the log but not the hardware. The atc0 hook
  wording in this receipt's earlier section is corrected: UART0/ATC are
  hooked by device ancestry, not by pmgr-window offset.

Open item: whether the disconnect is the guest powering something on the
proxy data/power path, or a host-side stall that pyserial reports as a
disconnect. Next experiment: a marker writer (e.g. a localhost TCP line
per traced line, or a host-side pyserial frame counter with a read
timeout) so the next death carries a last-trace-time timestamp. That
distinguishes "guest idle while host stalls" from "host idle while guest
runs".

## Stand down: lazy ASC start (added 2026-09-24, per AneStaticStart via Main)
AneStaticStart proved the 13.5 ANE kext starts the ASC lazily: it powers the
coprocessor on only when a user client (aned/CoreML) opens it. Our hv guest
is kernel-only, so no kernel-only trace can ever contain the CPU_CONTROL /
mailbox sequence. The three 13.5 runs end at the ISP power-down walk not
because the trace is too short, but because the start sequence is never
issued inside that guest. This also closes the 35 s SerialException hunt on
the trace side: there is no later kext sequence for a longer run to reach.
A trace that captures the start needs a 13.5 userspace in the guest that
opens the ANE (an aned/CoreML inference workload). Recorded in
docs/t6021-ane-bringup-findings.md (trace section, superseded next step,
method lesson).
