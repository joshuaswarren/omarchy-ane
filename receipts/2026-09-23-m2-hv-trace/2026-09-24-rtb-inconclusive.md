# RTBuddy-mode attempt inconclusive — 2026-09-24 ~11:40 UTC

## Change
fw_start_rtb_mode=1 plumbed: param -> boot_start -> cfg -> S1
SCRATCH6 write (0 vs legacy 1). Built clean on-box, bound by itself.

## Outcome
Bind ran P1-P4, RUN, full poll, no READY, HELD, alive. But no S1
log line exists, so SCRATCH6=0 is unproven live — the run may have
replayed legacy S1. INCONCLUSIVE per the abort rule, not negative.

## Next
Verify the S1 write (observer readback of SCRATCH6 before RUN) before
any further bind. Then the hv trace.
