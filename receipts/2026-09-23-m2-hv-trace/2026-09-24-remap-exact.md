# Remap-exact test — 2026-09-24 ~14:05 UTC

## Map (both ADT entries in place, reserved phys)
- IOVA 0x10000000000 -> PA 0x10000848000, 0xc4000 (TEXT).
- IOVA 0x100000c4000 -> PA 0x10001400000, 0x438000 (DATA).
- SCRATCH6=0 proven in dmesg before RUN (RTBuddy select).

## Release (M1-style)
- Outbox bit, ctl 0 -> 0x10. 60 s poll.

## Outcome
- SCRATCH7 zero throughout, drain 0 words, no HELLO, status 0x28.
- Negative: correct map + RTBuddy + release still parks.
- Next: hv trace for macOS pre-RUN writes.
