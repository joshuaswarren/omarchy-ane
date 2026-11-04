# J700/H17: offline derivation of the ANE clock window

No hardware was touched. Everything below is from the retained ADT, the
retained Darwin 25.6 kernelcache, and the macOS 27 registry capture.

## 1. The ANE node has six resources; Linux maps one

ADT `/arm-io/ane` `reg` is 96 bytes = 6 entries. The macOS capture shows
the same six, translated by a constant `+0x210000000`:

| # | ADT address | macOS / global | size | |
|---|---|---|---|---|
| 0 | `0x1f0000000` | `0x400000000` | `0x2000000` | engine (the only one Linux maps) |
| 1 | `0x0f0700000` | `0x300700000` | `0x18000` | PMGR |
| 2 | `0x0f0724000` | `0x300724000` | `0x4000` | PWGATE |
| 3 | `0x0f03c0000` | `0x3003c0000` | `0x30000` | PTD |
| 4 | `0x01000000` | `0x211000000` | `0xff4000` | alternate base |
| 5 | `0x0f82c8000` | `0x3082c8000` | `0x4000` | **unnamed, never examined** |

All six satisfy the same `+0x210000000` offset, so resource 5 is a genuine
ANE window rather than a parsing artifact.

## 2. Resource 5 is the ANE clock generator

`ApplePMGR::initRegGroups` (`0xfffffe00098372cc`) lays out the reg-group
table as `(group index, map id, offset within map)`. One entry is:

```
0xfffffe000983735c  mov w1, #0x20      ; group index 32
0xfffffe0009837360  mov w2, #0x71      ; RegMap 0x71
0xfffffe0009837364  mov w3, #0x1000    ; offset 0x1000
```

RegMap `0x71` is `SNE_ANECLKGEN_TOP` (string at file `0x574671`). Group
`0x20` is therefore `0x1000` inside a `0x4000`-sized window — which is
exactly resource 5's size and nothing else in the set fits it.

`_printClocks` (`0xfffffe0009466138`) then computes each gate register:

```
0xfffffe000946613c  ldr  x9, [x19, #0x60f8]   ; reg-group table
0xfffffe0009466140  add  x8, x9, w8, lsl #4   ; group = table + idx*16
0xfffffe0009466154  ldr  w9, [x8, #4]         ; group base offset
0xfffffe0009466194  add  w9, w9, #0x100       ; clock registers
0xfffffe0009466164  add  w9, w9, #0x2c        ; (power registers)
0xfffffe00094661b0  madd w26, w10, w8, w9     ; = stride*n + base
```

so for clocks:

```
register = 0x3082c8000 + 0x1000 + 0x100 + stride*n
         = 0x3082c9100 + stride*n          stride in {0x10, 0x1c, 0x20}
```

**Proven:** map 0x71 = `SNE_ANECLKGEN_TOP`, group offset `0x1000`, the
`+0x100` clock / `+0x2c` power split, the `madd` formula, and that
resource 5 is the only ANE window whose size matches group `0x20`.

**Inference:** that map 0x71's base is resource 5. It is strongly implied
by the size match and by the ANE node owning that window, but the
`initRegMap(0x71, …)` call site that would prove it was not found.

## 3. What is still missing: `n`

The gate register is `0x3082c9100 + stride*n`, and `n` is the per-device
ordinal taken from a runtime device struct (`ldrb w8, [x27, #1]`). It is
not derivable from the ADT `devices` blob:

- For CPU rails, word `+0x0c` is a small ordinal (`ECPU_0`=5 … `PCPU_1`=0xa),
  which looks like an index.
- `ANE-SYS-V` (291) has `0x00000000`, and `ANE-SYS-V-PMP` (338) has
  `0x80000000` — not an ordinal, and not consistent with the CPU pattern.

One consistent reading is that `ANE-SYS-V` is bit 0 and `ANE-SYS-V-PMP` is
bit 31 of a **shared** gate register at `0x3082c9100`, which would make
the pair one word. That is speculation from two data points; it is
recorded as a hypothesis, not a conclusion.

## 4. Incidental confirmation

`ANE_CPU` is PMGR device `0x81` with ps-word `0xc000` — the same
`0xc000` this repository has been voting through genpd since stage 3c.
That sub-domain is now named rather than assumed.

## 5. What this changes for the prepared read

`ane_h17_clk_scan.ko` previously sampled `0x1000…0x102c`. It now samples
the group header (`0x1000…0x101c`) **and the first 16 clock-gate slots**
(`0x1100…0x113c`), because that is where the formula says gates live.

Still read-only, still bounded, still one cold boot. It is not known to be
safe: the window has never been read from Linux.

## 6. Why this matters

If the gate words are readable, one of them should show the ANE gate bits
and we can attempt the vote. If the read faults, the window is gated behind
the very clock we are trying to enable, and the macOS route becomes the
only way left.

## 7. Corroboration: the window is PMGR-declared

`/arm-io/pmgr` `reg` is 44 windows. Entry 30 is

```
30  pa=0xf82c8000  len=0x4000
```

which is exactly the ANE's resource 5 (`0xf82c8000` in ADT space,
`0x3082c8000` global). Its neighbours in the same list are
`0xf82b8000/0x10000` (entry 29) and `0xf82d0000/0x8000` (entry 31) — a
band of small PMGR sub-windows.

So resource 5 is not a stray ANE-private aperture; PMGR declares it. That
raises confidence that map `0x71` binds to it, though it still does not
prove the binding, because `initRegMap` for map `0x71` was not located.

Other ANE windows appear in the same list: entry 21 `0xf0700000/0x18000`
(PMGR slice), 22 `0xf0724000/0x4000` (PWGATE), 34 `0xf03c0000/0x30000`
(PTD), 41 `0x01000000/0xff4000` (alternate base). The engine aperture
`0x1f0000000` is absent, as expected — it is not a PMGR window.

## 8. What remains unresolvable offline

The per-device ordinal `n` in `0x3082c9100 + stride*n`. It is read from a
runtime device struct built from the `devices` blob, and the blob's fields
do not expose it in a form that separates gates from power-state devices.
Two candidate readings are recorded (bit 0 / bit 31 of one shared gate
word, versus a per-device ordinal), and neither can be settled without
either reading the window or a kernel-debugging session on macOS.
