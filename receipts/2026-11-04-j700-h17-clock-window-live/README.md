# J700/H17: the ANE clock window is live — the earlier "all zero" was a sampling artifact

Boot `3789060f-c92e-4bf2-911d-e631abf51f95`, warm reboot, run at uptime
~26 s. Commit `6029488`. 192 reads over `0x0–0xfc` and `0x1000–0x11fc`
of the window at physical `0x3082c8000`. No fault, no freeze.

## Correction to the previous receipt

`receipts/2026-11-04-j700-h17-clock-window-zero/` concluded the window read
"all zero". **That is falsified.** The 24 offsets sampled there
(`0x1000–0x101c`, `0x1100–0x113c`) are genuinely zero; the rest of the
window is not.

## What the window actually contains

```
0x0000  0xa55ac33c    0x0020  0x001040e0    0x0040  0x0010141f
0x0004  0xa55ac33c    0x0024  0x00000001    0x0044  0x00010240
0x0008  0xa55ac33c    0x0028  0x000a050c…   0x0048  0x00000409
0x000c  0x00a050c030  0x0038  0x00a050c030  0x0100+ (see below)
```

- `0xa55ac33c` repeats at `0x0`, `0x4`, `0x8`, `0x18`, `0x20`, `0x40`,
  `0x44`, `0x48`, `0x58`, `0x5c`, `0x60`, `0x6c`, `0x78`, `0xb4` — a
  fixed signature, i.e. a live, initialised register file, not a dead
  aperture.
- `0x4c/0x50/0x64/0x68` read `0xffffffff`.
- `0x80–0x88` read `0x98121641`, `0x90005cc2`, `0x50dc0480`.
- Group area: `0x1020 = 0x001040e0`, `0x1028 = 0x1`, `0x1038 = 0x14000000`,
  `0x1040 = 0x0010141f`, `0x1044 = 0x00010240`, `0x1048 = 0x00000409`,
  `0x1058 = 0x14000000`, `0x1068 = 0x0010141f`, `0x109c/0x10a8 = 0x400000`,
  `0x10f8 = 0x08fe0000`.

## Why this matters

The mapping `RegMap 0x71 -> resource 5` is now supported by live data, not
just a size coincidence: the window contains a structured, signed register
file. The earlier "all zeros ⇒ maybe wrong base" doubt is resolved in favour
of the base being right.

## The gate slots are still zero, and that is the point

`0x1100–0x11fc` — the 64 words where `_printClocks` would place clock-gate
registers at `0x1100 + stride*n` for the first 16-odd `n` — are **all zero**,
while the group header below them is populated.

Two readings, not yet separated:

1. The gates exist there and every one is disabled — which would literally
   be the clock-off state we suspect the ANE is in.
2. The ANE's `n` is larger than fits in `0x1100–0x11fc`, so its gate word
   lies beyond `0x1200`.

With stride up to `0x20`, covering `n` up to ~48 requires reading to about
`0x1400`. That is the next bounded read; it also stays inside the declared
`0x4000` window.

## Not established

- Which word/bit is the ANE gate.
- That writing any of them is safe.
- That enabling a clock would make the firmware speak.
