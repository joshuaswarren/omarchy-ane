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
