# J700 H17: containment v2 and the rehearsal rung

2026-10-31. Branch `neo/h17-bringup`. Offline work only; no ANE MMIO, no
module load. This receipt closes the
[correction receipt](../2026-10-31-j700-h17-containment-correction/README.md):
the module implements containment v2, the runner has a rehearsal rung
that verifies the recovery path on hardware without touching any ANE
register, and the gate stays closed for engine access until that
rehearsal has run.

## How v2 maps to the correction's requirements

| requirement | v2 implementation |
| --- | --- |
| sourced register roles, widths, ordering, power | the gate stays 0: engine access is compiled but refused until rehearsal evidence; unsourced rows stay behind opt-ins |
| exclusive watchdog ownership, verified timeout/keepalive/disarm | the module maps `apple,t8140-wdt` (WD1: `0x10` CUR_TIME, `0x14` BITE_TIME, `0x1c` CTRL, `RESET_EN` = BIT 2 — the kernel's `drivers/watchdog/apple_wdt.c`), refuses unless `RESET_EN` reads clear, `BITE_TIME` is the apple_wdt driver's programmed value and CUR_TIME ticks, then arms, pets per access and disarms with readback |
| recovery independent of the stalled path | the engine phase runs in a kthread bound to one CPU (`kthread_create_on_cpu`); the containment timer is pinned (`add_timer_on`) to a different online CPU with an **absolute expiry set before arming** (the v1 defect), re-armed per access; if that CPU stalls the timer core of the other CPU fires and panics naming the pending access |
| mandatory durable records, rejecting storage failures | the marker file is opened `O_SYNC` and **mandatory**: an open failure refuses the run; a per-access write failure is remembered and aborts the run before the next access; the runner fsyncs the file and its directory |
| fault-injection tests | the sim exercises the rehearsal lifecycle against a fake WD1 register block (verify/tick/arm/disarm with readback, marker records, zero ANE MMIO) and carries two mutation controls: unqualifying the map and removing the rehearsal timer arm |

`kernel.panic` is raised to 30 by the runner (saved and restored on the
alive path, verified by readback) so the containment panic reboots
automatically; the accidental Oct 31 reset already demonstrated the WD1
reset path on this machine.

## What the rehearsal is

`run-stage4.sh REHEARSE`: preflight must pass and the module must be
unloaded; the runner records the run header (fsynced), raises
kernel.panic and runs `insmod ane_h17_wake.ko optin=1
contain_rehearse=1 marker_file=…`. The rehearsal touches no ANE
register: it verifies the watchdog is disowned and ticking, arms it,
pins the containment timer to a different online CPU and waits for the
panic (`ane_h17_wake REHEARSAL contain_ms=…`). The machine reboots; the
marker and the journal are the evidence. If the timer does not fire,
the rehearsal disarms the watchdog (verified), records
`rehearsal-no-fire` in the marker and exits with the machine alive — a
recorded FAIL.

## Verification

- `make -C ane/h17 sim-all`: snapshot/no-optin/wrong-board assert the
  refusal with zero registrations, ioremaps, PMGR reads, genpd votes,
  marker operations and timers; `rehearse` asserts the full rehearsal
  lifecycle including the no-fire failure branch; both mutants
  (gate removal, rehearsal-timer-arm removal) are killed.
- `make -C ane/h17 script-test`: diagnostics stay lifecycle-free for
  any argument; GO refuses with `reason=gate-closed`; REHEARSE is
  refused while loaded or on a failed preflight; the alive rehearsal
  path records the FAIL, passes `contain_rehearse=1` and
  `marker_file` to the module, raises and restores kernel.panic.
- The module rebuilt against kernel `7.2.11-6-1-ARCH`.

## Status

- Containment: implemented, sim-verified, **hardware rehearsal
  pending** — that is the next rung, and it reboots the machine by
  design.
- Register map: gate stays closed until the rehearsal evidence exists;
  the engine experiment (kthread, marker, watchdog, sourced rows) is
  compiled and gated behind `H17_REGISTER_MAP_QUALIFIED`.

## Rehearsal attempt 1 (2026-10-31, 06:48): refused on a pre-armed watchdog

The first `REHEARSE` attempt failed safely, as designed: the module
read WD1 `CTRL` with `RESET_EN` set and refused
(`reason=wdt-armed`) before arming anything. The machine stayed up,
the runner recorded the FAIL and restored `kernel.panic`.

Register evidence gathered read-only through `/dev/mem`
(`watchdog@3082b0000`, PA from the live DT):

- WD1 `CTRL=0x4` (`RESET_EN` set), `BITE_TIME=0x2aea5400` (the
  apple_wdt probe's 30 s at the ~24 MHz reference clock — measured
  23,991,636 ticks/s), `CUR_TIME` climbing: the watchdog was **armed
  and ~21 s from reset** at measurement time.
- The machine stayed up well past that, and a 5 s trace showed no
  pet: the kernel watchdog **core** is the petter — at boot,
  apple_wdt found `RESET_EN` set, marked the device
  `WDOG_HW_RUNNING`, and `watchdog_worker_should_ping()` keeps the
  core pinging a hardware-running watchdog (`watchdog_dev.c` of the
  same tree). So the hardware watchdog is owned by the kernel core on
  this boot, and the module's exclusive-ownership check correctly
  refused to fight it.
- The cause of the earlier 05:09 watchdog reset remains unattributed;
  the same core-and-firmware interaction is the candidate space.

## Ownership handoff (verified, no reboot)

The core's ownership was released through the proper API rather than
by register writes: open `/dev/watchdog` (exclusive),
`WDIOC_SETTIMEOUT` 30 (readback confirmed 30), write the magic `'V'`
and close — the core calls `apple_wdt_stop` (`CTRL <- 0`) and clears
`WDOG_HW_RUNNING`. Register readback after the close: `WD1_CTRL=0x0`,
`RESET_EN` clear — **disarmed and verified**. The machine stayed up.

With the watchdog disowned, the rehearsal rung is unblocked. It still
reboots the machine by design when run; it has not been run. Note: a
reboot may re-arm the watchdog through the same firmware/core path —
the handoff check (read `CTRL`, refuse if `RESET_EN` is set) must run
before every rehearsal and engine run.
