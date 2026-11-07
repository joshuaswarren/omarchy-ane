# J700/H17: gate region 0x1100–0x13ff is entirely zero

Boot `080bf754-a27c-4ef3-abfd-7607cc30494a`, warm reboot. Ran
manually (the launcher refused because another analysis directory was
untracked): `/var/log/omarchy-ane/manual-clkwide2-080bf754…/probe.log`,
module `ane_h17_clk_wide.ko`, offsets extended to 320 covering
`0x0–0xfc`, `0x1000–0x10fc`, `0x1100–0x11fc`, `0x1200–0x13fc`.

## Result

```
total reads 320, nonzero 65
0x1100–0x11ff : all zero
0x1200–0x13ff : all zero
```

Every non-zero word lies below `0x1100`:

```
0x00  0xa55ac33c   0x20  0xa55ac33c   0x40  0xa55ac33c
0x0c  0xa050c030   0x24  0x00000001   0x4c  0xffffffff
0x10  0x0c5c000b   0x28  0xa050c030   0x80  0x98121641
0x1020 0x001040e0  0x1038 0x14000000  0x1040 0x0010141f
```

The signature `0xa55ac33c` still repeats below `0x100`, confirming the
window is live; the group area at `0x1000+` is populated; the region where
`_printClocks` would place clock-gate words (`0x1100 + stride*n`) is
uniformly zero across 192 words — enough for `n` up to ~48 at stride
`0x10`, ~24 at `0x1c`, ~16 at `0x20`.

## What this means

Taking the formula at face value, **every clock-gate word in this block
reads zero** — the gates are all disabled. That is exactly the state we
suspect the ANE is in, and it is consistent with a firmware that accepts
`CPU_CONTROL := 0x10` and never executes.

But it also means the read cannot tell us *which* word belongs to the ANE.
A field of uniformly-zero registers carries no identity. Two readings
remain, and this run separates neither:

1. The ANE's gate word is in `0x1100–0x13ff` and reads zero because it is
   gated off.
2. The ANE's gate is elsewhere (different group, different stride), and
   this range belongs to something else entirely.

## Why I am not writing a bit

To enable a clock we must set a specific bit in a specific word. Every
candidate word currently reads zero, so there is no way to identify the
ANE's from the read alone. Writing each candidate in turn would mean
several boots of blind writes into a live PMGR block — the exact class of
access that has frozen this machine. I will not do that without a source
that names the bit.

## What would actually settle it

- The `_printClocks` device ordinal `n` for devices 291/338 — still
  unresolved offline.
- A macOS-side read of the same window while the ANE is running, which
  would show which words are non-zero when the clock is on. That needs a
  kernel-debugging session; the registry capture cannot reach it.

Until one of those exists, the clock-enable step has no evidence base and
should stay unexecuted.
