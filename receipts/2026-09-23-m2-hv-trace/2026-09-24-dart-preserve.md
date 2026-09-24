# DART-preservation test — 2026-09-24 ~13:50 UTC

## Pre-state (power-gated observer, no reset touched)
- sid0 TCR=0x9, TTBR0 valid, err residue, ENABLE=0xffff.
- ctl 0, status 0x2a, RVBAR latched.

## Attempt
- No B2, no sid re-attach. Outbox bit, ctl 0 -> 0x10.
- 60 s poll: ctl/status frozen, err 0 (no new fault), no HELLO.
- Quiesced after (ctl 0).

## Verdict
Fetch path eliminated twice (reserved alias + preserved tables).
Remaining: image content at entry, or pre-RUN clock/reset piece.
