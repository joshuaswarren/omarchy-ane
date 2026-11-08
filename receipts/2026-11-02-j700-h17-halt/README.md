# J700/H17: hardware ladder halted after repeated short boots

Status: **all ANE hardware access stopped.** No engine read, engine write,
genpd vote, or PMGR probe from this point until a controlled,
reboot-tolerant window is agreed.

## What happened

Boot history (`journalctl --list-boots`):

| Boot | From | To |
| --- | --- | --- |
| `1d31ba24…` | — | 2026-11-02 06:50 |
| `3134730e…` | 06:50 | 2026-11-02 20:20 |
| `dcfe04e7…` | 20:20 | 2026-11-02 20:23 |
| `ee7d48fa…` | 20:25 | current |

- The stage-4 engine read passed on `3134730e…` at 17:45:32
  (`CPU_STATUS=0x30006a`, `RVBAR=0x10000000001`), and the machine kept
  running for ~2.5 hours afterwards.
- On `dcfe04e7…` a repeat engine read was attempted. The shell tool was
  aborted before its result returned, and the machine rebooted to
  `ee7d48fa…`.
- `journalctl -b -1 -k` for `dcfe04e7…` contains **no** `ane_h17` line, so
  the `insmod` most likely never executed. The reboot correlation is
  therefore **unproven**, not established.
- No panic, oops, SError, external abort, or ANE line appears in the
  previous-boot kernel logs.

Interpretation: two boots of ~3 minutes each, one during a tool
interruption, are enough to stop and re-plan. This receipt does not claim
the ANE caused the reboot, and it does not claim it did not.

## Decision

Hardware work pauses. The remaining stages (engine write, firmware RUN
release, SCRATCH7 poll) need a session where a reboot is expected and
recoverable, plus independent review of the write sequence.

## State preserved

- Stage-5 module `ane/h17/ane_h17_start_probe.c` is authored and
  Makefile-wired but **has not run**. It writes only `CPU_CONTROL` (`0`
  then `0x10`), polls `SCRATCH7` for `0x08042006` for 500 ms, and stops
  the CPU before releasing the domains. It deliberately omits the pre-CPU
  engine-table writes and any RVBAR write.
- Passing results retained: stage 3 (ANE_SYS), stage 3b (sub-domains off),
  stage 3c (sub-domains raise), stage 4 (first engine read).

## Safe work still possible

- Offline: source review of the stage-5 write sequence against the
  shipped H14G boot code; userspace sim coverage; register-map notes;
  docs and receipts.
- Read-only system checks that do not touch ANE MMIO: DT/sysfs, preflight,
  module verification.
