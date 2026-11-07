# J700 / T8140 (H17): the ANE clock + power enable path, register level

Offline binary analysis only. No hardware, no module load, no MMIO, no writes
outside this directory. Every claim carries the instruction address (and the
raw bytes where it matters) so it can be re-checked.

Kernelcache: `/var/tmp/neo/kernelcache.bin`, 114 688 000 B, SHA-256
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
`VA = 0xfffffe0007004000 + file_offset` (re-verified on every instruction quoted:
`0xfffffe0009453fa8` → `0xe9 9f 9f 52` at file offset `0x2453fa8`).

---

## 0. Bottom line (read this if you read nothing else)

1. **The prior round's blocking unknown is resolved.** The six dispatches in
   `ApplePMGRFunctionClockGate::callFunction` *do* resolve. The missing piece was
   the Itanium ABI vtable base: the object's `vptr` is `__ZTV + 0x10`, so a
   instruction sequence `ldr x16,[x0]; mov x17,#SLOT; add x16,x16,x17; ldr x8,[x16]`
   reads `*(__ZTV + 0x10 + SLOT)`, not `*(__ZTV + SLOT)`. §2.1.

2. **The clock/power-gate field is a read-modify-write of bits `[19:16]` with a
   4-bit value, over a literal mask `0xfff0fcff`.** It is not an OR into a single
   bit and not an AND with a clear-mask: it clears all four of bits 19..16 and
   writes the level with `bfi`. The ack is bits `[7:4]`. §4.1.

   ```
   level = (arg == 0) ? 0xF : 0x0        // for enableDeviceAutoClockGating, arg = enable
   tmp = readReg32(map, base + 8*index, die)
   if (((tmp >> 16) & 0xF) == level) goto done        // already there, no write
   writeReg32(map, base + 8*index, (tmp & 0xFFF0FCFF) | (level << 16), die)
   done:
   if (level == 0xF) waitReg32(map, base + 8*index, mask=0x000000F0, value=0x000000F0,
                               timeout=0x2EE00 ticks /* 192 512 ns */, die)
   ```

   The poll therefore runs **only on the `level = 0xF` = "pin the device awake"
   transition** — i.e. when `enableDeviceAutoClockGating` is called with
   `enable = 0`. The `level = 0` ("allow auto-gating") direction is write-only.
   Branch: `0x9475e58: 15 01 00 b4  cbz x21, #0xfffffe0009475e78` (the wait) with
   the fall-through returning.

3. **The register is NOT in map `0x71` / `0x401878000`.** The group table used
   here (`this+0x60c0`) is a verbatim copy of the DT property **`ps-groups`**,
   which exists on J700 with exactly **two** entries:

   | group | resource idx → map | window (ADT) | window (physical) | base field | valid-mask |
   |---|---|---|---|---|---|
   | 0 | 0 → map `0x00` | `0xf0700000` | **`0x3_0070_0000`** | `0x0` | `0x0` |
   | 1 | 1 → map `0x01` | `0xf8280000` | `0x3_0828_0000` | `0x0` | `0x0` |

   So every ANE gate/PS write in this path lands in **PMGR window 0 at physical
   `0x3_0070_0000`**, at `base + 8*index`, `base = 0`. §5.

4. **The per-device index comes from the DT `devices` blob**, not from `reg[36]`
   and not from `clocks`/`perf-regs`. Device record `+0x10` is the byte offset
   (index = `(rec[+0x10] >> 3) & 0x1FFFFF`), record `+0x13` is the PS reg group.
   Decoded for the four ANE devices in §5.3:

   | rec | deviceID | name | group | PS offset |
   |---|---|---|---|---|
   | 59 | `0x3c` | `ANE_SYS` | 0 | `0x290` |
   | 128 | `0x81` | `ANE_CPU` | 0 | `0xc000` |
   | 267 | `0x123` | `ANE-SYS-V` (gate id 291) | 0 | `0x0` |
   | 290 | `0x152` | `ANE-SYS-V-PMP` (gate id 338) | 0 | `0x0` |

5. **`readANEClkGenReg` / `writeANEClkGenReg` / `waitANEClkGenReg` — the only
   accessors that touch map `0x71` — are dead code in this kernelcache.**
   0 `bl` callers and 0 pointer references image-wide. **Apple never writes the
   ANE clock generator through them.** §6.

6. **`enableDeviceClock` does not write a clock register at all on this SoC.**
   It reaches `ApplePMGR::_enableDeviceGatedEmmmm`, which routes the state change
   to the **PMP** through `_sendPMPCommand` (a command mailbox), not through
   `writeReg32`. §3.3.

7. **The live values `0x11f4 = 0xffffffff` / `0x11f8 = 0xffffffe7` are not
   gate-enable registers under any of the three decoded layouts.** Under the
   `setPSAutoMinLevel` layout the group-0x20 window `0x1000..0x11ff` holds 64
   8-byte PS slots and the last two (`0x11f0`, `0x11f8`) are the only programmed
   ones — but the fields do not decode as request/ack. Under the `_printClocks`
   layout `0x11f0..0x11ff` is exactly one 16-byte clock record (`n = 15`, the
   last of 16). Both readings are arithmetically exact; **nothing offline
   distinguishes them.** §7.2.

---

## 1. Evidence base

| item | value |
|---|---|
| kernelcache | `/var/tmp/neo/kernelcache.bin` |
| VA ↔ file | `VA = 0xfffffe0007004000 + file_off` |
| ADT | `/var/tmp/opencode/adt.bin`, 425 984 B, parsed 412 112 B, 325 nodes / 4 985 props |
| ADT parser | `/home/admin/linux-aurora/tools/aurora-sep/aurora-adt-extract.py` (`parse_adt`, `walk`) |
| disassembler | capstone (`kdis.py`, reused from `2026-11-04-j700-h17-regmap-71`) |
| symbols | `syms_com_apple_driver_Apple{PMGR,T8140PMGR,T8140,ARMPlatform,H16ANEInterface,T8140ANEHAL}.json` |

Kexts that matter here:

| kext | text |
|---|---|
| `com.apple.driver.ApplePMGR` | `0xfffffe000943ff90` … |
| `com.apple.driver.AppleT8140PMGR` | `0xfffffe0009836b80` … |
| `com.apple.driver.AppleARMPlatform` (`AppleARMIO`, `AppleARMIODevice`) | `0xfffffe000895d900` … |
| `com.apple.driver.AppleT8140` (`AppleH17PPlatformIO`) | `0xfffffe0009790950` … |
| `com.apple.driver.AppleH16ANEInterface` (`ANEHWDevice`) | `0xfffffe0008eeb820` … |

New scripts added in `scripts/` (`vtslot.py`, `slots.py`, `findslot.py`);
`kdis.py` / `blscan.py` / `strs.py` are reused from `2026-11-04-j700-h17-regmap-71`.

---

## 2. Correction to two earlier reports, with proof

### 2.1 The vtable base — `__ZTV + 0x10`

Apple kexts are Itanium-ABI C++. `__ZTV<Class>` points at
`[offset_to_top, typeinfo, entry0, entry1, …]` and the object's `vptr` points at
`__ZTV + 0x10`. Every virtual dispatch in this driver follows the form

```
0xfffffe0009453f34: 10 03 40 f9   ldr   x16, [x24]        ; x16 = vptr
0xfffffe0009453f3c: 11 0a 82 d2   mov   x17, #0x1050     ; code slot
0xfffffe0009453f40: 10 02 11 8b   add   x16, x16, x17
0xfffffe0009453f44: 08 02 40 f9   ldr   x8, [x16]
```

so the target is `*(vptr + slot) = *(__ZTV + 0x10 + slot)`.

Two independent proofs that the `+0x10` is real and not a coincidence:

* `ApplePMGR::initRegMap` reads the die count through **code slot `0xb30`**
  (`0x94487e0: mov x17,#0x700` … `0x9448838`; and the identical idiom at
  `0x94803a0` in `AppleBringUpPMGR::initRegMaps`). With `+0x10`,
  `__ZTV9ApplePMGR``+0xb40` = `0xfffffe000947f4fc` = `ApplePMGR::getDieCount()` —
  a function that takes no arguments and returns an int, which is exactly how it
  is used. Without `+0x10`, `+0xb30` = `ApplePMGR::enableBatteryReporting(bool)`,
  which cannot be called with no argument and cannot return a die count.
* `__ZTV14AppleT8140PMGR` `0xfffffe000814e778` + `0x10` + `0x1058` =
  `AppleT8140PMGR::writeReg32(RegMap,uint,uint,uint)` — i.e. the subclass really
  does override the register write, which is only visible under the `+0x10`
  convention (`__ZTV`+`0x1058` = `ApplePMGR::writeReg32`).

Reproduce:

```
python3 scripts/vtslot.py 0xfffffe0008060c30 0xb30 0x948 0x1050 0x1058 0x1060
python3 scripts/vtslot.py 0xfffffe000814e778 0x1058 0x1068    # t8140 override
```

### 2.2 `ApplePMGRFunctionClockGate::callFunction` is `0x94864f4`, confirmed

`__ZN26ApplePMGRFunctionClockGate12callFunctionEPvS0_S0_` = `0xfffffe00094864f4`
(`0x94864f4: 7f 23 03 d5` = `pacibsp`). The address in my original brief
(`0x948bdf8`) is `__ZN36ApplePMGRFunctionEnableAutoClockGate12callFunctionEPvS0_S0_`,
a *different* class. Both are analysed below; they do different things.

### 2.3 The six slots of `ClockGate::callFunction` now resolve

| code slot | `__ZTV9ApplePMGR` + 0x10 + slot | target | role |
|---|---|---|---|
| `0xb68` | `+0xb78` | `ApplePMGR::isDebugAllowed()` | `cbz w0` → skip |
| `0xb70` | `+0xb80` | `ApplePMGR::canPublish()` | `cbz w0` → skip |
| `0xb78` | `+0xb88` | `ApplePMGR::getAnalyticsManager(type, ps_state, die)` | returns object, `cbz x0` |
| `0xb30` | `+0xb40` | `ApplePMGR::getDieCount()` | `cmp w22, w0 / b.hs` → panic |
| `0x948` | `+0x958` | **`ApplePMGR::_enableDeviceEmmmm`** | the actual work |

The three `0xb68/0xb70/0xb78` dispatches are analytics plumbing only: they are
gated on `isDebugAllowed()` and `canPublish()`, and if either is false the code
branches straight to `0x94865ec`/`0x94865f0` (which zero `x23`/`x24`, the
analytics handles) and then does the real call. They are **not** part of the
register write.

---

## 3. The three functions, one at a time

### 3.1 `ApplePMGRFunctionClockGate::callFunction` — `0xfffffe00094864f4`

```
948651c: 96 7e 1c 53   lsr   w22, w20, #0x1c      ; w22 = x1 >> 28  = die
...
9486608: mov x17,#0xb30 ; getDieCount()
948661c: df 02 00 6b   cmp   w22, w0
9486620: e2 05 00 54   b.hs  -> panic (0x94866dc)
9486624: 7f 12 00 71   cmp   w19, #4               ; x2 (enable word) must be < 4
9486628: c2 05 00 54   b.hs  -> panic (0x94866e0)
...
9486664: 63 06 41 d3   ubfx  x3, x19, #1, #1       ; x3 = (x2 >> 1) & 1
9486668: e4 03 16 2a   mov   w4, w22               ; w4 = die
948667c: mov x17,#0x948                          ; ApplePMGR::_enableDeviceEmmmm
9486688: 81 6e 40 92   and   x1, x20, #0xfffffff ; x1 = id & 0x0fffffff
948668c: 62 02 40 92   and   x2, x19, #1          ; x2 = enable & 1
9486694: blraa x8, x16
```

**No register access whatsoever.** It is a pure argument-unpacking shim:
`(word) → _enableDeviceEmmmm(id = word & 0x0fffffff, enable = arg2 & 1,
arg3 = (arg2 >> 1) & 1, die = word >> 28)`.

The incoming `word` is the DT gate id, and it reaches this function untouched:

```
AppleARMIODevice::enableDeviceClock(enable=w1, index=w2)      0xfffffe000896a304
  896a3b4: ldr  w1, [x10]        ; w1 = clock_gates[index]  <- /arm-io/ane clock-gates
  896a3dc: mov  x2, x19          ; x2 = enable
  896a3c8: mov  x17,#0x8a8 ; braa ; -> provider->enableDeviceClock(id, enable)
AppleH17PPlatformIO::enableDeviceClock(id=w1, enable=w2)      0xfffffe0009791310
  979138c: e1 03 14 2a  mov  w1, w20   ; w1 = arg1 = id  (unchanged)
  9791390: e2 03 13 2a  mov  w2, w19   ; w2 = arg2 = enable
  97913b8: 03 00 80 d2  mov  x3, #0
  97913a4: mov  x17,#0x140 ; braa ; -> [function-clock_gate object]
ApplePMGRFunctionClockGate::callFunction(word=w1, a2=w2, a3=w3)
```

`/arm-io/ane` `clock-gates = <0x123 0x152>` = 291, 338 — both `< 0x10000000`, so
**die = 0** for both, and `id & 0x0fffffff` is the id itself.

### 3.2 `ApplePMGRNub::enableDeviceAutoClockGatingEjb` — `0xfffffe0009491510`

```
9491518: 00 44 40 f9   ldr  x0, [x0, #0x88]        ; x0 = provider (ApplePMGR)
9491524: 29 7c 7e d3   ubfiz x9, x1, #2, #0x20     ; index*4
949152c: 0a c1 29 8b   add  x10, x8, w9, sxtw
949153c: 41 01 40 b9   ldr  w1, [x10]              ; w1 = nub->[a0][index]  = device id
9491540: e2 03 02 2a   mov  w2, w2                 ; zero-extend enable
9491554: mov x17,#0xba0                           ; ApplePMGR::_enableDeviceAutoClockGatingGatedEmmm
9491568: 03 00 80 d2   mov  x3, #0                ; die = 0
9491574: braa x16, x17
```

`[this+0xa0]` is the nub's own gate-id array, filled at `start()` from the ADT
properties of the node the nub was published for. Same shape as
`AppleARMIODevice::enableDeviceClock` (§3.5: its `[this+0xa8]` from ADT
`clock-gates`), one level up. *Which* property fills the nub's `+0xa0` is not
verified here — only that the array is `u32`-strided, indexed by `w1`, and its
elements are passed on as 16-bit device ids (`0x9475cd0: and w1, w24, #0xffff`).

The sibling `ApplePMGRNub::enableDevicePowerEjb` `0x94914a8` is byte-identical
except code slot **`0xb98`** → `ApplePMGR::_enableDeviceAutoPowerGatingGatedEmm`.

### 3.3 `ApplePMGR::_enableDeviceAutoClockGatingEmmm` → `…GatedEmmm`

`__ZN9ApplePMGR28_enableDeviceAutoClockGatingEmmm` `0xfffffe0009475b44` is the
un-gated wrapper: it takes the PMGR gate lock object `[this+0x1b80]` and calls its
slot `+0x1e8` with the *gated* body as the function pointer:

```
9475bb8: adrp x16,#0x9475000 ; add x16,x16,#0xc6c
9475bc0: mov  x17, #0xa83d ; pacia x16, x17        ; a symbol/lock name
9475bb4: cbnz x16, 0x9475c0c                        ; fast path when already gated
9475c0c: ldr  x16, [x22]                            ; x22 = [this+0x1b80]
9475c1c: mov  x17, #0x1e8
9475c34: mov  x2, x21   ; = w1 (device id)
9475c38: mov  x3, x20   ; = w2 (enable)
9475c3c: mov  x4, x19   ; = w3 (die)
9475c40: mov  x5, #0
9475c68: braa x16, x17
```

#### 3.3.1 `ApplePMGR::_enableDeviceAutoClockGatingGatedEmmm` `0xfffffe0009475ca0`

```
9475cd0: 01 3f 00 12   and  w1, w24, #0xffff     ; id = deviceId & 0xFFFF
9475cd4: bl   0x9454b6c                          ; _deviceIDToDeviceData(id) -> x25
9475cdc: 01 34 40 79   ldrh w1, [x0, #0x1a]      ; rec->+0x1a (u16)
9475ce0: cbnz w1, +8
9475ce4: 21 0f 40 39   ldrb w1, [x25, #3]        ;   else rec->+0x03 (u8)
9475cec: bl   0x9453b4c                          ; _getDevicePsRegGroup(id) -> x22
9475d00: bl   0x9453ba0                          ; _getDevicePsRegIndex(id) -> x23
...
9475e38: bf 02 00 f1   cmp  x21, #0               ; x21 = enable
9475e3c: e8 01 80 52   mov  w8, #0xf
9475e40: 03 01 9f 1a   csel w3, w8, wzr, eq       ; level = enable ? 0 : 0xF
9475e44: mov  x0, x20                            ; this
9475e48: mov  x1, x22                            ; group
9475e4c: mov  x2, x23                            ; index
9475e50: mov  x4, x19                            ; die
9475e54: bl   0x9453e7c                          ; setPSAutoMinLevel(group,index,level,die)
9475e58: cbz  x21, +0x20                          ; if disabling: DONE, no poll
9475e78: 88 62 70 f9   ldr  x8, [x20, #0x60c0]    ; PS reg-group table
9475e7c: 89 01 80 52   mov  w9, #0xc              ; stride 0xC
9475e80: c8 22 a9 9b   umaddl x8, w22, w9, x8     ; &psGroups[group]
9475e84: 01 21 40 29   ldp  w1, w8, [x8]          ; w1 = entry.map, w8 = entry.base
9475e88: 02 0d 17 0b   add  w2, w8, w23, lsl #3   ; off = base + 8*index
9475e9c: mov x17,#0x1060                          ; ApplePMGR::waitReg32
9475eb4: 03 1e 80 52   mov  w3, #0xf0             ; mask  = 0x000000F0
9475eb8: 04 1e 80 52   mov  w4, #0xf0             ; value = 0x000000F0
9475ebc: 05 c0 9d 52   mov  w5, #0xee00
9475ec0: 45 00 a0 72   movk w5, #2, lsl #16       ; timeout = 0x0002EE00
9475ec4: e6 03 13 aa   mov  x6, x19                ; die
9475ef8: braa x16, x17
```

So the *enable* direction is: **write, then poll**; the *disable* direction is
**write only, no poll**.

### 3.3.2 `ApplePMGR::_setPSAutoMinLevel(u8 group, u8 index, u16 level, u8 die)` = the actual MMIO — `0xfffffe0009453e7c`

Name from `__ZN9ApplePMGR18_setPSAutoMinLevelEhjhj` (`0xfffffe0009453f58` is
`+0xdc` of it, so the entry is `0x9453e7c`).

```
9453e98: 18 18 40 91   add  x24, x0, #6, lsl #12   ; x24 = this + 0x6000
9453e9c: 08 cb 40 b9   ldr  w8, [x24, #0xc8]       ; w8 = [this+0x60c8] = #groups
9453ea0: 1f 01 01 6b   cmp  w8, w1 ; b.ls -> panic ; group < count
9453ec0: bl   0x9453bfc                          ; _getMaxDevicePsRegSize() -> w0
9453ec4: df 02 00 6b   cmp  w22, w0 ; b.hs -> panic
9453ec8: 88 62 70 f9   ldr  x8, [x20, #0x60c0]    ; PS reg-group table
9453ecc: 89 01 80 52   mov  w9, #0xc
9453ed0: e9 22 a9 9b   umaddl x9, w23, w9, x8     ; &psGroups[group]
9453ed4: 29 09 40 b9   ldr  w9, [x9, #8]          ; entry.validMask
9453ed8: 29 25 d6 1a   lsr  w9, w9, w22           ; bit `index`
9453edc: 89 00 00 37   tbnz w9, #0, +0x10         ; bit set -> ok
9453ee0: 89 0a 5c b9   ldr  w9, [x20, #0x1c08]    ; else: only allowed on layout v2
9453ee4: 3f 09 00 71   cmp  w9, #2 ; b.ne -> panic
9453f38: 8a 01 80 52   mov  w10, #0xc
9453f3c: 28 21 aa 9b   umaddl x8, w9, w10, x8
9453f40: 17 21 40 29   ldp  w23, w8, [x8]        ; w23 = entry.map, w8 = entry.base
9453f44: 16 0d 16 0b   add  w22, w8, w22, lsl #3 ; off = base + 8*index
9453f58: mov x17,#0x1050                          ; ApplePMGR::readReg32
9453f78: blraa x8, x16                            ; w0 = current value
9453f7c: 08 4c 10 53   ubfx w8, w0, #0x10, #4    ; bits [19:16] = request field
9453f80: 1f 01 15 6b   cmp  w8, w21               ; == level ?
9453f84: e1 00 00 54   b.ne +0x1c                ; equal -> return, NO WRITE
9453fa0: e8 03 14 aa   mov  x8, x20
9453fa8: e9 9f 9f 52   mov  w9, #0xfcff
9453fac: 09 fe bf 72   movk w9, #0xfff0, lsl #16 ; w9 = 0xFFF0FCFF
9453fb0: 03 00 09 0a   and  w3, w0, w9
9453fb4: a3 0e 10 33   bfi  w3, w21, #0x10, #4   ; w3 = (v & ~0xF<<16 & ~0x3<<9) | level<<16
9453fc0: mov x17,#0x1058                          ; ApplePMGR::writeReg32
9453fe0: mov  x4, x19                            ; die
9453fe4: braa x16, x17
```

**This is the whole clock/power-gate write.** Note the `csel` at `0x9475e40`
(`03 01 9f 1a`, `eq` selects `0xF`) and the `cbz x21` at `0x9475e58`: for this
caller the poll in step 9 only happens at `level = 0xF`. Points that matter for
a direct MMIO reimplementation:

* polarity: the field is **active-high on the "pinned awake" side**. `0xF` means
  "minimum power state = max" = running; `0x0` means "no floor" = the hardware
  may gate it. It is neither a plain OR of a bit nor an AND with a clear-mask —
  it is a 4-bit field replace, so bits 19..16 are all driven.
  The other cleared bits (`9`, `10`) are collateral of the literal mask
  `0xFFF0FCFF` and are always written back as 0.
* **Which direction means what depends on the caller.** For
  `enableDeviceAutoClockGating` the mapping is compiled-fact and counter-intuitive:
  `0x9475e40` `03 01 9f 1a csel w3,w8,wzr,eq` gives `level = enable ? 0x0 : 0xF`,
  so `enable = 1` (turn auto-gating ON) *clears* the field and `enable = 0`
  *sets* it to `0xF`. `_enableDeviceClock`'s path computes the opposite sense
  (`0x9473ee8`), so "set bits 19:16" is not synonymous with "turn the clock on".
* the write is skipped entirely if the field already holds the requested value.
* **there is exactly one write, and no pre-write to a "clock off" register, no
  unlock/lock pair, and no other register touched on the way.**

### 3.4 `ApplePMGRFunctionEnableAutoClockGate::callFunction` — `0xfffffe000948bdf8`

This is the *other* class (the one at the address in my original brief). It is
the "auto clock gating" DT function (`function-auto_clock_gate`), not the clock
gate:

```
948be14: 16 30 40 b9   ldr  w22, [x0, #0x30]      ; packed {die:4, id:28}
948be18: d3 fe 5c d3   lsr  x19, x22, #0x1c      ; die
948be1c: ldr  x0, [x0, #0x28]                    ; provider object (ApplePMGR)
948be30: mov  x17, #0xb30 ; getDieCount() ; b.hs -> panic 0x949f7dc
948be60: mov  x17, #0x980                        ; ApplePMGR::_enableDeviceAutoClockGatingEmmm
948be6c: and  x1, x22, #0xfffffff                ; device id
948be70: and  x2, x21, #1                        ; enable
948be74: mov  x3, x19                            ; die
948be7c: blraa x8, x16
```

So the constant `0x30` in the function object is `{die << 28 | device-id}`, and
the call lands in `_enableDeviceAutoClockGatingEmmm` (§3.3), whose three
arguments are exactly `(id, enable, die)` — which is what distinguishes it from
`ApplePMGRNub::enableDeviceAutoClockGatingEjb` (two arguments, and it does its
own array lookup). **This is the path that reaches `_setPSAutoMinLevel`.**

`ApplePMGRFunctionPowerGate::callFunction` `0xfffffe00094869ec` is 4 instructions:
`mov x2,x3; ldr x0,[x0,#0x28]; ldr x8,[vptr+0x968]; blraa` — code slot `0x968`
resolves to `ApplePMGR::_wasDeviceDisabled(unsigned long, unsigned long*)`, so
the power-gate function object *queries* prior state rather than performing the
write itself. I did not trace what consumes its result; state that as a gap.

### 3.5 The `enableDeviceClock` / `enableDevicePower` vtable slots

The task statement's `+0x8a8` / `+0x8b0` is **correct**, once the `+0x10` base
correction is applied (`__ZTV + 0x10 + 0x8a8`):

| class | `__ZTV` | code slot `0x8a8` | code slot `0x8b0` |
|---|---|---|---|
| `AppleARMIO` | `0xfffffe0007e275e0` | `0x896974c` **stub → `0xe00002c7`** | `0x896975c` **stub → `0xe00002c7`** |
| `AppleARMIODevice` | `0xfffffe0007e28058` | `0x896a304` real | `0x896a414` real |
| `AppleH17PPlatformIO` | `0xfffffe0008130878` | inherits `AppleARMIO`'s stub | inherits the stub |
| `AppleARMPerformanceController` | `0xfffffe0007e2d328` | `_updateDVDParameters` | `_enableDeviceClockGatedEmm` |

`AppleARMIO::enableDeviceClock` and `enableDevicePower` are literally

```
0xfffffe000896974c: 5f 24 03 d5   bti  c
0xfffffe0008969750: e0 58 80 52   mov  w0, #0x2c7
0xfffffe0008969754: 00 00 bc 72   movk w0, #0xe000, lsl #16
0xfffffe0008969758: c0 03 5f d6   ret
```

i.e. `kIOReturnUnsupported`. **The platform object does not implement them.** The
real implementations are on `AppleARMIODevice`, and `AppleARMIO` does not
override them.

Note that `AppleH17PPlatformIO` (`com.apple.driver.AppleT8140`) *does* have its
own `enableDeviceClock` / `enableDevicePower`, but at addresses
`0x9791310` / `0x97913e4`, which are **plain non-virtual methods** (they are
reachable only by `bl`), and its own `__ZTV` still inherits the stubs. So which
implementation the ANE reaches depends entirely on whether its provider object is
an `AppleARMIODevice` or an `AppleH17PPlatformIO`. Both provide a method with the
same name and the same argument shape, and both eventually call
`ApplePMGRFunctionClockGate::callFunction`.

---

## 4. Polarity, mask, offset — the answer

### 4.1 The one rule

For **any** PMGR device PS register (clock gate or power gate — they share this
one field; `ApplePMGR::_enableDeviceAutoPowerGatingGatedEmm` uses the same
`setPSAutoMinLevel` with `level` `0` / `0xF`):

```
off = psGroups[group].base + 8*index        // group, index from the DT `devices` record
v   = read32(PMGR + off)
v   = (v & 0xFFF0FCFF) | (level << 16)      // level = 0x0 (no floor) or 0xF (pinned)
write32(PMGR + off, v)                      // skipped if ((v>>16)&0xF) == level already
if (level == 0xF)                           // only the "pin awake" direction is polled
    while (read32(PMGR + off) & 0xF0) != 0xF0 and now < t0 + 0x2EE00 ticks: ;
```

* **Polarity: active-high.** `0xF` in bits `[19:16]` = "raise this device's
  minimum power state to the top" = running. `0x0` = "let it drop" = gated.
  (The code is `_setPSAutoMinLevel`; the `level` argument is literally the
  minimum PS level the hardware is allowed to auto-select. Setting the floor to
  `0` is what permits auto clock/power gating; setting it to `0xF` is what pins
  the device awake. **The `enable` argument of
  `enableDeviceAutoClockGating` maps to `level = 0`, not `0xF`** — see the
  `csel` at `0x9475e40`, `03 01 9f 1a`: `eq` (enable == 0) selects `0xF`.)
* **Ack field: bits `[7:4]`**, expected `0xF` after a `level = 0xF` write. Bits
  `[9:10]` are always forced to 0 by the write mask. Everything else is
  preserved.

**Warning for the freeze.** This function performs a read-modify-write of a full
32-bit register and only intends to change six of its bits (16–19, plus 9–10
which it forces to 0). If the Linux side writes this register without first
reading it, it will clobber whatever the boot chain or the PMP left in the other
26 bits. The freeze reports are consistent with that failure mode, but this
analysis cannot prove it.

### 4.2 Which offset the ANE gates actually use

Two distinct kinds of id are in play, and they go to different registers:

| DT id | consumer | function | group | PS offset | physical |
|---|---|---|---|---|---|
| 291 `0x123` (`clock-gates[0]`) | `AppleARMIODevice::enableDeviceClock` | `_enableDeviceEmmmm` → `_enableDeviceGatedEmmmm` → **PMP command** | — | — | — |
| 338 `0x152` (`power-gates[0]`) | `AppleARMIODevice::enableDevicePower` | same | — | — | — |
| 291/338 again, via the `function-auto_clock_gate` object | `ApplePMGRFunctionEnableAutoClockGate::callFunction` | `_enableDeviceAutoClockGatingGatedEmmm` → `setPSAutoMinLevel` | 0 | `0x000` | `0x3_0070_0000` |
| `ANE_SYS` `0x3c` | `_setPSLevel` / `_getPSLevel` family | `setPSLevel` | 0 | `0x290` | `0x3_0070_0290` |
| `ANE_CPU` `0x81` | as above | | 0 | `0xc000` | `0x3_0070_c000` |

(The third row is **inferred**: I have not read the `0x30` constant of the
`function-auto_clock_gate` object on the ANE node, so "the auto-clock-gate
function object carries id 291/338" is an assumption, not a decoded fact.)

The `0x0` for 291/338 is not a transcription slip: `_deviceIDToDeviceData(0x123)`
returns record 267 whose `+0x10` word is `0x00000000`, and `_getDevicePsRegIndex`
for layout v2 is `(rec[+0x10] >> 3) & 0x1FFFFF` (`0x9453bd8: 00 5d 03 53
ubfx w0, w8, #3, #0x15`). Record 59 (`ANE_SYS`, `0x3c`) has `+0x10 = 0x290`,
which is why `0x290` and `0xc000` do decode to plausible register offsets and
`0x0` does not. **This asymmetry is unexplained and is the single largest
remaining gap** (§8.3).

---

## 5. Where `group` and `index` come from — `ps-groups`, proven

### 5.1 The table is a verbatim DT property

`ApplePMGR::initDriver` `0xfffffe0009448f80`…`0x94490b8`:

```
9448f80: cmp  w8, #1 ; b.eq 0x9448f9c      ; layout v1 -> "ps-regs"
9448f88: cmp  w8, #2 ; b.ne 0x9450588      ; else      -> "ps-groups"
9448f90: adrp x1,#0x7578000 ; add x1,x1,#0xf7d    ; 0xfffffe0007578f7d = "ps-groups"
9448f9c: adrp x1,#0x7578000 ; add x1,x1,#0xf75    ; 0xfffffe0007578f75 = "ps-regs"
9448fb4: mov  x17,#0x2d8                          ; IOService::getProperty(OSSymbol*)
9448fd4: mov  x1, x22
9448fd8: bl   0xfffffe000bb29ac8                  ; getProperty
9448ff4: mov  x17,#0x160                          ; OSData::getLength()
9449008: 68 55 95 52   mov  w8, #0xaaab
944900c: 48 55 b5 72   movk w8, #0xaaaa, lsl #16 ; w8 = 0xAAAAAAAB
9449010: 08 7c a8 9b   umull x8, w0, w8
9449014: 09 fd 63 d3   lsr  x9, x8, #0x23          ; count = len / 12
9449018: 89 4b 03 b9   str  w9, [x28, #0x348]      ; this->psGroupCount
9449024: 01 f5 7e d3   lsl  x1, x8, #2             ; 12*count bytes
9449030: bl   0xfffffe000bb9fe8c                  ; kalloc
9449034: 60 62 30 f9   str  x0, [x19, #0x60c0]     ; this->psGroups  <-- the table
944907c: bl   0xfffffe000b394bf0                  ; memcpy(psGroups, prop, 12*count)
944909c: 41 00 40 b9   ldr  w1, [x2]               ; field 0 = a memory-resource index
94490a0: bl   0xfffffe00094505b4                  ; _findRegMap(this, w1, entry)  -> rewrites [0] to a map id
```

**`ps-regs` does not exist in the J700 ADT; `ps-groups` does.** So layout **v2**
is selected, which matches `_getDevicePsRegGroup`'s v2 branch.

Verification of the property strings:

```
python3 receipts/2026-11-04-j700-h17-regmap-71/scripts/strs.py \
    0xfffffe0007578f75 0xfffffe0007578f7d 0xfffffe0007578f89 0xfffffe0007578f9d
  0xfffffe0007578f75: 'ps-regs'
  0xfffffe0007578f7d: 'ps-groups'
  0xfffffe0007578f89: 'site.PSRegGroupData'
  0xfffffe0007578f9d: '_psRegGroups'
```

Grep of the ADT: `ps-regs` → **absent**; `ps-groups` → present at file offset
`0x2fe04`; `perf-regs` → absent; `pwrgate-regs` → absent.

### 5.2 The table contents, decoded from the ADT

`/arm-io/pmgr` `ps-groups`, 24 bytes = **2 records × 12 bytes**
(`struct { u32 resourceIndex; u32 base; u32 validMask; }`):

| group | resourceIndex | base | validMask |
|---|---|---|---|
| 0 | `0x0` | `0x00000000` | `0x00000000` |
| 1 | `0x1` | `0x00000000` | `0x00000000` |

`_findRegMap(this, resourceIndex, …)` maps resource index → `RegMap`, and
`AppleT8140PMGR::initRegMaps` uses the same indices: resource `0` → map `0x00`
(`/arm-io/pmgr` `reg[0]` = `0xf0700000`/`0xcc000`), resource `1` → map `0x01`
(`reg[1]` = `0xf8280000`/`0x80000`).

Physical translation (same `/arm-io` `ranges` rule as the prior report:
child `< 0x2f000000` → `+0x210000000`):

| map | ADT base | physical base |
|---|---|---|
| `0x00` | `0xf0700000` | **`0x3_0070_0000`** |
| `0x01` | `0xf8280000` | **`0x3_0828_0000`** |
| `0x71` (ANE clkgen, **not used here**) | `0x1f1878000` | `0x4_0187_8000` |

The `validMask` of `0` means "no index declared valid"; the code tolerates that
only because `[this+0x1c08] == 2`
(`0x9453ee0: ldr w9,[x20,#0x1c08]; 0x9453ee4: cmp w9,#2`).

### 5.3 Device record layout and the ANE entries

`ApplePMGR::_deviceIDToDeviceData(u16 id)` `0xfffffe0009454b6c`:

```
9454b80: cmp  w1, #0x320 ; b.hs -> panic        ; 800 id slots
9454b88: mov  w8, #0x9d40                       ; u16 id -> record index table
9454b90: 28 59 61 78   ldrh w8, [x9, w1, uxtw #1]
9454ba0: mov  w9, #0x90a8                       ; record array base
9454bac: mov  w10, #0x30                        ; 0x30-byte records
9454bb0: 00 25 aa 9b   umaddl x0, w8, w10, x9
```

Records are 48 bytes — exactly the `/arm-io/pmgr` `devices` property
(14 208 B = **296 × 48**).

`ApplePMGR::_getDevicePsRegGroup(DeviceID)` `0xfffffe0009453b4c`:

```
9453b60: bl   _deviceIDToDeviceData
9453b64: ldr  w8, [x19, #0x1c08]
9453b68: cmp  w8, #2 ; b.eq 0x9453b80           ; layout v2:
9453b70: cmp  w8, #1 ; b.ne -> assert
9453b78: 00 2c 40 39   ldrb w0, [x0, #0xb]      ;   v1: rec->+0x0b
9453b80: 00 4c 40 39   ldrb w0, [x0, #0x13]     ;   v2: rec->+0x13   <-- J700
9453b84: cmp  w0, #0x20 ; b.eq -> assert
9453b98: bl   #0xfffffe00094998e0              ;   "group != kPSRegGroupSize" (0xfffffe000758337e)
```

`ApplePMGR::_getDevicePsRegIndex(DeviceID)` `0xfffffe0009453ba0`:

```
9453bb8: ldr  w8, [x19, #0x1c08]
9453bd4: 08 10 40 b9   ldr  w8, [x0, #0x10]     ;   v2: u32 at rec+0x10
9453bd8: 00 5d 03 53   ubfx w0, w8, #3, #0x15   ;   index = (v >> 3) & 0x1FFFFF
9453bdc: mov  w8, #0xffff ; b.eq -> assert     ;   0xFFFF is the "none" sentinel
```

Decoded from the ADT `devices` blob:

| rec | deviceID | `+0x00` | `+0x04` | `+0x10` | `+0x13` | name | group | index | PS offset |
|---|---|---|---|---|---|---|---|---|---|
| 59 | `0x0003c` | `0x00000080` | `0x2f` | `0x00000290` | `0x00` | `ANE_SYS` | 0 | `0x52` | `0x290` |
| 128 | `0x00081` | `0x00000010` | `0x3c` | `0x0000c000` | `0x00` | `ANE_CPU` | 0 | `0x1800` | `0xc000` |
| 267 | `0x00123` | `0x00000010` | `0x3c` | `0x00000000` | `0x00` | `ANE-SYS-V` | 0 | `0` | `0x000` |
| 290 | `0x00152` | `0x06080012` | `0x00` | `0x00000000` | `0x00` | `ANE-SYS-V-PMP` | 0 | `0` | `0x000` |

(`+0x04` is the parent deviceID: `0x3c` = `ANE_SYS`, matching the prior round.)

---

## 6. The ANE clock-generator accessors are dead code

`ApplePMGR::readANEClkGenReg` `0xfffffe000946540c`,
`writeANEClkGenReg` `0xfffffe000946547c`,
`waitANEClkGenReg` `0xfffffe00094654f0` are the **only** functions that form
`map = 0x71 + bank` and touch the `0x401878000` window.

| property | result |
|---|---|
| `bl` callers, whole image | **0 / 0 / 0** (`blscan.py`, all three) |
| chained-fixup pointer references (`low32 == VA - image_base`) | **0 / 0 / 0** |
| entry in any `__ZTV` (either base convention) | **none** |

So on J700 macOS never writes the ANE clock generator through these. The
`0x1000`-based group area in map `0x71` that the Linux side has been poking is
**not** on the macOS enable path at all.

This is consistent with §5: everything macOS programs for an ANE gate goes into
PMGR window 0 at `0x3_0070_0000`.

`writeANEClkGenReg`'s shape, for completeness (it is what a future
`Android`/`m1n1` port would use if this block ever needs writing):

```
9465448: 41 c4 01 11   add  w1, w2, #0x71      ; map = 0x71 + bank
9465450: mov  x2, x8                           ; offset
...      braa x16, x17                        ; -> readReg32
94654a0: ...                                   ; writeReg32
```

---

## 7. The live map-0x71 dump, interpreted against the decode

Data from `receipts/2026-11-04-j700-h17-clkgen-live` (read-only, 4096 reads, no
fault): `0x1000..0x1008 = 0x80000000` (three words), `0x11f0 = 0x1f`,
`0x11f4 = 0xffffffff`, `0x11f8 = 0xffffffe7`, `0x11fc = 0x1`, and everything in
`0x100c..0x11ef` is zero. `0x1200` is the PLL/div table, `0x1804` the divider
table, `0x1ff8` an end marker.

### 7.1 Group 0x20's window is exactly `0x1000..0x11ff`

`initRegGroups` gives group `0x20` → `(map 0x71, offset 0x1000)`
(`0x983735c: mov w1,#0x20 ; 0x9837360: mov w2,#0x71 ; 0x9837364: mov w3,#0x1000`).
Both candidate layouts fit that window exactly:

* **PS-slot layout** (`setPSAutoMinLevel`): 8 bytes per index, so
  `0x1000..0x11ff` = **64 slots, index 0..63**, and
  `0x11f0 = 0x1000 + 8*62`, `0x11f8 = 0x1000 + 8*63`. The window ends precisely
  where the PLL table begins at `0x1200`.
* **`_printClocks` layout**: `base + 0x100 + n*stride` with `stride = 0x10` gives
  16 records of 16 bytes at `0x1100..0x11ff`; `0x11f0` is `n = 15`, the last.

Both are exact. Nothing offline separates them.

### 7.2 Do `0xffffffff` / `0xffffffe7` read as "gate closed" or "gate open"?

Under the decoded field layout (§4.1) the two fields are
`[19:16] = request` and `[7:4] = ack`, `0xF` = running:

| word | value | `[19:16]` | `[7:4]` | reading |
|---|---|---|---|---|
| `0x11f0` | `0x0000001f` | `0x0` | `0x1` | request 0 (gated), ack 1 (not `0xF`) |
| `0x11f4` | `0xffffffff` | `0xF` | `0xF` | request `0xF`, ack `0xF` — "running and acknowledged" |
| `0x11f8` | `0xffffffe7` | `0xF` | `0x6` | request `0xF`, ack `0x6` — **not** acknowledged |
| `0x11fc` | `0x00000001` | `0x0` | `0x0` | request 0, ack 0 |

So, **if** these are PS registers, `0xffffffff` is *fully open* and `0xffffffe7`
is *un-acknowledged*, not "all closed". An all-ones value is not a
"gate closed" pattern in this encoding — closing is `0x0` in bits `[19:16]`.

**However**: this must be read as conditional. Three things argue against
treating them as PS registers:

* under the `_printClocks` layout they are instead one 16-byte clock record
  `{0x1f, 0xffffffff, 0xffffffe7, 0x1}`, which is a much more natural shape for
  a clkgen block (divider/mux/status/enable) than four barely-related PS slots;
* §6 shows macOS never writes here at all;
* every other PS slot in the window (`0x100c..0x11ef`) is zero, which is
  improbable for a PS group whose devices are in use.

**Conclusion, stated as inference:** `0x11f0..0x11ff` is most likely the
**16th and last clock record of group `0x20`**, not a gate register, and the
"all gates closed" hypothesis is not supported by the encoding. What *is*
proven is that values `0xffffffff`/`0xffffffe7` cannot be interpreted as
"everything gated" under the PMGR encoding this driver actually uses.

### 7.3 What the live dump does *not* tell us

It says nothing about whether the ANE's *functional* clock is running, because
the register that would say so is at `0x3_0070_0290` (map `0x00`), which was not
mapped by the clkgen scan.

---

## 8. The ordered recipe, and what is still unknown

### 8.1 What is fully determined (Apple's own sequence)

For writing one PMGR device's PS-register field, in this order, and nothing else:

```
 1. group = devices[id].byte[0x13]                 # from /arm-io/pmgr `devices`
 2. index = (devices[id].u32[0x10] >> 3) & 0x1FFFFF
 3. map, base = psGroups[group].map, psGroups[group].base
 4. off = base + 8*index
 5. v = read32(map_window + off)                   # readReg32, bounds-checked
 6. if ((v >> 16) & 0xF) != level:                 # caller-chosen, 0x0 or 0xF
 7.     write32(map_window + off, (v & 0xFFF0FCFF) | (level << 16))
 8. if level == 0xF:                               # only the "pin awake" direction
 9.     wait until (read32(map_window + off) & 0xF0) == 0xF0, timeout 0x2EE00
10. on timeout: read once more; if still mismatched, panic path in
    ApplePMGR::_getDeviceID / kdebug; the caller is not told "try again".
```

`level` per caller, both compiled facts:

| caller | VA | expression | meaning |
|---|---|---|---|
| `enableDeviceAutoClockGating` | `0x9475e40` `03 01 9f 1a csel w3,w8,wzr,eq` | `enable ? 0x0 : 0xF` | enable auto-gating = drop the floor to 0 |
| `_enableDeviceClock` → `_enableDeviceGatedEmmmm` | `0x9473ee8` `37 11 88 1a csel w23,w9,w8,ne` | `arg2 != 0 ? 0xF : ((arg3 & 1) << 2)` | a device-state value in {0, 4, 0xF} — see note |

**Note on the second row (deliberately cautious).** In `_enableDeviceGatedEmmmm`
the `csel` result `w23` is written to a per-device byte array
(`0x9473fa0: strb w23,[x8]`) and passed to
`_updateDeviceStatus(id, w23, old, …)` (`0x9456b00`), so it is a *device status
enum* in `{0, 4, 0xF}` — the same value set as the PS level field, and almost
certainly the same encoding, but I have not proven it is the same register
field. `_enableDeviceGatedEmmmm` never calls `setPSAutoMinLevel`; it routes to
`_sendPMPCommand`. Treat "`_enableDeviceClock` writes bits [19:16]" as
**inferred**, not proven.

`waitReg32` `0xfffffe0009463ebc` semantics, from the instruction stream:

```
9463f20: bl   0xfffffe000b5a143c          ; t0 = mach_absolute_time()
9463f30: 1a 40 3a 8b   add x26, x0, w26, uxtw   ; deadline = t0 + timeout
9463f3c: mov x17,#0x1050 ; readReg32
9463f60: 08 00 14 0a   and w8, w0, w20          ; mask
9463f64: 1f 01 15 6b   cmp w8, w21              ; vs value
9463f68: c0 02 00 54   b.eq -> success
9463f6c: bl   mach_absolute_time ; cmp deadline, now ; b.hi -> loop
9463f80: ... repeat read; if [this+0x16d] bit0 clear -> return anyway
9464000: ... else _getDeviceID + panic path
```

Timeout `0x2EE00` = **192 512 ns ≈ 192.5 µs**, in `mach_absolute_time` units
(nanoseconds on Apple Silicon). That is the only timing constraint in the whole
path — there is **no PLL-lock wait, no reset pulse, no delay between write and
poll**.

### 8.2 A direct-MMIO recipe — and why it is not the same thing

If you only want to replicate `setPSAutoMinLevel` for `ANE_SYS` (deviceID `0x3c`),
which is the register the decode actually lands on with a plausible offset:

```
physical 0x3_0070_0290   bits [19:16] = 0x0   -> gated / low floor
physical 0x3_0070_0290   bits [19:16] = 0xF   -> pinned running
then poll bits [7:4] == 0xF for up to ~192 µs
always read-modify-write with mask 0xFFF0FCFF
```

For gate ids 291 (`ANE-SYS-V`) and 338 (`ANE-SYS-V-PMP`) the same formula yields
`0x3_0070_0000` (offset `0x0`) and `0x3_0070_0000` respectively. **An offset of
`0x0` into the PMGR window is not a credible gate register**, and I will not
present it as a recipe. See §8.3.

Additionally, `AppleT8140PMGR::writeReg32` `0xfffffe000983bc88` is **not** a plain
store: when `[this+0x738e0] != 0` it first calls
`AppleT8140PMGR::isPsRegEN(map, off, val, die)` (code slot `0x1068`,
`__ZTV14AppleT8140PMGR + 0x1078` → `0xfffffe000983c198`), then scans a 10-entry,
`0x28`-byte table `_axi2afWorkaroundPsRegs` at `0xfffffe000c465600` for
`(map, offset)`:

```
rec0 (0,0x3e8) rec1 (0,0x280) rec2 (0,0x288) rec3 (0,0x278) rec4 (0,0x270)
rec5 (0,0x240) rec6 (0,0x248) rec7 (0,0x1f0) rec8 (0,0x230) rec9 (0,0x2a0)
```

A matching write takes the workaround branch at `0x983bfb8`. The ANE offsets
`0x290` and `0x0` are **not** in the table, but `0x290` sits inside the
`0x280..0x2a0` cluster, so a real register write there almost certainly obeys
ordering rules this table encodes. **Do not write `0x290` blind.**

### 8.3 What I could not resolve — explicitly

1. **Why gate ids 291/338 resolve to group 0 / index 0.** The two DT ids whose
   records (`267`, `290`) encode `+0x10 == 0` and `+0x13 == 0`. Either the
   `clock-gates` values are *not* `devices`-table ids in the sense I assumed
   (`_deviceIDToDeviceData` looks them up by index into the 800-entry u16 map,
   and I cannot reconstruct that map's ordering statically — it is built at
   runtime from the `devices` blob), or the gate write for those ids does not go
   through `setPSAutoMinLevel`. **I cannot show the u16 `id → record` array**, so
   the `291 → rec 267` step is an assumption inherited from the prior round, not
   something I re-proved here. This is the biggest single hole.
2. **Whether the ANE's `enableDeviceClock` reaches `setPSAutoMinLevel` at all.**
   §3.3/§3.5 show `AppleARMIODevice::enableDeviceClock` → gate id →
   `ApplePMGRFunctionClockGate::callFunction` → `_enableDeviceEmmmm` →
   `_enableDeviceGatedEmmmm`, and that function's only MMIO-free route is
   `_sendPMPCommand` (a mailbox to the PMP). So the *functional clock enable* on
   this SoC is a **PMP command**, not a register write. Which register the PMP
   then writes is inside the PMP/APSC, not in this kernelcache.
3. **Which of the two `0x11f0..0x11ff` readings is right** (§7.2).
4. **The runtime value of `[this+0x1c08]`** (layout v1/v2). It is set in
   `ApplePMGR::start` `0x9440a98`…`0x9440ab0`:
   `ldrb w8,[x9,#0x9a0]; tbz w8,#0 -> default 1; else ldr w8,[x19,#0x250c];
   str w8,[x19,#0x1c08]`. The default is **1**; v2 requires a feature bit in a
   table I did not decode. `ps-groups` (v2-only) being present in the ADT is
   strong evidence for v2 but is not proof that the feature bit is set.
5. **The physical/KVA mapping of `0x3_0070_0000`** — that is the Linux side's
   problem; PMGR window 0 is `/arm-io/pmgr` `reg[0]`.

---

## 9. Statement of method and limits

**Proven by instruction stream at the stated VAs**

* the `vptr = __ZTV + 0x10` convention, twice-independently (§2.1);
* `ApplePMGRFunctionClockGate::callFunction` `0x94864f4` is an argument shim
  that dispatches code slot `0x948` = `_enableDeviceEmmmm` (§3.1);
* `ApplePMGRNub::enableDeviceAutoClockGatingEjb` `0x9491510` dispatches code
  slot `0xba0` = `_enableDeviceAutoClockGatingGatedEmmm` (§3.2);
* `ApplePMGR::_enableDeviceAutoClockGatingGatedEmmm` `0x9475ca0` computes
  `level = enable ? 0 : 0xF`, calls `setPSAutoMinLevel`, and — **only when
  enabling** — polls mask `0xF0` against `0xF0` with timeout `0x2EE00` (§3.3.1);
* `ApplePMGR::_setPSAutoMinLevel` `0x9453e7c` does the single RMW of bits
  `[19:16]` with mask `0xFFF0FCFF` via `bfi`, skipping the write when the field
  already matches (§3.3.2);
* `ApplePMGR::waitReg32` `0x9463ebc` is a `mach_absolute_time` deadline loop with
  one post-timeout re-read and a panic path (§8.1);
* `ps-groups` is the DT property that fills `this+0x60c0`, stride `0xC`,
  `count = len/12`, field 0 rewritten by `_findRegMap` (§5.1);
* `_getDevicePsRegGroup` / `_getDevicePsRegIndex` field extraction and the
  `0x20` sentinel assert `"group != kPSRegGroupSize"` (§5.3);
* `AppleARMIO::enableDeviceClock`/`enableDevicePower` are
  `return kIOReturnUnsupported` stubs, and the real ones are on
  `AppleARMIODevice` at code slots `0x8a8`/`0x8b0` (§3.5);
* `readANEClkGenReg`/`writeANEClkGenReg`/`waitANEClkGenReg` have zero callers and
  zero references (§6).

**Proven by data**

* `ps-groups` exists in the J700 ADT (24 B, 2 records, both `base = 0`,
  `validMask = 0`); `ps-regs`, `perf-regs`, `pwrgate-regs` do not (§5.1–5.2);
* the `devices` blob is 296 × 48 B and records 59/128/267/290 carry the
  `+0x10`/`+0x13` values in §5.3;
* `_axi2afWorkaroundPsRegs` is 10 × `0x28` B with the offsets listed in §8.2;
* the maps `0x00`/`0x01` window bases `0xf0700000`/`0xf8280000` and the
  `+0x210000000` translation (§5.2).

**Inference, labelled as such**

* that layout v2 is selected at runtime on J700 (§8.3.4);
* that `0x11f0..0x11ff` is a 16-byte clock record rather than four PS slots
  (§7.2);
* that the ANE's `enableDeviceClock` reaches `_enableDeviceGatedEmmmm`'s PMP
  branch rather than the direct path (`[this+0x1b70]` value not statically
  known);
* that `291 → devices record 267` (§8.3.1).

**Not derivable offline**

* the runtime `id → devices-record` index array at `this+0x9d40`;
* what the PMP/APSC does with a device-enable command;
* whether any part of map `0x71` is writable, or aliased through the ANE CSR
  aperture (map `0x6c`).

Nothing here is device qualification. Code reading and a register map are not
device behaviour, and a covered geometry is not a covered neighbourhood.

---

## 10. Explicit not-founds

| searched | result |
|---|---|
| ADT property `ps-regs` | **not found** |
| ADT property `perf-regs`, `pwrgate-regs` | **not found** (confirms earlier round) |
| ADT property `ps-groups` | **found**, 24 B, 2 records |
| `bl` to `readANEClkGenReg` / `writeANEClkGenReg` / `waitANEClkGenReg` | **0** each |
| pointer to any of the three, image-wide | **0** each |
| any `__ZTV` slot for any of the three | **none** |
| code slot `0x8a8` / `0x8b0` resolving to `enableDeviceClock` / `enableDevicePower` | **yes**, on `AppleARMIO` and `AppleARMIODevice`, only with the `+0x10` base |
| `AppleARMIO::enableDeviceClock` body | `return 0xE00002C7` (two instructions + `ret`) |
| direct `writeReg32` in `_enableDeviceGatedEmmmm` | **none** — it dispatches `_sendPMPCommand` |
| PS register at `0x3_0070_0290` appearing in `_axi2afWorkaroundPsRegs` | **no** (nearest neighbours `0x280`/`0x288`/`0x2a0` are) |

---

## 11. Files

```
receipts/2026-11-04-j700-h17-clkgen-enable-path/
├── README.md            this report
└── scripts/
    ├── vtslot.py        resolve a code slot against a __ZTV using vptr = __ZTV+0x10
    │                    (supports `--find <substring>` across all 151 vtables)
    ├── slots.py         list every vtable dispatch inside a VA range
    └── findslot.py      find every `mov x17,#SLOT` site in the whole PMGR text,
                         annotated with the enclosing function
```

Reproduce the load-bearing claims:

```
python3 scripts/vtslot.py 0xfffffe0008060c30 0xb30 0x948 0x1050 0x1058 0x1060
python3 scripts/vtslot.py 0xfffffe000814e778 0x1058 0x1068
python3 scripts/findslot.py 0xfffffe000943ff90 0xfffffe00094a0000 0x1050 0x1058 0x1060
python3 ../2026-11-04-j700-h17-regmap-71/scripts/kdis.py 0xfffffe0009453e7c 90
python3 ../2026-11-04-j700-h17-regmap-71/scripts/kdis.py 0xfffffe0009475ca0 220
python3 ../2026-11-04-j700-h17-regmap-71/scripts/kdis.py 0xfffffe00094864f4 62
```
