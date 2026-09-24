# Reserved-placement alias attempt — 2026-09-24 ~10:46 UTC

## Change
fw_alias_reserved=1: map reserved SEG0 0x10000848000+0xc4000 at entry
0x10000000000 and SEG1 0x10001400000+0x438000 right after, instead of
the staged DMA copy. Same bytes (TEXT head 81000014... verified both
sides), preloaded placement.

## Outcome
- fwalias: reserved SEG0/SEGi at entry 0x10000000000 (preloaded placement).
- RUN 0x10, full poll A, sequence error -110, no READY, HELD, box alive.
- Negative: placement is not the blocker. Remaining untested: VENC/clock
  readback at RUN time.
