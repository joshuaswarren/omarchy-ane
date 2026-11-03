# J700/H17: ANE probes correlate with PMU-level resets

Status: **all ANE hardware work stopped.** The probes themselves are now
the leading suspect for the host resets.

## Timeline that matters

| Boot | Behaviour |
| --- | --- |
| `34ee8a16…` | stayed up; no ANE probe run |
| `f1a4bae3…` | **stable for 8.7 minutes**, no PMU boot error, no USB resets, 4.5 GiB available |
| `f1a4bae3…` → probe | `ane_h17_read_probe.ko` loaded (genpd vote + engine reads); the shell tool was aborted and the machine reset |
| `3e32247f…` | boots with `PMU logged 1 boot error(s) and 0 panic(s)` again |

The stable boot had no ANE activity. The reset arrived during the ANE
probe. Earlier the same probe had also coincided with a reset on
`878ab177…` (probe at 380 s, boot ended at 385 s).

## What the resets are

`macsmc-reboot: PMU logged 1 boot error(s) and 0 panic(s)` — a
**power-management-unit-level boot error**, not a kernel panic. There is
no oops, SError, watchdog line, or shutdown sequence in the journal.

## What this implicates

The probe does two things: (1) raises and releases the ANE sub-domain
genpds through the pmgr driver, and (2) reads engine registers. Either
could be the trigger. The genpd vote is the more suspicious one: it
cycles `ane_sys` and `ane_sub0/1/2`, and a power-sequencing fault would
explain a PMU-recorded reset with no software trace.

This is a **correlation, not a proof**. The same probe has also completed
without an immediate reset, so the fault is intermittent.

## Decision

- No genpd votes, no engine reads, no engine writes.
- Do not re-run the read probe to "check" the ANE; each attempt risks
  another reset.
- Treat the ANE power-domain sequencing as unqualified until the PMU
  behaviour is understood.

## Next steps that are safe

1. Leave the ANE alone and confirm the host stays up for a long period
   with no ANE modules.
2. If sequencing is to be studied, do it from the macOS side or with the
   PMU/boot-error source decoded, not by trial on this machine.
3. Keep the passing measurements as data: stages 3/3b/3c/4 and the
   mailbox read were real, but they may be the events that tripped the
   PMU.

## Controlled comparison (boot `3e32247f…`)

Two probes, one variable each, same boot:

| Time | Probe | Engine access | Outcome |
| --- | --- | --- | --- |
| 303 s | `ane_h17_cpu_vote_probe.ko` (genpd vote + PMGR reads) | **none** | PASS; host continued, still up at 352 s |
| 357 s | `ane_h17_read_probe.ko` (vote + engine whitelist reads) | CPU_STATUS, RVBAR, SCRATCH0..7, A2I/I2A_CTRL | PASS at 357 s; host reset by ~13:50 (boot `b4c6a3e7…`, PMU boot error) |

So the genpd vote alone did **not** reset the host in this run, while the
engine read did — within roughly 2.5 minutes. That shifts the suspicion
from power-domain sequencing to **engine-window access**.

Caveat: engine reads have also completed without an immediate reset (the
stage-4 boot ran for hours), so the fault is intermittent. This is still
correlation, not a decoded cause.

## Implication

If ANE work continues, it must not include engine-window access on this
host. The read path is no longer worth the reset cost; the mailbox and
register map are already measured, and repeating them adds no evidence.
