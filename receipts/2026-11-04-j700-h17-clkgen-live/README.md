# J700/H17: the ANE clock-generator window (map 0x71) is live and programmed

Boot `6a3d0b8c-85a4-4e39-93c8-c3efb0d079e8`, warm reboot, run at uptime ~28 s.
Commit `7547fe2`. Module `ane_h17_clkgen_scan.ko` maps ANE resource 0
(`0x400000000`, 32 MiB, verified against the DT) and reads the whole `0x4000`
window at offset `0x1878000` (= physical `0x401878000`, RegMap 0x71 =
`SNE_ANECLKGEN_TOP`). Read-only. 4096 reads, no fault, no freeze.

## Result

```
CLKGEN_READ total 4096, nonzero 251
```

This confirms the correction in
`receipts/2026-11-04-j700-h17-clkgen-map71-correction/`: the clock generator
is at `0x401878000`, *not* `0x3082c8000` (which is map 0x6c, the CSR
aperture). The window is live and structured — a fully programmed register
file, not a dead aperture and not all-zeros.

## Layout (non-zero words)

Two bank headers, each with an end marker:

```
0x0000  bank-0 header:  0x1, 0x4, 0x1, 0xa700, 0x8180, 0x36f83e04
0x07f8  end marker:     0x7d, 0x1
0x0800  bank-1 header:  0x1, 0x4, 0x1, 0xa700, 0x8180, 0x30f83e04
0x0ff8  end marker:     0x7d, 0x1
```

Group area and clock tables:

```
0x1000  group valid:    0x80000000, 0x80000000, 0x80000000
0x11f0  mask-ish:       0x1f, 0xffffffff, 0xffffffe7, 0x1
0x1200  PLL/div table, 0x30 stride, two words per record, e.g.
        0x4201/0x3c01, 0x4c01/0x4201, 0x2003400/0x5801, 0x3004400/0x2003700,
        0x3005100/0x2004100, … increasing through 0x500b500/0x5008900
0x1804  divider table:  0x3c0fff, 0xc0eee, 0xc06db, 0xc0555, 0xc0249,
        0xc0111, 0xc0011, 0xc0001, then 0x1, 0x4, 0x5, 0x7
0x1ff8  end marker:     0x3fff, 0x1
0x2004  mirror of 0x1804 (0x3c0fff … 0x7)
0x27f8  0x3fff, 0x1     0x29f8  0x1, 0x1     0x2a00  0x1, 0xa2d000, 0xf0
0x2ff8  0xc, 0x1
```

Clock-rate config block, mirrored at 0x3000 and 0x3800:

```
0x3008  0xc800, 0xc801, 0x32030c74, 0x6, 0x61f81, 0x4201
0x3034  0x2900, 0x18202, 0x12400, 0x12409, 0xb801, 0x5c01, 0xb701,
        0x5b01, 0x17001, 0x12400, 0x212400, 0x1f2400
0x31f8  0x3f, 0x1
0x3200  0x30e0e, 0x3433434, 0x1aa3902, 0x273b5a52, 0x211181b, …
0x3400  0x4, 0x3000, 0x1f, 0x20400, 0xff00, 0x10000, 0x4, 0x400
```

(`0x3800` repeats `0x3000` with `0x3c01` instead of `0x4201` at the first
divider — a bank-0/bank-1 difference, same as the header nibble
`0x36…`/`0x30…`.)

## What this means

1. The address is right, and the clock generator is **alive and configured**:
   PLL/dividers are programmed, headers and end markers are present, and two
   banks are populated. The ANE clock infrastructure is not dead and not in a
   blank reset state.

2. The "gate region all zero" conclusion from the previous (wrong-window)
   receipt is now replaced by real clock data.

3. The per-device gate-enable bit is not identified by this read alone. The
   group area (`0x1000`) carries `0x80000000` valid flags; the `_printClocks`
   clock/gate formula (`+0x100`/`+0x2c`) produced all-zeros here, consistent
   with the subagent's finding that the `perf-regs`/`clocks` properties that
   feed `_printClocks` are absent from the J700 ADT. The *non-debug* enable
   path uses `initRegGroups` (group 0x20 → map 0x71, offset 0x1000), which is
   a different code path — see next step.

## Next step

Determine the exact clock-gate register/bit and the write sequence from the
non-debug enable path (`ApplePMGRFunctionClockGate::callFunction` →
`ApplePMGR::_enableDeviceAutoClockGating` →
`ApplePMGRNub::enableDeviceAutoClockGatingEjb`). That is offline kernelcache
analysis; see the follow-up receipt. No write is attempted until a source
names the bit and the ordering.

## Not established

- Whether the clock to the ANE CPU is currently gated on or off.
- Which word/bit enables it, or the write polarity.
- That writing any of these registers is safe (the "write outside Apple's
  order freezes the box" rule applies).
