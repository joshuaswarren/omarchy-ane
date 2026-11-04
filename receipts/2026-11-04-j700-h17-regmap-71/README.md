# J700 / T8140 (H17): `ApplePMGR::initRegMap` and **RegMap 0x71 = `SNE_ANECLKGEN_TOP`**

Offline binary analysis only. No hardware, no module load, no device access.
All addresses are kernelcache VAs; every claim carries the instruction address
(and, where useful, the raw bytes) so it can be re-checked.
Everything below is decompiled from a retained kernelcache, i.e. *code reading*,
not device behaviour.

---

## 0. Bottom line

> **Map 0x71 is the ANE clock-generator window and it IS initialised: from
> `/arm-io/pmgr` `reg` entry **36 (0x24)**, ADT address `0x1_f1878000`,
> size `0x4000` — i.e. physical `0x4_0187_8000` after the `/arm-io` `ranges`
> translation (`+0x2_1000_0000`).**
>
> With `initRegGroups` group `0x20 -> (map 0x71, offset 0x1000)`, the ANE
> clock-generator register bank starts at ADT **`0x1_f1879000`** =
> physical **`0x4_0187_9000`** (`+0x100` for the clock registers →
> `0x4_0187_9100`, `+0x2c` for the power/gate registers → `0x4_0187_902c`,
> plus `stride*n`), *not* at `0xf82c8000 + 0x1100` — the latter is map
> `0x6c`, the ANE's 16 KiB CSR aperture (§4.5).
>
> Corroborated three independent ways (§4): the dedicated accessor
> `ApplePMGR::readANEClkGenReg()` hard-codes map `0x71`; the resulting window
> `0x1f1878000` lies inside the ANE's own 32 MiB ADT block
> `0x1f0000000+0x2000000`; and `ApplePMGR::readGFXClkGenReg()` uses the same
> mechanism with map `0x73 -> 0x270e1c000`, which lands inside `/arm-io/sgx`
> (the GPU). Every one of the 10 `initRegGroups` offsets also fits inside its
> resolved window.

Two corrections to the working assumptions in the task description:

1. `initRegMap` takes **no address argument**. Its second argument is an
   *index into the provider's device-memory array* (DT `reg` entry index);
   the physical base never appears in the call.
2. In `_printClocks`, `[x27+1]` is the **reg-group index**, not the ordinal.
   The ordinal `n` is `[x27+0]`. Neither byte comes from the
   `/arm-io/pmgr` `devices` blob; they come from `perf-regs` / `clocks`
   properties, which are **absent from the J700 ADT** (§5).

---

## 1. Evidence base

| item | value |
|---|---|
| kernelcache | `/var/tmp/neo/kernelcache.bin`, 114 688 000 bytes |
| SHA-256 | `7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe` (re-verified this session) |
| VA ↔ file offset | `VA = 0xfffffe0007004000 + file_offset` (checked against every instruction quoted below) |
| disassembler | capstone 5.0.7 (`scripts/kdis.py`) |
| ADT | `/var/tmp/opencode/adt.bin` (425 984 B), parsed with `/home/admin/linux-aurora/tools/aurora-sep/aurora-adt-extract.py` |
| symbols | `__ZN…` names from `/var/tmp/neo/syms_com_apple_driver_ApplePMGR.json`, `…AppleT8140PMGR.json` |

The two PMGR kexts in this cache:

* `com.apple.driver.ApplePMGR` — base class `ApplePMGR` (text `0x9440000…0x949xxxx`).
* `com.apple.driver.AppleT8140PMGR` — subclass `AppleT8140PMGR` (text `0x9836xxx…0x983dxxx`),
  `compatible = "pmgr1,t8140"` (ADT `/arm-io/pmgr`).

Scripts used are in `scripts/` (`kdis.py` linear disassembly, `blscan.py` whole-image
`bl`/`b` target scan, `calls.py` call-site argument recovery). Reproduce with e.g.

```
python3 scripts/kdis.py 0xfffffe000944879c 130
python3 scripts/blscan.py 0xfffffe000944879c
```

---

## 2. (a) `initRegMap` — location, signature, argument meaning

### 2.1 Symbol and signature

```
__ZN9ApplePMGR10initRegMapENS_6RegMapEjjb   @ 0xfffffe000944879c   (file off 0x244479c, 7f 23 03 d5 = pacibsp)
  ->  ApplePMGR::initRegMap(ApplePMGR::RegMap, unsigned int, unsigned int, bool)
```

So the register file is `x0 = this`, `w1 = RegMap`, `w2 = unsigned`, `w3 = unsigned`, `w4 = bool`
(the mangled name fixes the argument *types*; the roles below are proven from the body).

### 2.2 Which argument is the map id

`w1` is the map id. Proof — the map array is `this + 0x29e8` with a 0x60-byte stride
derived from `w1` **only**:

```
0x9448820: 08 3d 85 52   mov   w8, #0x29e8
0x9448824: d9 02 08 8b   add   x25, x22, x8          ; x25 = this + 0x29e8   (_regMaps)
0x9448828: e8 7e 7f d3   ubfiz x8, x23, #1, #0x20   ; x23 = w1
0x944882c: 08 41 37 8b   add   x8, x8, w23, uxtw    ; x8 = 3*w1
0x9448830: 08 e9 7b d3   lsl   x8, x8, #5           ; x8 = 0x60*w1
0x9448838: 38 c3 28 8b   add   x24, x25, w8, sxtw   ; &_regMaps[map]
```

and the accessor agrees (`ApplePMGR::getRegMap`, `0xfffffe0009454b08`):

```
0x9454b14: 3f 24 02 71   cmp   w1, #0x89        ; map id must be < 137
0x9454b1c: 88 63 8c 52   mov   w8, #0x631c
0x9454b20: 08 00 08 8b   add   x8, x0, x8
0x9454b24: 08 01 40 b9   ldr   w8, [x8]         ; w8 = this->dieCount
0x9454b28: 5f 00 08 6b   cmp   w2, w8           ; arg2 (die) < dieCount
0x9454b38: 09 06 80 52   mov   w9, #0x30
0x9454b3c: 49 7c a9 9b   umull x9, w2, w9      ; 0x30*arg2
0x9454b40: 0a 0c 80 52   mov   w10, #0x60
0x9454b44: 29 24 aa 9b   umaddl x9, w1, w10, x9 ; 0x60*map + 0x30*die
0x9454b4c: 00 c1 29 8b   add   x0, x8, w9, sxtw ; return &_regMaps[map][die]
```

`_regMaps[map][die]`, map stride `0x60` = 2 dies × `0x30`, `dieCount` at `this+0x631c`,
`mapCount` = `0x89`.

### 2.3 Which argument is the die

`w3`. Proof — it selects the 0x30-byte die slot inside the map (same arithmetic as
`getRegMap`), and it is bounds-checked against the die count:

```
0x9448800: 88 63 8c 52   mov   w8, #0x631c
0x9448808: 08 01 40 b9   ldr   w8, [x8]          ; dieCount
0x9448818: bf 02 08 6b   cmp   w21, w8
0x944881c: c2 11 00 54   b.hs  <cold.1>          ; assert
0x94488a0: e8 7e a8 9b   umull  x8, w23, #0x60
0x94488a8: a8 22 a9 9b   umaddl x8, w21, #0x30, x8   ; 0x60*map + 0x30*die
```

`readReg32(map, offset, die)` (`0xfffffe0009463c7c`) independently confirms the
`(map, offset, die)` order of the *accessors*: `w3` is bounds-checked against
`[this+0x631c]` before `getRegMap(w1, w3)` is called at `0x9463ccc`.

### 2.4 Which argument is the physical base — **none; w2 is a memory-resource index**

There is no address anywhere in the call. `w2` is:

1. **stored into the die record** (`+0x08`):
   ```
   0x94488c0: b4 0a 00 b9   str  w20, [x21, #8]      ; die->[8] = w2
   ```
2. **bounds-checked against the provider's memory count**, then
   **used as an index into the provider's memory array**:
   ```
   0x94487cc: 00 44 40 f9   ldr  x0, [x0, #0x88]    ; x0 = this->provider
   0x94487e0: 11 e0 80 d2   mov  x17, #0x700
   0x94487e4: 10 02 11 8b   add  x16, x16, x17
   0x94487e8: 08 02 40 f9   ldr  x8, [x16]
   0x94487f0: 10 09 3f d7   blraa x8, x16           ; w0 = provider->getDeviceMemoryCount()
   0x94487f4: 73 00 00 34   cbz  w19, +0x18         ; skip checks if !required
   0x94487f8: 9f 02 00 6b   cmp  w20, w0
   0x94487fc: 02 12 00 54   b.hs <cold.1>           ; assert w2 < count
   ...
   0x94489a0: 11 e2 80 d2   mov  x17, #0x710
   0x94489a8: 08 02 40 f9   ldr  x8, [x16]
   0x94489ac: e1 03 14 aa   mov  x1, x20            ; arg = w2
   0x94489b0: 02 00 80 52   mov  w2, #0
   0x94489b8: 10 09 3f d7   blraa x8, x16           ; provider->getDeviceMemoryWithIndex(w2)
   0x94489bc: 00 0b 00 f9   str  x0, [x24, #0x10]   ; map->memDescriptor = result
   ```

`[this+0x88]` is the `IOService* provider` handed to `start()`:

```
ApplePMGR::start  @ 0x94405cc
0x94405f0: f6 03 01 aa   mov  x22, x1
0x944062c: 76 46 00 f9   str  x22, [x19, #0x88]    ; this->provider = provider
```

The two dispatch slots `+0x700` / `+0x710` with signatures `() -> uint` and
`(uint, uint) -> object` match `IOService::getDeviceMemoryCount()` /
`IOService::getDeviceMemoryWithIndex(unsigned int)`, which are both imported by
this kext (`v=0` undefined entries in `syms_com_apple_driver_ApplePMGR.json`:
`__ZN9IOService20getDeviceMemoryCountEv`, `__ZN9IOService24getDeviceMemoryWithIndexEj`).

**Therefore the physical base is not an argument — it is `provider`'s DT `reg`
entry number `w2`, mapped by IOKit in `reg` order.** Three empirical checks that
this index is the `/arm-io/pmgr` `reg` index:

* `AppleT8140PMGR::initRegMaps` passes `w2 = 0,1,2,…,43` strictly increasing
  with no gaps, for exactly **44** calls, and `/arm-io/pmgr` is the **only**
  ADT node with exactly 44 `reg` entries (next largest: 33).
* Independent cross-check with `_findRegMap` (`0xfffffe00094505b4`), which
  searches all 137 maps for `die[+8] == w1` and returns the map id:
  ```
  0x94505f0: 08 08 40 b9   ldr  w8, [x0, #8]
  0x9450604: 1f 01 14 6b   cmp  w8, w20          ; die->[8] vs arg
  0x9450620: 76 02 00 b9   str  w22, [x19]       ; *out = map id
  ```
  i.e. the value held per map in `+8` is exactly `w2` of `initRegMap`.
* Every one of the 10 `initRegGroups` `(map, offset)` pairs satisfies
  `offset < size` of the `reg` entry selected by that map's index (§3.3). A
  wrong index-to-entry mapping would break these bounds immediately.

### 2.5 Die record layout (`0x30` bytes), for the driver author

| off | contents | evidence |
|---|---|---|
| `+0x00` | name/marker pointer, copied from `_regMaps[map][0]`; printed with `%-11s` | `0x944897c ldr x8,[x24]` / `0x9448980 str x8,[x21]`; format `0x757da5c` = `"ApplePMGR:   [Die %u] %-11s | %#llx -> %#llx\n"` |
| `+0x08` | `u32` = DT `reg` index (`w2`) | `0x94488c0 str w20,[x21,#8]` |
| `+0x10` | device-memory / memory-map object | `0x94489bc` (die 0), `0x94488fc` (die 1) |
| `+0x18` | **mapped kernel VA used for all register accesses** | `readReg32`: `0x9463cd0 ldr x8,[x0,#0x18]` then `0x9463cd4 ldr w0,[x8,w19,uxtw]` |
| `+0x20` | `u64` from `0xfffffe000bc20be4(desc)` (first `%#llx` of the print) | `0x9448934 bl 0xbc20be4`, `0x9448938 str x0,[x21,#0x20]` |
| `+0x28` | `u32` window size returned by `vtable+0x148`; **bound for register offsets** | `str w0,[x21,#0x28]` `0x9448978`; `getReMapSize` `0x9463a3c ldr w0,[x0,#0x28]` |

`ApplePMGR::getReMapSize` (`0xfffffe0009463a1c`) is `return getRegMap(map,0)->size32`, and
`readReg32` asserts `offset < getReMapSize(map)` (`0x9463cb8`).

Note the physical base is *not* kept as a 32-bit field: `+0x20` is a 64-bit value and
`+0x18` is the mapped VA. Two windows on this SoC are above 4 GiB in ADT space
alone (`0x1f1878000`, `0x270e1c000`; `0x4_0187_8000` and `0x4_80e1c000` after
the `/arm-io` `ranges` translation), which is itself a proof that no 32-bit
address is passed through `initRegMap`'s arguments.

---

## 3. (b) Every `initRegMap` call site

Whole-image scan for `bl` to `0xfffffe000944879c`
(`scripts/blscan.py`, 46 `bl` sites: 3 in `AppleBringUpPMGR::initRegMaps`, 43 in
`AppleT8140PMGR::initRegMaps`, whose 44th call is a tail `b` at
`0xfffffe00098372c8` found by disassembling that epilogue — 47 invocations in
total). Two functions own them:

* `AppleBringUpPMGR::initRegMaps` @ `0xfffffe00094802d4` — 3 calls (maps 0/1/2, all dies).
* `AppleT8140PMGR::initRegMaps` @ `0xfffffe0009836e84` — 44 calls (all windows, die 0).

No other callers exist; the 44-window table and the 3-map bring-up loop are the
complete set. Both symbols also appear as virtual methods in the kext method
tables (single occurrence of each address at `0xfffffe000c973338` /
`0xfffffe000c89c6d8`), and nothing calls them by `bl`, so they are dispatched
virtually from `ApplePMGR::initDriver` — see §6 (inference).

### 3.1 `AppleBringUpPMGR::initRegMaps` — 3 calls, looped over dies

```
0x9480314: mov w20, #0                    ; die
0x9480320: mov x0, x19 ; mov w1,#0 ; mov w2,#0 ; mov x3,x20 ; mov w4,#0
0x9480334: bl  initRegMap                ; map 0, res 0, die, optional
0x948033c: mov w1,#1 ; mov w2,#1 ; mov x3,x20 ; mov w4,#0
0x948034c: bl  initRegMap                ; map 1, res 1, die, optional
0x9480354: mov w1,#2 ; mov w2,#2 ; mov x3,x20 ; mov w4,#1
0x9480364: bl  initRegMap                ; map 2, res 2, die, REQUIRED
0x948036c: add w20, w20, #1
0x94803a0: blraa [vtable+0xb30]          ; dieCount
0x94803a8: cmp w20, w0 ; b.lo 0x9480320
```

| call VA | map | res idx | die | required |
|---|---|---|---|---|
| `0xfffffe0009480334` | `0x00` | `0x00` | loop `x20` | no |
| `0xfffffe000948034c` | `0x01` | `0x01` | loop `x20` | no |
| `0xfffffe0009480364` | `0x02` | `0x02` | loop `x20` | **yes** |

### 3.2 `AppleT8140PMGR::initRegMaps` — 44 calls, `die = 0`, all optional

`w4 = 0` on every call (never asserts on a missing window). All calls are
`mov w1,#map ; mov w2,#idx ; mov w3,#0 ; mov w4,#0 ; bl`. The last one is a tail
branch (`0x98372c8: 35 45 f0 17  b #0xfffffe000944879c`). Resolved base/size is
`/arm-io/pmgr` `reg[idx]` (property offset `idx*16`; `reg[36]` bytes =
`00 80 87 f1 01 00 00 00 00 40 00 00 00 00 00 00`).

| call VA | map id | res idx | window base | size |
|---|---|---|---|---|
| `0xfffffe0009836ea8` | `0x00` | `0x00` | `0xf0700000` | `0xcc000` |
| `0xfffffe0009836ec0` | `0x01` | `0x01` | `0xf8280000` | `0x80000` |
| `0xfffffe0009836ed8` | `0x5b` | `0x02` | `0xf0000000` | `0x78000` |
| `0xfffffe0009836ef0` | `0x5d` | `0x03` | `0xf8200000` | `0x7c000` |
| `0xfffffe0009836f08` | `0x42` | `0x04` | `0xf8004000` | `0x1000` |
| `0xfffffe0009836f20` | `0x09` | `0x05` | `0xe20000` | `0x2000` |
| `0xfffffe0009836f38` | `0x0d` | `0x06` | `0x50000` | `0xef1000` |
| `0xfffffe0009836f50` | `0x0a` | `0x07` | `0xe40000` | `0x9000` |
| `0xfffffe0009836f68` | `0x0b` | `0x08` | `0xe50000` | `0xe81000` |
| `0xfffffe0009836f80` | `0x0c` | `0x09` | `0xe70000` | `0x1000` |
| `0xfffffe0009836f98` | `0x0e` | `0x0a` | `0x50000` | `0x10000` |
| `0xfffffe0009836fb0` | `0x0f` | `0x0b` | `0x150000` | `0x10000` |
| `0xfffffe0009836fc8` | `0x10` | `0x0c` | `0x250000` | `0x10000` |
| `0xfffffe0009836fe0` | `0x11` | `0x0d` | `0x350000` | `0x10000` |
| `0xfffffe0009836ff8` | `0x15` | `0x0e` | `0x1e20000` | `0x2000` |
| `0xfffffe0009837010` | `0x19` | `0x0f` | `0x1050000` | `0xef1000` |
| `0xfffffe0009837028` | `0x16` | `0x10` | `0x1e40000` | `0xb000` |
| `0xfffffe0009837040` | `0x17` | `0x11` | `0x1e50000` | `0xe81000` |
| `0xfffffe0009837058` | `0x18` | `0x12` | `0x1e70000` | `0x1000` |
| `0xfffffe0009837070` | `0x1a` | `0x13` | `0x1050000` | `0x10000` |
| `0xfffffe0009837088` | `0x1b` | `0x14` | `0x1150000` | `0x10000` |
| `0xfffffe00098370a0` | `0x64` | `0x15` | `0xf0700000` | `0x18000` |
| `0xfffffe00098370b8` | `0x65` | `0x16` | `0xf0724000` | `0x4000` |
| `0xfffffe00098370d0` | `0x68` | `0x17` | `0xf0728000` | `0x4000` |
| `0xfffffe00098370e8` | `0x69` | `0x18` | `0xf0730000` | `0x14000` |
| `0xfffffe0009837100` | `0x67` | `0x19` | `0xf0760000` | `0x4000` |
| `0xfffffe0009837118` | `0x66` | `0x1a` | `0xf0764000` | `0x4000` |
| `0xfffffe0009837130` | `0x61` | `0x1b` | `0xf0768000` | `0x4000` |
| `0xfffffe0009837148` | `0x62` | `0x1c` | `0xf0780000` | `0x4000` |
| `0xfffffe0009837160` | `0x6d` | `0x1d` | `0xf82b8000` | `0x10000` |
| `0xfffffe0009837178` | `0x6c` | `0x1e` | `0xf82c8000` | `0x4000` |
| `0xfffffe0009837190` | `0x6e` | `0x1f` | `0xf82d0000` | `0x8000` |
| `0xfffffe00098371a8` | `0x4e` | `0x20` | `0xf0208000` | `0x8000` |
| `0xfffffe00098371c0` | `0x4d` | `0x21` | `0xf0280000` | `0x80000` |
| `0xfffffe00098371d8` | `0x08` | `0x22` | `0xf03c0000` | `0x30000` |
| `0xfffffe00098371f0` | `0x43` | `0x23` | `0xf8100000` | `0xa8000` |
| **`0xfffffe0009837208`** | **`0x71`** | **`0x24`** | **`0x1f1878000`** | **`0x4000`** |
| `0xfffffe0009837220` | `0x62` | `0x25` | `0xf0780000` | `0x4000` |
| `0xfffffe0009837238` | `0x51` | `0x26` | `0x1f195c000` | `0x4000` |
| `0xfffffe0009837250` | `0x52` | `0x27` | `0x1f1960000` | `0x4000` |
| `0xfffffe0009837268` | `0x53` | `0x28` | `0x1f1974000` | `0x4000` |
| `0xfffffe0009837280` | `0x20` | `0x29` | `0x1000000` | `0xff4000` |
| `0xfffffe0009837298` | `0x73` | `0x2a` | `0x270e1c000` | `0x4000` |
| `0xfffffe00098372c8` (tail `b`) | `0x81` | `0x2b` | `0xf2000000` | `0x1000000` |

Two notes:

* map `0x62` is initialised **twice** (idx `0x1c` and `0x25`); both `reg`
  entries are `0xf0780000/0x4000`, so the second call is idempotent.
* map `0x72` is **never** initialised by this table (see §4.4).

### 3.3 `initRegGroups` — the group → (map, offset) table

`ApplePMGR::initRegGroup(RegGroup, RegMap, uint offset)` @ `0xfffffe0009448a60`:

```
0x9448a64: mov  w8, #0x5d48
0x9448a68: add  x8, x0, x8            ; &regGroups[0]
0x9448a6c: mov  w9, #0x18
0x9448a70: umull x9, w1, w9           ; 0x18 per group
0x9448a88: mov  w8, #1
0x9448a8c: strb w8, [x10, #8]         ; group->valid = 1
0x9448a90: 42 8d 01 29   stp  w2, w3, [x10, #0xc]   ; +0xc = map id, +0x10 = offset
```

and the reader `ApplePMGR::getRegGroupOffset(RegGroup)` @ `0xfffffe0009463a78`:

```
0x9463a7c: cmp  w1, #0x25            ; 37 groups
0x9463a88: umaddl x8, w1, #0x18, x0
0x9463a8c: mov  w9, #0x5d58          ; 0x5d48 + 0x10
0x9463a94: ldr  w0, [x8]             ; return group->offset
```

`AppleT8140PMGR::initRegGroups` @ `0xfffffe00098372cc` (11 calls; last one at
`0x98373bc` is a tail `b`). Group `0x20` is the one at `0x983735c`:

```
0x983735c: 01 04 80 52   mov  w1, #0x20     ; group 0x20
0x9837360: 22 0e 80 52   mov  w2, #0x71     ; map 0x71
0x9837364: 03 00 82 52   mov  w3, #0x1000   ; offset 0x1000
0x9837368: be 45 f0 97   bl   0x9448a60
```

| group | map | offset | window base | size | `offset < size` |
|---|---|---|---|---|---|
| `0x02` | `0x62` | `0x0` | `0xf0780000` | `0x4000` | yes |
| `0x03` | `0x67` | `0x0` | `0xf0760000` | `0x4000` | yes |
| `0x08` | `0x69` | `0x4000` | `0xf0730000` | `0x14000` | yes |
| `0x09` | `0x69` | `0x8000` | `0xf0730000` | `0x14000` | yes |
| `0x16` | `0x6d` | `0x4000` | `0xf82b8000` | `0x10000` | yes |
| `0x17` | `0x5d` | `0x4009c` | `0xf8200000` | `0x7c000` | yes |
| **`0x20`** | **`0x71`** | **`0x1000`** | **`0x1f1878000`** | **`0x4000`** | **yes** |
| `0x22` | `0x81` | `0x800000` | `0xf2000000` | `0x1000000` | yes |
| `0x23` | `0x81` | `0x900000` | `0xf2000000` | `0x1000000` | yes |
| `0x24` | `0x81` | `0x100000` | `0xf2000000` | `0x1000000` | yes |

All ten fit. This is the strongest single consistency check available offline
that `w2` = `reg` index: an off-by-one in the index would push
group `0x09`'s `0x8000` past a `0x4000` window, and group `0x20`'s `0x1000`
would land outside most other candidates.

---

## 4. (c) Verdict on map id 0x71

### 4.1 It is initialised — once

`AppleT8140PMGR::initRegMaps`, call `0xfffffe0009837208`, arguments
`w1 = 0x71`, `w2 = 0x24 (36)`, `w3 = 0 (die 0)`, `w4 = 0 (optional)`.
Raw bytes `0x28331f8`: `21 0e 80 52 82 04 80 52 03 00 80 52 04 00 80 52 65 45 f0 97`
(`mov w1,#0x71; mov w2,#0x24; mov w3,#0; mov w4,#0; bl 0x944879c`).

Source of the base: **`/arm-io/pmgr` `reg` entry 36**, value
`0x1_f1878000`, size `0x4000` (ADT property `reg`, record 36 = bytes 576..591:
`00 80 87 f1 01 00 00 00 00 40 00 00 00 00 00 00` → `addr=0x1f1878000`,
`size=0x4000`).

**Address translation.** `/arm-io` `ranges` (7 × 24 B) begins with
`child 0x0 -> parent 0x210000000, size 0x2f0000000`, so every `/arm-io` child
address below `0x2f0000000` becomes physical `child + 0x210000000`. (Same
translation the working notes already used: ANE `reg[5] = 0xf82c8000` →
`0x3082c8000`.) Hence:

| | ADT (`reg` value) | physical (after `ranges`) |
|---|---|---|
| map `0x71` window | `0x1_f1878000` + `0x4000` | `0x4_0187_8000` + `0x4000` |
| group `0x20` base (`+0x1000`) | `0x1_f1879000` | `0x4_0187_9000` |
| clock registers (`+0x100`) | `0x1_f1879100` | `0x4_0187_9100` |
| power/gate registers (`+0x2c`) | `0x1_f187902c` | `0x4_0187_902c` |

### 4.2 Independent confirmation #1 — a dedicated accessor hard-codes map 0x71

`ApplePMGR::readANEClkGenReg` @ `0xfffffe000946540c`:

```
0x9465410: 89 63 8c 52   mov  w9, #0x631c
0x9465414: 09 00 09 8b   add  x9, x0, x9
0x9465418: 29 01 40 b9   ldr  w9, [x9]
0x946541c: 7f 00 09 6b   cmp  w3, w9                ; die check
0x9465438: mov  x17, #0x1050          ; virtual slot of readReg32
0x9465440: ldr  x9, [x16]
0x9465448: 41 c4 01 11   add  w1, w2, #0x71     ; <<< map = 0x71 + bank
0x9465450: mov  x2, x8                ; offset
0x9465460: mov  x17, x4
0x9465468: 11 0a 1f d7   braa x16, x17          ; readReg32(map=0x71+bank, offset, die)
```

`w2` is a bank selector: bank 0 → map `0x71` (`SNE_ANECLKGEN_TOP`), bank 1 →
map `0x72` (`SNE1_ANECLKGEN_TOP`, the string at `0x7578683`).

Siblings using the same mechanism, which is what makes this a *pattern* and not
a coincidence:

| accessor | VA | map | resolved window |
|---|---|---|---|
| `ApplePMGR::readMPMGRClkGenSOCReg` | `0x9465110` | `0x5c` | **not initialised on t8140** (§4.4) |
| `ApplePMGR::readPMGRClkGenAONReg` | `0x94651dc` | `0x5d` | `0xf8200000` (= group `0x17` base, AON clkgen) |
| `ApplePMGR::readGFXClkGenReg` | `0x94652ac` | `0x73` | `0x270e1c000` (inside `/arm-io/sgx` — the GPU) |
| `ApplePMGR::readANEClkGenReg` | `0x946540c` | `0x71`(+bank) | `0x1f1878000` (inside `/arm-io/ane`) |

### 4.3 Independent confirmation #2 — containment in the ANE's own ADT block

ADT `/arm-io/ane` `reg` has 6 windows; `reg[0] = 0x1f0000000`, size `0x2000000`
(32 MiB; physical `0x4_0100_0000`). Map `0x71`'s window `0x1f1878000 + 0x4000`
is **inside** it (`0x1f1878000 - 0x1f0000000 = 0x1878000 < 0x2000000`). The same
holds for the four other high ANE-window maps (`0x51 = 0x1f195c000`,
`0x52 = 0x1f1960000`, `0x53 = 0x1f1974000`). Meanwhile `/arm-io/ane`
`reg[5] = 0xf82c8000/0x4000` is map `0x6c` (pmgr `reg[30]`) — a *different*
window, and not the clock generator.

### 4.4 What is NOT initialised (relevant footguns)

* **map `0x72`** (`SNE1_ANECLKGEN_TOP`, bank 1 of `readANEClkGenReg`) appears in
  no `initRegMap` call. On an SNE1/dual-ANE part, a bank-1 clock access would
  hit the zero-initialised record (`+0x18 == 0`) and fault.
* **map `0x5c`** (`readMPMGRClkGenSOCReg`) likewise. The SOC clock generator on
  t8140 is reached through group `0x02 -> map 0x62` instead, i.e. the legacy
  accessor is dead code on this SoC.

### 4.5 Discrepancy with the assumed `0x3082c9100`

The working assumption put the ANE clock register at `0x3_082c_9100`
(= `0xf82c8000 + 0x1100`, i.e. "the ANE `reg[5]` window plus the clkgen
offset"). The arithmetic there is right (the `+0x210000000` translation is
correct, and `0x3082c8000` is exactly what pmgr `reg[30]` translates to), but
**it is applied to the wrong window**. The clkgen offset `0x1100` belongs to
map `0x71`:

```
map 0x6c  (pmgr reg[30]) = 0xf82c8000 -> phys 0x3_082c_8000   ANE CSR aperture, 0x4000
map 0x71  (pmgr reg[36]) = 0x1f1878000 -> phys 0x4_0187_8000  ANE clkgen,      0x4000
        + group 0x20 offset 0x1000 -> 0x4_0187_9000
        + clock base      0x100    -> 0x4_0187_9100   <- SNE_ANECLKGEN_TOP registers
```

so the ANE clock generator is `0x4_0187_8000`, not `0x3_082c_8000`.

*Inference (not proven here):* the two are separate apertures into different
sub-blocks of the ANE complex; whether the clkgen page is additionally visible
through the ANE CSR aperture is not something a static read can settle.

---

## 5. (d) The ordinal `n` in `_printClocks` and the `devices` blob

### 5.1 What `_printClocks` actually reads

`ApplePMGR::_printClocks` @ `0xfffffe0009465fc4`. Loop body (record pointer `x27`):

```
0x94660fc: 7c 4e 72 f9   ldr  x28, [x19, #0x6498]    ; clock record table
0x9466100: 9b 03 17 8b   add  x27, x28, x23          ; x27 = &records[i]
0x9466104: 68 0b 40 39   ldrb w8, [x27, #2]          ; record byte 2, bit0 = "enabled"
0x9466120: 69 07 40 39   ldrb w9, [x27, #1]          ; record byte 1 = GROUP index
0x9466124: 08 11 09 8b   add  x8, x8, x9, lsl #4     ; group table stride 0x10
0x9466128: 01 01 40 b9   ldr  w1, [x8]                ; group->[0] = map id
0x9466134: bl   getRegMap(this, w1, #0)               ; die 0
0x9466144: 09 3d 40 39   ldrb w9, [x8, #0xf]          ; group->[0xf] flags
0x946614c: 0a 35 40 39   ldrb w10, [x8, #0xd]         ; group->[0xd] type
0x9466154: 09 05 40 b9   ldr  w9, [x8, #4]            ; group->[4] = base offset
0x9466158: 68 03 40 39   ldrb w8, [x27]               ; record byte 0 = ORDINAL n
0x9466194: 29 01 04 11   add  w9, w9, #0x100          ; clock register base
0x94661a0: 5f 0d 00 71   cmp  w10, #3
0x94661a4: 0a 04 80 52   mov  w10, #0x20              ; stride 0x20
0x94661a8: 4a 01 8b 1a   csel w10, w10, w11, eq        ; else 0x10 / 0x1c
0x94661b0: 5a 25 08 1b   madd w26, w10, w8, w9        ; reg = stride*n + base
```

* **`[x27+1]` = reg-group index** (0…36, stride `0x10` into `[this+0x60f8]`),
  **not** the ordinal.
* **`[x27+0]` = the ordinal `n`** that multiplies the stride. This is the byte
  the task description was looking for, at the wrong offset.
* stride ∈ {`0x10`, `0x1c`, `0x20`} chosen from `group[0xd] & 0xff` (== 2 →
  `0x1c`, == 3 → `0x20`, else `0x10`), with `group[0xf]` bit 1 selecting the
  `+0x100` clock-register form vs the `+0x2c` power-register form.

So for group `0x20` (map `0x71`, base offset `0x1000`):
`n=0` → clock reg `0x1f1878000+0x1100 = 0x1f1879100`
(= physical `0x4_0187_9100`), `n=1` → `+stride`, etc.

### 5.2 Where those two bytes come from

`ApplePMGR::_initPerfCountersV1` @ `0xfffffe0009451c54`:

```
0x9451cac: adrp x1, #0xfffffe0007583000 ; add x1, x1, #0x47f   -> "perf-regs"
0x9451cc0: blraa [provider vtable+0x2d8]                       ; getProperty("perf-regs")
0x9451d04: blraa [prop vtable+0x160]                           ; getLength()
0x9451d08: 08 7c 04 53   lsr  w8, w0, #4                       ; count = len/16
0x9451d24: 40 7f 30 f9   str  x0, [x26, #0x60f8]               ; group table = alloc(16*count)
0x9451d6c: bl   memcpy                                         ; copy property verbatim
0x9451d88: 41 00 40 b9   ldr  w1, [x2]                         ; record[0]
0x9451d90: bl   _findRegMap(this, record[0], record)           ; rewrite [0] := map id
...
0x9451e00: adrp x1, #0xfffffe0007583000 ; add x1, x1, #0x4df   -> "clocks"
0x9451e78: 40 4f 32 f9   str  x0, [x26, #0x6498]               ; clock record table
```

* The group table is a **verbatim copy of the DT property `perf-regs`**, 16-byte
  records, with field 0 rewritten from "device-memory resource index" to
  "map id" by `_findRegMap`, field `+4` used as the base offset, `+0xd` type,
  `+0xf` flags.
* The clock table is the DT property **`clocks`**; each record is at least
  4 bytes: `+0` ordinal `n`, `+1` group index, `+2` flags (bit0 = enabled),
  `+3` printed as a suffix.

**Neither property exists anywhere in the J700 ADT.** A full-tree scan of all
3 497 dumped nodes found no `perf-regs` and no `clocks` property. Consequently,
on this machine the `_printClocks` path is never populated (and `_printClocks`
is debug-gated anyway: `0x9465fc8 ldrb w8,[x0,#0x108]; tbz w8,#0` returns
immediately unless the debug flag is set).

### 5.3 The `devices` blob

`/arm-io/pmgr` `devices`, 14 208 bytes = **296 records × 48 bytes**:

| off | contents |
|---|---|
| `+0x00` | u32 type/flags (e.g. `0x00000010`, `0x00020000`, `0x06080012`) |
| `+0x04` | u32 (e.g. `0x3c`, `0x2f`, `0`) |
| `+0x08` | 0 |
| `+0x0c` | u32 (`0` or `0x80000000`, `0x10000000`) |
| `+0x10` | u32 offset-like field (`0x290`, `0xc000`, mostly `0`) |
| `+0x14` | 0 |
| `+0x18` | u32 = `deviceId << 16` (the id the ADT `clock-gates`/`power-gates` arrays reference) |
| `+0x1c` | 0 |
| `+0x20` | char `name[16]` |

ANE gate records (ids from `/arm-io/ane` `clock-gates` / `power-gates` =
`0x123` = 291 and `0x152` = 338):

| rec | id | `+0x00` | `+0x04` | `+0x0c` | `+0x10` | `+0x18` | name |
|---|---|---|---|---|---|---|---|
| 267 | 291 (`0x123`) | `0x00000010` | `0x0000003c` | `0` | `0` | `0x01230000` | `ANE-SYS-V` |
| 290 | 338 (`0x152`) | `0x06080012` | `0` | `0x80000000` | `0` | `0x01520000` | `ANE-SYS-V-PMP` |

### 5.4 Verdict on `n`

**Not derivable from the J700 ADT.** Specifically:

* Neither `[x27]` nor `[x27+1]` comes from the `devices` blob — they come from
  the absent `clocks` / `perf-regs` properties.
* For the two ANE gate devices the only offset-like blob field (`+0x10`) is
  **0** in both records, so even under the hypothesis "`n` = blob `+0x10`" the
  answer would be 0 for both, and that hypothesis is unsupported.
* The blob's record index (267 / 290) is a global array position, not a
  per-block ordinal, and record ids are only 16-bit-deep `id<<16` markers.

What would settle it — in order of cost:

1. Dump `perf-regs` and `clocks` from an ADT **that has them** (they are present
   on T8103-generation PMGR nodes; absent here), then read the `clocks` record
   whose name is `SNE_ANECLKGEN_TOP` (`n = byte 0`, group = byte 1).
2. Read the live `ApplePMGR` object under a macOS kernel (LLDB/`paniclog`):
   `[pmgr+0x6498]` table, count at `[pmgr+0x64a0]`.
3. Disassemble the *non-debug* ANE clock path that actually runs on t8140
   (`ApplePMGRFunctionClockGate::callFunction` @ `0x948bdf8` →
   `ApplePMGR::_enableDeviceAutoClockGating` / `ApplePMGRNub::enableDeviceAutoClockGatingEjb`
   @ `0x9491510`), which consumes the `initRegGroups` table rather than
   `perf-regs`.

Until one of those is done, treating `n = 0` for the ANE clkgen (`n` becomes 0
when the ANE is the only consumer of the block) is an **assumption**, and the
`+0x100`/`+0x2c` and stride set are inherited from base-class code that is
populated only from the missing `perf-regs` property.

---

## 6. Statement of method and limits

* **Proven by instruction stream**: `initRegMap` signature and argument roles
  (§2), the 46 call sites and their literal arguments (§3.1–3.2), the 11
  `initRegGroups` triples (§3.3), map `0x71 → reg[36] → 0x1f1878000/0x4000`
  (§4.1), `readANEClkGenReg` using map `0x71+bank` (§4.2), the
  `_printClocks` field usage `[x27]=n`, `[x27+1]=group` (§5.1), and the DT
  origin of both tables (§5.2).
* **Proven by data**: the `reg` and `devices` blobs, `perf-regs`/`clocks`
  absence, the `reg[36]` value bytes, the group-offset bounds consistency
  (§3.3), the ANE/SGX containment checks (§4.3).
* **Inference, labelled**: that `initRegMaps`/`initRegGroups` actually run at
  boot on this machine (both are virtual methods, listed once each in the kext
  method tables, with no direct `bl` caller — so dispatch is through the
  `[obj]` table whose entries are not resolvable to raw pointers in this
  image), and that the clock-register formula `base + 0x100 + stride*n`
  applies to the t8140 ANE block given that its input property is absent here.
* **Not derivable offline**: the ordinal `n` for `SNE_ANECLKGEN_TOP` on J700
  (§5.4), the kernel VA / mapping created for `0x1f1878000`, and whether the
  ANE CSR aperture aliases any part of the clkgen page.

Nothing here is device qualification. Register tables and code reading are not
device behaviour.

---

## 7. Explicit not-founds

| searched | result |
|---|---|
| `initRegMap` callers outside `ApplePMGR`/`AppleT8140PMGR` | none (46 `bl` + 1 tail `b`, all in the two `initRegMaps`) |
| `initRegMap` with a literal physical address argument | none — no address is ever passed |
| map `0x72` initialisation | **not found** — `SNE1_ANECLKGEN_TOP` bank has no window |
| map `0x5c` initialisation | **not found** — legacy `readMPMGRClkGenSOCReg` accessor is dead on t8140 |
| ADT property `perf-regs` anywhere in the tree | **not found** |
| ADT property `clocks` anywhere in the tree | **not found** |
| ordinal `n` for ids 291/338 in the `devices` blob | **not found** (`+0x10` is 0 for both) |
| direct `bl` to `AppleT8140PMGR::initRegMaps` / `initRegGroups` | none — virtual dispatch only |
| callers of `readANEClkGenReg`/`writeANEClkGenReg` by `bl` | none — virtual dispatch only |

## 8. Files

```
receipts/2026-11-04-j700-h17-regmap-71/
├── README.md          this report
└── scripts/
    ├── kdis.py        linear disassembly at a VA (VA = 0xfffffe0007004000 + off)
    ├── blscan.py      whole-image scan for bl/b targeting a VA
    ├── calls.py       recovers the literal w1..w4 for each initRegMap call
    └── strs.py        string-at-VA helper
```
