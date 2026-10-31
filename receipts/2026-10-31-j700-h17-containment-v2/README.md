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
