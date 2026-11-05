# Collector field: `die`

For the omarchy-mlx collector owner (`scripts/collect_deep.py`), effective with
omarchy-ane M1 (docs/ultra-die1.md §7). Legacy rows without this field
count as die 0 only when their check output does not bind both dies.

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
- An absent field means `0`, unless the check output binds both dies.
- **Consumer:** `tools/promotion_check.py` keys verdicts by `(chip, die)`
  (`soc(row)`, `die_of(row)`). A die-1 row never rides a die-0 pass: a chip
  is fully promoted only when each die has its own passing row.
- **Falsifying rows:** die-1 faults are scoped like die-0 ones — the die-1
  DART addresses (`2285800000`/`2285810000`/`2285820000`) and the die-1
  mailbox (`2285408000`) count as ANE lines in `tools/promotion_check.py`
  ANE_LINE.

The smoke result (`omarchy-ane-smoke` JSON) carries the same field, `die`,
next to `chip`, derived from the bound device the same way.

## Multiple bound dies

The checker reads every `bound to <engine base>.ane` match in the chosen block's check output.
If a block binds both dies, the checker does not judge it.
Line order and a scalar `die` field do not change this rule.
The shared smoke and fault fields cannot show which die passed or failed.
The rule covers runtime blocks and the `summary.ane_linux` fallback after `runtime:over_budget`.
Repeated matches for one die still count.

Collect smoke and fault evidence for each die before using a dual-die row to change its default state.
The checker does not credit shared evidence to both dies.
