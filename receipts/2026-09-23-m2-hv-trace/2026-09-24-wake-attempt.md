# Wake attempt (no RUN) — 2026-09-24 ~10:55 UTC

## Send
A2I EP0: type 6 (SET_IOP_PWR_STATE) state 0x20, no RUN, no reset.

## Result
- Drained 0 words over 60 s; out114 frozen 0x000a0001 (EMPTY).
- No PWR_ACK, no HELLO. Firmware takes no input while parked.
- Lone word decode (upstream GENMASK_ULL(59,52)): type 0, unrouted,
  no reply. Not HELLO, not power-ack.
- Box alive, CPU never released in this attempt.

## Next: ane_cpu reset fallback (driver vehicle).
