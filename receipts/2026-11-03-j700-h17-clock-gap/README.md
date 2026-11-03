# J700/H17: the ANE clock gap, now precisely identified

Offline finding from the full Apple ADT (`/dev/mtd1ro`, 0x68000 bytes,
read with `linux-aurora/tools/aurora-sep/aurora-adt-extract.py`). No
hardware access.

## What the ADT says about the ANE node

```
/arm-io/ane
  ane-type        [4]  = <0x00000200>          # 0x200 = J700/H17
  compatible      [17] = "ane,t8132exclave"
  clock-ids       [12] = <0x147 0x146 0x145>   # 327, 326, 325
  clock-gates     [8]  = <0x123 0x152>         # 291, 338
  power-gates     [8]  = <0x123 0x152>         # 291, 338
  exclave-reg     [16] = <0x41c00000 0x4 0x88000 0x0>
  segment-ranges  [64] = ...                    # __TEXT/__DATA placement
  pre-loaded      [4]  = <0x1>
```

The gate IDs reconcile with the recorded pmgr device table:
- `0x123` = 291 = `ANE-SYS-V`
- `0x152` = 338 = `ANE-SYS-V-PMP`

Both are `no_ps` in the ADT, i.e. they are clock/power **gates**, not
power-state words.

## The gap

Linux models **none** of these:

- `arch/arm64/boot/dts/apple/t8140-pmgr.dtsi` has **zero**
  `clock-controller@` nodes.
- This tree has **no PMGR clock driver**: `drivers/clk/` contains only
  `clk-apple-nco.c`, and the `clock-controller@` nodes that do exist in
  other SoCs are NCO oscillators (`apple,nco`), not gates.
- Neither `ane/h16/` nor `ane/t6021/` calls any `clk_*` API; both rely
  on genpd alone.

So on Linux the ANE's three clock IDs and two clock gates are never
voted, while the Apple driver votes them through
`AppleARMIODevice::enableDeviceClock` over the per-node `clock-ids` table
parsed by `initDeviceWithProvider`.

## Why this matters for the resets

The engine-window reads intermittently coincide with PMU-level resets
(`PMU logged 1 boot error(s)`, no panic). A gated clock on an accessed
block is a plausible cause of exactly that: the access faults, and the
PMU records a boot error rather than a kernel panic. It also explains the
intermittency — whether it faults depends on the clock state at that
moment.

This is a hypothesis, not a decoded cause, but it is the only
source-grounded one with a concrete fix.

## What is still missing

The **ID → register offset** map for clock gates 291/338 and clock IDs
327/326/325. It is not in the ADT: the IDs appear only in each node's
`clock-gates`/`clock-ids` property, and `/arm-io`, `/arm-io/pmgr`,
`soc-tuner` and `ppm` carry no such table (`hw-dpe-reg` is a DPE write
list; `soc-tuner/device-set-*` are tuner groupings).

The map lives in `AppleH17PPlatformIO` (com.apple.driver.AppleARMPlatform)
as the `function-clock_gate` table. Getting it needs one of:

1. reverse the platform kext from the retained kernelcache, or
2. an upstream Asahi T8140 PMGR clock table, if one is ever published, or
3. a read-only PMGR scan for gate-shaped registers, correlated with the
   known IDs — weakest option, and still engine-adjacent only in the
   PMGR window (which is always on).

## Consequence for the ladder

Until the clocks are modelled, an engine read is being taken with
possibly-gated clocks. Closing this gap is a prerequisite to any further
engine work, not an optimisation.

## Follow-up: the gate is a firmware function, not a table

The kernelcache is an MH_FILESET. `com.apple.driver.AppleT8140` (fileoff
`0x6175b0`) owns the arm-io provider class (`AppleH17PPlatformIO`, text at
`0xfffffe0009790950`). Its `start` builds the gate functions:

```
0xfffffe0009790fd8  adrp x1, 0xfffffe000761b000
0xfffffe0009790fdc  add  x1, x1, #0xea7        ; "function-clock_gate"
0xfffffe0009790fe0  bl   0xfffffe00089631b0    ; AppleARMFunction::withProvider
0xfffffe0009790fe8  str  x0, [x3, #240]!

0xfffffe0009791030  adrp x1, 0xfffffe000761b000
0xfffffe0009791034  add  x1, x1, #0xf2d        ; "function-power_gate"
0xfffffe0009791038  bl   0xfffffe00089631b0
0xfffffe0009791040  str  x0, [x3, #248]!
```

So the provider does not hold an ID-to-offset table that can be read out.
It resolves a named `AppleARMFunction` and calls it. The actual PMGR
register selection lives behind that function (provider/firmware side).

Searches that came up empty, for the record:
- no literal `0x123`/`0x152` (gates) or `0x145`/`0x146`/`0x147` (clock
  ids) anywhere in `AppleARMPlatform`'s `__TEXT,__const`,
  `__DATA_CONST,__const` or `__DATA,__data`;
- no chained pointer to the `clock-ids`/`clock-gate` strings from either
  kext's text.

## Consequence

"Vote the ANE clocks first" is not a small fix: there is no table to
copy. Closing the gap requires either implementing the provider's
function semantics (PMGR gate register layout for T8140, which is not
published in any tree here) or calling the same function through the
provider, which Linux does not model.

This does not kill the clock hypothesis; it means the hypothesis cannot
be tested by simply adding three clock votes.

## Follow-up 2: the function is declared, not implemented, in the kexts

Re-reading the ADT's `/arm-io` node with the parser's allowlist lifted
shows the function properties directly:

```
/arm-io
  function-clock_gate = <0x00000069 0x636c6b47>   # 0x69, "clkG"
  function-power_gate = <0x00000069 0x70777247>   # 0x69, "pwrG"
```

So `AppleARMFunction::withProvider(arm-io, "function-clock_gate")` reads a
property of the form `<type=0x69, tag="clkG">`.

Disassembly of `AppleARMFunction::withProvider` (`0xfffffe00089631b0`)
shows it: builds an `OSString` from the name, then resolves through the
provider's vtable slot `0x2c8` (a property lookup), casts the result, and
stores it. No register computation appears there.

Searches for a dispatch table came up empty:
- `0x636c6b47` ("clkG"), `0x70777247` ("pwrG") and `0x69` are absent from
  `AppleT8140`'s `__TEXT,__const`, `__DATA_CONST,__const` and
  `__DATA,__data`;
- "AppleARMFunction" appears only as a class/string, not as a table.

## Conclusion

The clock/power gate is a **provider-side function** identified by
`<0x69, "clkG">`, resolved through a property lookup and invoked through
the provider's command gate. The kexts declare it; they do not contain an
ID-to-register table we can copy. The register selection is behind the
provider/firmware boundary.

So the clock hypothesis cannot be closed by static analysis of the
retained kernelcache. It would need either a published T8140 PMGR clock
layout or an implementation of the provider's function semantics, neither
of which is in the sources held here.
