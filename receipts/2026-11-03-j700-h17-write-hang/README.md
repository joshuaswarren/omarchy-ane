# J700/H17: write path halted after a hang

Status: **hardware halted again, now including the read path.** The first
attempt at the RTBuddy write path coincided with a machine hang/reset.

## Evidence

- Previous boot ended at **13:09:15** with no shutdown sequence in the
  kernel log — the last lines are USB resets and the normal genpd
  `sync_state() pending` notices for the synthetic ANE consumers.
- The machine came back on boot `ad334b97…` at ~13:11.
- The RTBuddy probe source written just before the reset was **corrupt**
  (a block of NUL bytes, no code): the unclean shutdown truncated the
  write. It was never built, and the kernel log contains **no**
  `ane_h17_rtbuddy` line, so the module did not run.
- Therefore the hang is not attributed to a specific register write by
  this log. It is attributed to the write-path attempt window, and that
  is enough to stop.

## What this does and does not establish

- It does **not** prove that a SCRATCH or CPU_CONTROL write hung the
  machine; the module never executed.
- It **does** establish that the machine is currently unstable around
  these attempts, and that continuing to push engine writes is not
  defensible.

## Decision

- No engine writes. No further write-path attempts.
- The read path is also paused: the last several boots were short, and
  even read-only probes are not worth running on an unstable host until
  it is shown to stay up.

## Cleanup

- Corrupt `ane/h17/ane_h17_rtbuddy_probe.c` removed.
- `ane/h17/Makefile` reverted: the RTBuddy probe is not in `obj-m`.
- Quarantined stage-5 draft (`ane_h17_start_probe.c`) remains out of
  `obj-m` with `module_init` returning `-EPERM`.

## What is still valid

The passing measurements from stable boots stand as data, not as a
recipe: stage 3 (ANE_SYS), 3b (sub-domains off), 3c (raise to `0xf`),
stage 4 (engine read), mailbox read (`0x00020001`). The RTBuddy design and
protocol notes remain offline references.

## Recommendation

Before any further ANE work, establish host stability: confirm the
machine stays up across normal use, check for a hardware fault (these
resets have no kernel trace), and review whether the synthetic genpd
consumers should remain installed at all. The write path also needs an
independent review of the full seven-step sequence before another
attempt.

## Follow-up: the only abnormal hardware signal is USB

Across boots `024c93b4…`, `ad334b97…`, `6c6fda93…`, `878ab177…` the
kernel log has no ANE line, no panic, no watchdog, and no shutdown
sequence. The recurring signal is a USB mass-storage device resetting
continuously:

```
usb 1-1.1.4: SerialNumber: 20240418000000
usb-storage 1-1.1.4:1.0: USB Mass Storage device detected
usb 1-1.1.4: reset high-speed USB device number 4 using xhci-hcd   (8x in ~50 s)
```

Topology: root hub → VIA Labs USB2.0 hub (`1-1`) → QinHeng USB2.0 hub
(`1-1.1`) → this mass-storage device (`1-1.1.4`), alongside a QinHeng
CDC-Ethernet adapter (`1-1.1.5`); an iPhone is on `1-1.2`.

AC is online, the battery is charging, thermals are ~56–59 °C, and
`btrfs device stats` reports zero errors, so those are not implicated.

Until the USB chain is ruled out, ANE hardware work cannot be separated
from host resets. Suggested next step (host owner): unplug the generic
USB2.0 mass-storage device and the QinHeng hub, then observe whether the
reboot cycle stops before resuming ANE work.
