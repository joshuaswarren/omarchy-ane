# ADT remap decode — 2026-09-24 ~13:55 UTC

## Raw entries (live proxy, struct phys/iova/remap/size)
- entry0: phys 0x10000848000 size 0xc4000 remap 0x10000000000 (TEXT).
- entry1: phys 0x10001400000 size 0x438000 remap 0x100000c4000 (DATA).

## Correction
0x100000dca10 = entry1 remap + 0x18a10: INSIDE entry1 (DATA offset).
No gap. The 2020 fault was a DATA access; our map never covered DATA
at its remap IOVA. The macOS 0xe8000 TEXT figure is a red herring.

## Panic note
Observer phys read at ~14:00 wedged the box (m1n1 EL2h data abort
at 0x55000010, frame saved): no phys reads while a HELD bind owns
the alias. Power-button cycle needed.
