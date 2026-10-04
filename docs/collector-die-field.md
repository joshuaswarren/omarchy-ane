# Collector field: `die`

For the omarchy-mlx collector owner (`scripts/collect_deep.py`), effective with
omarchy-ane M1 (docs/ultra-die1.md §7). No existing field changes; rows without
this field keep meaning die 0 forever.

- **Field name:** `die`
- **Location:** the `omarchy_ane` block of the runtime section
  (`summary.ane_port_detail.runtime.omarchy_ane`), next to the smoke block.
- **Value:** integer, the ANE die index the row ran: `0` or `1` today.
- **Source:** the bound ANE platform device's device-tree node, first `reg`
  window (the engine base), floor-divided by `0x20_0000_0000`. omarchy-ane
  computes it the same way for the smoke result: die 1 is the pure
  +0x20_0000_0000 translation of die 0 on every window
  (receipts/2026-10-03-ultra-die1). On an x86 or non-Apple host there is no
  bound device: omit the field rather than sending 0.
- **Default:** absent field = `0`. Every legacy row counts as die 0.
- **Consumer:** `tools/promotion_check.py` keys verdicts by `(chip, die)`
  (`soc(row)`, `die_of(row)`). A die-1 row never rides a die-0 pass: a chip
  is fully promoted only when each die has its own passing row.
- **Falsifying rows:** die-1 faults are scoped like die-0 ones — the die-1
  DART addresses (`2285800000`/`2285810000`/`2285820000`) and the die-1
  mailbox (`2285408000`) count as ANE lines in `tools/promotion_check.py`
  ANE_LINE.

The smoke result (`omarchy-ane-smoke` JSON) carries the same field, `die`,
next to `chip`, derived from the bound device the same way.
