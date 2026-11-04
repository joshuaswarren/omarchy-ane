# J700/H17: map 0x71 (ANE clock generator) is at 0x401878000 — the earlier clock reads used the wrong window

Offline binary analysis plus reconciliation against measured hardware reads.
No hardware touched in this receipt.

## What happened

Two earlier receipts read the ANE "clock window" at physical `0x3082c8000`:

- `receipts/2026-11-04-j700-h17-clock-window-live/` — reported a live signature
  `0xa55ac33c` and concluded `RegMap 0x71 -> resource 5`.
- `receipts/2026-11-04-j700-h17-gate-region-zero/` — reported `0x1100–0x13ff`
  "all zero" and concluded the clock gates are disabled.

Both read the **wrong window**. `0x3082c8000` is `/arm-io/pmgr` `reg[30]`
(`0xf82c8000/0x4000`), which is **RegMap 0x6c — the ANE 16 KiB CSR
aperture** — not the clock generator. The `0xa55ac33c` signature is the CSR
block's signature, and "0x1100–0x13ff all zero" is a statement about the CSR
aperture, which says nothing about the clock gates.

## The correction

Offline binary analysis (`receipts/2026-11-04-j700-h17-regmap-71/README.md`,
agent A) establishes, by instruction stream and data:

- `ApplePMGR::initRegMap` takes a device-memory *resource index*, not an
  address (`w2`).
- `AppleT8140PMGR::initRegMaps` makes 44 calls with `w2 = 0..43`; `/arm-io/pmgr`
  is the only node with exactly 44 `reg` entries.
- map `0x71` is initialised once, from `reg[36] = 0x1f1878000 / 0x4000`.
- map `0x6c` is `reg[30] = 0xf82c8000 / 0x4000`.
- group `0x20 -> (map 0x71, offset 0x1000)`; `readANEClkGenReg` hard-codes
  map `0x71` (+bank).

Address translation uses `/arm-io` `ranges` `+0x210000000` (the same
translation that correctly maps `reg[30] 0xf82c8000 -> 0x3082c8000`, which
we measured live):

| | ADT `reg` value | physical |
|---|---|---|
| ANE engine block (`/arm-io/ane` reg[0]) | `0x1f0000000` | `0x400000000` |
| map `0x6c` (CSR aperture, pmgr reg[30]) | `0xf82c8000` | `0x3082c8000` |
| map `0x71` (clkgen, pmgr reg[36]) | `0x1f1878000` | `0x401878000` |

Note on agent A's report: its §4.3 says the engine block is "physical
`0x4_0100_0000`". That is an arithmetic slip — `0x1f0000000 + 0x210000000 =
0x400000000` (i.e. `0x4_0000_0000`), not `0x401000000`. The slip does not
affect the clkgen address (`0x1f1878000 + 0x210000000 = 0x401878000`, which
agent A states correctly) and the containment check was done in ADT space.
We record it here because the correct block base is independently pinned by
hardware: `ane_h17_af_scan.c` asserts, and at runtime confirms, that Linux
DT resource 0 of `/soc/ane@400000000` is `0x400000000 / 0x2000000`.

## Reconciliation with measured reads

- `0x400000000` (32 MiB, Linux ANE resource 0): measured live by `af_scan`
  (`AF_READ 0x0 = 0x16`, `0x338 = 0xffff`). The engine/AF register file sits
  at the block start.
- `0x3082c8000` (CSR aperture, map 0x6c): measured live by `clk_wide`
  (`0xa55ac33c`). This is a *different* aperture, not the clock generator.
- `0x401878000` (clkgen, map 0x71): **offset `0x1878000` inside the engine
  block**, between the wrapper (`0x1600000`) and the CE handshake
  (`0x18a4000`). Never read before.

The clock-generator register bank is therefore:

```
window base    0x401878000  (offset 0x1878000)
group +0x1000  0x401879000  (offset 0x1879000)
clock regs     0x401879100  (offset 0x1879100, +0x100)
power/gate     0x40187902c  (offset 0x187902c, +0x2c)
plus stride*n, stride in {0x10, 0x1c, 0x20}
```

All of it is inside resource 0, the block `af_scan` already maps and reads
safely. This makes the clock generator reachable with a read-only probe and
no new mapping.

## New probe

`ane/h17/ane_h17_clkgen_scan.c` maps ANE resource 0 (verifying
`0x400000000/0x2000000` against the DT) and reads the whole `0x4000` clkgen
window at `0x1878000`, logging every non-zero word durably. Read-only, one
shot per load, same power-domain + DART preconditions as `af_scan`.

## What this changes

- The "gate region all zero" conclusion is void: it read the CSR aperture.
- The clock-enable question is back to *unmeasured*. The actual clock
  registers at `0x401879100` have never been read on this machine.
- The ordinal `n` for devices 291/338 remains not-derivable offline; the
  probe sidesteps `n` by reading the whole window.

## Not established

- That `0x401878000` reads live (the next probe).
- That the clock is on or off.
- That writing a clock/gate bit is safe.
