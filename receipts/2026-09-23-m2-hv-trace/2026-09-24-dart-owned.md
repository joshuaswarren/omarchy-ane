# Kernel owns the ANE DARTs — 2026-09-24 ~14:20 UTC

## Evidence
- All three dart-ane instances bound to apple-dart at boot, locked: 0.
- Probe calls apple_dart_hw_reset on every unlocked DART (source).
- TTBR 0x1001253d is Linux's fresh table, not iBoot's.
- iBoot firmware mapping gone at ~0.07 s, before any ANE attempt.

## Fix
DCP-style lock: apple,dma-range on ANE DART nodes (or locked-DART
bypass) so probe preserves iBoot tables. DT + driver work, not MMIO.
