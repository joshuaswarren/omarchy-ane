# J700 H17: hang containment shipped, register-map gate lifted

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`, branch
`neo/h17-bringup`. Offline work only: no ANE MMIO, no module load, no
reboot. This receipt lands criterion 4 of the
[hang receipt](../../receipts/2026-10-30-j700-h17-hang/README.md) — a
hang-containment design and implementation, so the outcome of the first
hardware run is a recorded FAIL instead of a frozen machine.

## The containment design

Three layers, so that no single failure mode loses the evidence:

1. **Pending marker (survives everything).** Before every engine access
   the module appends `pending access=<n> off=<x>` to `marker_file`
   (default `/var/lib/ane-h17/stage4-marker`), opened `O_SYNC`; after
   the last access it appends `done access=<n>`. Disk state survives a
   watchdog reset — the unsynced tee log is exactly what the first hang
   lost. `run-stage4.sh` writes the run header (kernel, commit, boot
   id) and syncs the file before the `insmod`, and prints the marker
   tail in every diagnostics run afterwards.
2. **Panic-on-timeout (the module's own watch).** A containment timer
   is pinned, with `add_timer_on`, to a CPU other than the one running
   the probe — a CPU stalled mid-MMIO cannot take timers, so the watch
   must not live on the same core — and every access pushes its
   deadline (`contain_ms`, default 1000, 0 = off). On expiry it panics
   naming the pending access: `ane_h17_wake HANG contain_ms=…
   access=… off=…`. A panic with `panic=` on the cmdline reboots
   automatically (repo precedent: `panic=30` on the t6021 DART work,
   receipts/2026-10-01-t6021-dart-kernel), and
   `panic_console_replay` shows the console on the built-in display
   after the reboot.
3. **SoC watchdog (the second layer).** `CONFIG_APPLE_WATCHDOG=y` on
   this kernel and `/dev/watchdog` is present; arming it
   (`systemd.watchdog_sec=` or a manual keeper) covers the case where
   the panic machinery itself wedges. Same precedent as above.

## Implementation

- `ane/h17/ane_h17_wake.c`: the containment state and timer, the
  `h17_engine_pre` record-and-refresh step inside every access
  helper (`h17_rd`, `h17_rdq`, `h17_wrq`), the `contain_ms` and
  `marker_file` parameters, and the gate lifted:
  `H17_REGISTER_MAP_QUALIFIED` is now 1 (criteria 1–3 landed with the
  [qualified-map receipt](../../receipts/2026-10-30-j700-h17-qualified-map/README.md)).
  The module rebuilt against kernel `7.2.11-6-1-ARCH`.
- `ane/h17/run-stage4.sh`: no-argument diagnostics (marker tail
  included, exit 2, no lifecycle) and `GO` — the contained run: preflight
  must pass, the module must be unloaded, the run header is recorded and
  synced, one `insmod ane_h17_wake.ko optin=1` (snapshot:
  `confirm_wake` defaults off), the dmesg tail and verdict
  (`H17_STAGE4_PASS`/`H17_STAGE4_FAIL`) are printed, and the module is
  removed on success. Exit 0 on PASS, 1 on FAIL, 2 when refused.
- `ane/h17/test-stage4.sh`: fixture regression for both paths — the
  diagnostics stay module-lifecycle-free, GO is refused while loaded or
  on a failed preflight, a passing GO performs exactly one insmod and
  one rmmod with the marker recorded, and a failed insmod is a recorded
  FAIL without rmmod.
- `ane/h17/sim/sim_harness.c` + `sim/shim.h`: the sim now runs the
  probe (the gate is qualified) and asserts per access that it sits
  inside the qualified map, that the marker records every pending
  access, and that the containment timer arms before the first engine
  access and disarms after the last; the `hang` mode drives the real
  panic path through the shim's `longjmp` and asserts the panic names
  the pending access. Two mutation controls — map unqualification and
  containment-timer removal — are each rejected.

## Verification

`make -C ane/h17 sim-all`: all seven sim modes pass (snapshot, wake,
peek, legacy-overrides, no-optin, wrong-board, hang) and both mutants
are killed. `make -C ane/h17 script-test`: all runner cases pass.
Host builds and sims are not device qualification: nothing in this
receipt is measured behaviour of the T8140 ASC.

## Status

All four release criteria of the hang receipt are met; the gate is
open. The first hardware run is one `run-stage4.sh GO` on a boot with
the containment cmdline (`panic=30 systemd.watchdog_sec=120`, repo
precedent) — a snapshot with `confirm_wake` off, containment on. The
hardware run itself remains unauthorized until the owner schedules it;
after a hang, reboot and re-read the marker, never rmmod/reload
mid-hang.
