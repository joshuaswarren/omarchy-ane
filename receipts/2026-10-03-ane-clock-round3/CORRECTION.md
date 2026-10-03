# CORRECTION to the first issue of this receipt (2026-10-03T18:3xZ)

T6021Gate's peer review is accepted and this corrects commit 9815cdf's README.

**Wrong in the first issue:** "the word is 0x1e000 BELOW the W10 fatal-read window start" and "round-1's
exclusion of reg[41] was an arithmetic error".

**Right:** 0x285869200 - 0x285854000 = **+0x15200**, so the word (engine+0x1869200) is INSIDE the W10
fatal-read window [0x285854000, 0x285c04000). The same holds for the window base reg[41] = 0x285868000
(+0x14000) and for the context words 0x285869000 and 0x28586a000. Round 1 and round 2 had excluded this window
correctly; the arithmetic error was mine, in this round.

**Unaffected:** the address resolution (RegMap 113 → ADT pmgr reg[41]; PA 0x285868000 + 0x1200) and the value
format (0x80000000 | (prev << 4) | new, rungs 0..6 = 600..2100 MHz) stand as derived from the running build's
own `AppleT6021PMGR::initRegMaps`.

**Consequences:**

- No Linux read or write of reg[41] under the standing engine-window ban. The E2 drafted in the first issue
  (a Linux `ane_clk_twin_probe` read of 0x285869200) is **withdrawn**; it was stopped before any hardware
  access.
- The replacement capture is macOS-side: one ANERegDump range (0x285869200, 4 bytes), gated on the proven
  island set, snapshot idle / mid-loop / power-off. One macOS window, no kext build.
- A Linux gated write (E3) is blocked unless Main grants an explicit written W10-class exception; without it
  the lane closes as "operating point identified, Linux access banned by W10".

The lab report and summary JSON carry the same correction with the arithmetic shown.
