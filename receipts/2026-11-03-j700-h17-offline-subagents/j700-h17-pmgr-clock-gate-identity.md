# J700 / T8140 (H17): the PMGR clock-gate identity behind ANE clock ids 325/326/327 and gate ids 291/338

Offline analysis only: ADT image + retained kernelcache. No device access, no module
load, no MMIO. Everything below is either a decoded ADT property, a decoded static table,
or a compiled instruction offset. None of it is device qualification.

## 0. Direct answer

**The exact (register address, bit) pair for `enableDeviceClock(1, index)` on J700 is
not derivable from the retained artifacts, and the question as posed contains a wrong
premise.**

1. **Clock ids 325/326/327 (`0x145`/`0x146`/`0x147`) are never used in the gate write
   path.** `AppleARMIODevice::enableDeviceClock(bool, u32 index)` does **not** index
   `clock-ids`; it indexes **`clock-gates`**. Proof (disassembly, §2): the array it reads
   is the one `initDeviceWithProvider` filled from the `clock-gates` property, and the
   count it bounds-checks is the `clock-gates` count. On `/arm-io/ane` those are
   `clock-ids = <0x147 0x146 0x145>` but `clock-gates = power-gates = <0x123 0x152>`.
   So the bring-up votes **291 (`0x123`) and 338 (`0x152`)** and nothing else; id 325 is
   dead on this path. `clock-ids` feed the AppleARMIO frequency/name tables, not the gate.
2. **291 and 338 are PMGR *device IDs*, not bit numbers.** `/arm-io/pmgr`'s `devices`
   property (296 × 48-byte records, decoded in §3) contains deviceID `0x123` = `"ANE-SYS-V"`
   and deviceID `0x152` = `"ANE-SYS-V-PMP"`, both virtual devices parented to
   `ANE_SYS` (deviceID `0x3c`). `0x3c` is corroborated independently by the `hw-dpe-reg`
   ANE energy record (`deviceID 0x3c`, addr `0x4_019780E0`).
3. **The register that gets written is a PMGR power-state ("PS") / pwr-gate register,
   not an ANE register**, and its offset is computed at runtime from per-SoC tables that
   macOS builds from ADT properties **which do not exist in this ADT**
   (`pwrgate-regs`, `clocks`, `perf-regs` are all absent; verifiable, §5). The register
   *file* is identified — PMGR physical base **`0x3_0070_0000`** (window 0, size `0xcc000`),
   with the ANE's own shared slices at `0x3_0070_0000…+0x18000` and `0x3_0072_4000` — but
   the group base + index inside it is not recoverable statically.
4. `clock-frequencies-regs` is **not** the missing table: it cannot be id-indexed
   arithmetically (1408 B cannot hold an entry for id 325 at ≥8 B/entry) and the string
   `clock-frequencies-regs` **does not occur anywhere in the kernelcache** — macOS never
   looks it up.

What is required to finish this is listed in §6.

## 1. Evidence identities

| Item | Value |
| --- | --- |
| ADT | `/var/tmp/opencode/adt.bin`, 425 984 B, parsed 0x649d0 B, 325 nodes / 4985 props |
| Parser | `/home/admin/linux-aurora/tools/aurora-sep/aurora-adt-extract.py` (`parse_adt`, `walk`) |
| Kernelcache | `/var/tmp/neo/kernelcache.bin`, MH_FILESET, 366 entries, 114 688 000 B |
| `com.apple.driver.AppleARMPlatform` (defines `AppleARMIO`/`AppleARMIODevice`) | entry VA `0xfffffe000715d410`, file `0x159410`; `__text` VA `0xfffffe000895d900`, file `0x1959900`, size `0x533c4` |
| `com.apple.driver.AppleT8140` (defines `AppleH17PPlatformIO`) | entry VA `0xfffffe000761b5b0`, file `0x6175b0`; `__text` VA `0xfffffe0009790950`, file `0x278c950`, size `0x86e8` |
| `com.apple.driver.ApplePMGR` | entry VA `0xfffffe00075771e0`, file `0x5731e0`; `__text` VA `0xfffffe000943ff90`, file `0x243bf90` |
| `com.apple.driver.AppleT8140PMGR` | entry VA `0xfffffe0007632710`, file `0x62e710`; `__text` VA `0xfffffe0009836b80`, file `0x2832b80` |
| VA↔file (kernel-wide, `__TEXT_EXEC`) | `VA = 0xfffffe0007004000 + file_off` |
| VA↔file (chained-fixup targets) | target field = low 32 bits, same identity as above |

## 2. Task 2: how a DT gate id becomes an MMIO write (exact addresses)

### 2.1 `AppleARMIODevice::initDeviceWithProvider` — where the four arrays come from
`0xfffffe000896a8c0` … `0xfffffe000896af10`. Each block is: create/lookup `OSSymbol`
from a literal (`IOARMIOVtable+0x2d8`), `getProperty` on the device node
(`bl 0xfffffe000bb29ac8`), `OSData::getLength` (vtable `+0x160`), `OSData::getBytes`
(vtable `+0x198`). The literal pointer in each block identifies the property:

| Property VA | String | count field | data field |
| --- | --- | --- | --- |
| `0xfffffe0007162d3f` | `clock-ids` | `this+0x90` | `this+0x98` |
| `0xfffffe0007162d49` | `clock-gates` | **`this+0xa0`** | **`this+0xa8`** |
| `0xfffffe0007162d55` | `service-gates` | `this+0xd8` | `this+0xd0` |
| `0xfffffe0007162d63` | `service-reset` | `this+0xe8` | `this+0xe0` |
| `0xfffffe0007162d71` | (var-len blob) | `this+0xf8` | `this+0xf0` (copy) |
| `0xfffffe0007162d88` | `power-gates` | **`this+0xb0`** | **`this+0xb8`** |
| `0xfffffe0007162d94` | `pin-groups` | `this+0xc0` | `this+0xc8` |

Store instructions: `str w8,[x20,#0xa0]` at `0x896aa80`, `str x0,[x20,#0xa8]` at
`0x896aabc` (clock-gates); `str w8,[x20,#0x90]` at `0x896a9bc`, `str x0,[x20,#0x98]` at
`0x896a9f8` (clock-ids); `str w8,[x20,#0xb0]` at `0x896ada4`, `str x0,[x20,#0xb8]` at
`0x896ade0` (power-gates).

### 2.2 `AppleARMIODevice::enableDeviceClock(bool enable, u32 index)`
`0xfffffe000896a304`:

```
896a308  ldr  w8,[x0,#0xa0]        ; clock-gates count
896a30c  cmp  w2,w8 ; b.hs -> 0xe00002c7 (kIOReturnUnsupported)
896a394  ldr  x8,[x20,#0xa8]       ; clock-gates[]
896a398  ubfiz x9,x21,#2,#0x20     ; index*4  (u32 entries)
896a3b4  ldr  w1,[x10]             ; w1 = clock_gates[index]      <-- the gate id
896a3b0  ldr  x0,[x20,#0x88]       ; provider (AppleH17PPlatformIO)
896a3c8  mov  x17,#0x8a8
896a3d0  ldr  x8,[x16]             ; provider vtable slot 0x8a8
896a3dc  mov  x2,x19               ; x2 = enable
896a404  braa x16                  ; provider->enableDeviceClock(id, enable)
```

`AppleARMIODevice::enableDevicePower(bool enable, u32* out, u32 index)`
`0xfffffe000896a414`: identical shape, `this+0xb0` / `this+0xb8` (**power-gates**),
provider slot **`0x8b0`**, args `(id, enable, out)` (`0x896a4c8`…`0x896a520`).

So the index used by `AppleANEHWDevice::EnableANEClocksAndPower` (`this+0x380`, driver VA
`0x8f2a060`/`0x8f2a0a8`) selects `clock_gates[index]` and `power_gates[index]`. With two
gates and the documented "index+1 for the second vote" (`pmp-virt-notify`), index = 0 then
1 → **291 first, 338 second**. (Inference on the value of `this+0x380`; the array index
arithmetic and the property identity are compiled-fact.)

### 2.3 Provider: `AppleH17PPlatformIO` (concrete `AppleARMIODevice` for H17P)
`com.apple.driver.AppleT8140`:

* `AppleH17PPlatformIO::start` `0xfffffe0009790bf8` builds the two function objects from
  the DT function properties: `[self+0xf0]` = `function-clock_gate` (`<0x69 "clkG">`),
  `[self+0xf8]` = `function-power_gate` (`<0x69 "pwrG">`), via
  `[[self+0xd8] vtable+0x1e8]` with `x1=[self+0xe0]` (the OSSymbol), then
  `vtable+0x160`/`vtable+0x198`/`vtable+0x198` as above for the blob form
  (`0x97912e4`…`0x97913e0` region — see §2.4 for the byte layout of the two trampolines).
* `AppleH17PPlatformIO::enableDeviceClock(u32 id, u32 enable)` `0xfffffe0009791310`:
  gate on `this[0xec] bit0`; then tail-call `[[self+0xf0] vtable+0x140]` with
  `w1 = id`, `w2 = enable`, `x3 = 0` (`0x979138c`…`0x97913e0`).
* `AppleH17PPlatformIO::enableDevicePower(u32 id, u32 enable, u32* out)`
  `0xfffffe00097913e4`: same with `[self+0xf8]`, `w1 = id`, `w2 = enable`, `x3 = out`
  (`0x9791464`…`0x97914b8`).

Phandle `0x69` = `/arm-io/pmgr` (`AAPL,phandle = 0x69`), so the named functions are
`ApplePMGRFunctionClockGate` and `ApplePMGRFunctionPowerGate` in `com.apple.driver.ApplePMGR`:

* `__ZN26ApplePMGRFunctionClockGate12callFunctionEPvS0_S0_` `0xfffffe00094864f4`
* `__ZN26ApplePMGRFunctionPowerGate12callFunctionEPvS0_S0_` `0xfffffe00094869ec`
  (the latter is 4 instructions: `mov x2,x3`; `ldr x0,[x0,#0x28]`;
  `ldr x8,[vptr+0x968]`; `blraa`; `mov w0,#0`; `retab`).

### 2.4 What is *not* resolvable here (explicit)
`ApplePMGRFunctionClockGate::callFunction` dispatches six virtuals on the object at
`[self+0x28]`: slots `0xb68`, `0xb70`, `0xb78`, `0xb30`, and then `0x948` with
`(id & 0x0fffffff, enable & 1, (enable>>1)&1, id>>28)` (`0x948667c`…`0x9486694`).
I resolved the vtables `__ZTV9ApplePMGR` `0xfffffe0008060c30`, `__ZTV11ApplePMGRv2`
`0xfffffe0008067700`, `__ZTV13ApplePMGRBase` `0xfffffe000806d2f8`,
`__ZTV16AppleBringUpPMGR` `0xfffffe00080664e0`, `__ZTV12ApplePMGRNub`
`0xfffffe000806e160` — **every one gives `getProfileFlags` at `0xb68`, `isDebugAllowed` at
`0xb78`, `_getPTDRangeInfo` at `0x948`, `_assertPSDReset` at `0x968`.** Those signatures do
not match the call shapes (`0xb78` returns an object that is then dereferenced; `0x948`
takes 4 scalars). **Conclusion: the class of `[self+0x28]` is not one of the ApplePMGR
classes whose `__ZTV` is in this kernelcache, at least not with the vtable base and
offset convention I used, so the actual MMIO performed for a clock-gate id stays
unresolved.** This is a real gap, not a stylistic one: without it the `id → (group,
index)` step is opaque.

## 3. Task 1 + 3: the ADT clock blobs and the static tables

### 3.1 `/arm-io` clock blobs: **not** id-indexed
| Property | size | factorisations |
| --- | --- | --- |
| `clock-frequencies` | 704 (0x2c0) | 44×16, 88×8, 176×4 |
| `clock-frequencies-nclk` | 704 | same |
| `clock-frequencies-regs` | 1408 (0x580) | 44×32, 88×16, 176×8, 352×4 |

Arithmetic bound: a dense table indexed by clock id needs ≥ 328 entries to reach id 325.
1408/328 = 4.3 B/entry — too small for `{register, bit}`. So `clock-frequencies-regs`
**cannot** be an id-indexed `id → register+bit` table. It is only id-indexed if ids are
*rebased* (`index = id - base`), and I found no base field in it.

Record-size evidence (all strides tried on the real bytes):

* **44×32 (per-clock record, 4 words)** — first record is
  `00366e01 000000cc 00800000 0000cc00`. Under a 2-register/2-cell reading this is
  `{0x016e3600 = 24 MHz, 0xcc000000, 0x8000, 0xcc000000}`: one plausible frequency, three
  values with no monotonic or id-like structure.
* **88×16 / 176×8** — the only *self-consistent* structure in the whole blob is a run of
  8-byte records at offset `0x0070`: `{u32 A, u32 B}` with `A` stepping by exactly 4
  (`0x08240000 … 0x0824009c`, then `0x08240304 … 0x08240374`, then
  `0x08250000`/`0x08252000`/`0x08256000`/`0x0825a000`/`0x0825e000`/`0x08262000`,
  then `0x00040000 … 0x00040170` stepping by 4) and `B` constant over long runs
  (`0xd1000003`, then `0xd5000003`, `0xd9000003`, `0xd0000003`, `0xd4000003`).
  That is a **write script** (address/offset + payload), not a sparse id map: no field
  ever takes the value `0x145`/`0x146`/`0x147` as an index and the `A` field is an
  address-like stride-4 sequence.
* Verified by scan: **no u32 in any of the three blobs — 4-byte aligned or not — equals
  `0x145`, `0x146`, `0x147`, `0x123` or `0x152`.** So there is no id field in them at all,
  aligned or packed.

**Therefore: none of the three is the id → register table; the `…-regs` blob is a
register-programming script.** (This also matches §5: nothing in macOS reads it.)

### 3.2 Static ClockData table in `__DATA_CONST`/`__const`: **none found**
* `AppleT8140PMGR.__const` (file `0x114a768`, `0x14f0` B): contains **no strings at all**
  (regex over the whole region → 0 matches) and only vtable-style pointers into
  `0xfffffe0009836b80…0xfffffe000983ed34`.
* `AppleT8140PMGR.__data` (file `0x5461538`, `0x288` B): kext personality/version blob
  (`com.apple.driver.ApplePMGR`, `"1.0.1d1"`), not a register table.
* `ApplePMGR.__const` (file `0x105bfa8`, `0xf8b0` B) holds the vtables, `_schemas`,
  `ApplePMGRFunction*_ktv`, `kalloc_type_view`s — no 24-byte `ClockData` array.
* The `ClockData` array is **runtime-allocated**: `_clocks` at `this+0x6498`, count at
  `this+0x64a0`, filled only by
  `ApplePMGR::_initPerfCountersV1(IOService*)` `0xfffffe0009451c54`, which reads
  properties **`"perf-regs"`** (`0xfffffe000758347f`, read at `0x9451cac`) and
  **`"clocks"`** (`0xfffffe00075834df`, read at `0x9451e04`) *from its provider*.
  Neither property exists in this ADT (§5), so on J700 that table is never built.
  `ClockData` layout recovered from `_printClocks` `0xfffffe0009465fc4` (stride 0x18):
  `+0x00 u8 index`, `+0x01 u8 regGroup`, `+0x02 u8 flags`, `+0x03 u8`, `+0x04 u32 id`,
  `+0x08 char name[16]`.
* The register *groups* are also runtime: `ApplePMGR::initRegGroup(RegGroup, RegMap, u32)`
  `0xfffffe0009448a60` fills a 16-byte entry table at `this+0x60f8` (count `this+0x6100`),
  and `getRegMap(RegMap, u32 die)` `0xfffffe0009454b08` reads a per-`RegMap`/per-die table
  at `this+0x29e8` (index `die*0x30 + map*0x60`, guard `map < 0x89`), whose entries hold
  `[+8] = window id`, `[+0x10] = mapped base VA`.

Since I can dump every static table in the binary, "no static clock-gate table exists in
either kext" is a positive, checked statement — the tables are all DT-derived.

### 3.3 The one table that *does* answer the id question: `/arm-io/pmgr` `devices`
14208 B = **296 records × 48 B**. Field `+0x18..+0x1b` (u32, high half = deviceID) plus
`char name[16]` at `+0x20`. Decoded entries for our ids (all from the ADT, so this is a
fact, not inference):

| index | `+0x00` | `+0x04` | `+0x08` | `+0x0c` | `+0x10` | `+0x14` | `+0x18` | name |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 59 | `00000080` | `0000002f` | `0` | `0` | `0x00000290` | `0` | `0x003c0000` | `ANE_SYS` (deviceID **0x3c**) |
| 128 | `00000010` | `0000003c` | `0` | `0` | `0x0000c000` | `0` | `0x00810000` | `ANE_CPU` (deviceID 0x81, parent 0x3c) |
| 267 | `00000010` | `0000003c` | `0` | `0` | `0` | `0` | `0x01230000` | **`ANE-SYS-V`** (deviceID **0x123** = 291) |
| 290 | `06080012` | `0` | `0` | `0x80000000` | `0` | `0` | `0x01520000` | **`ANE-SYS-V-PMP`** (deviceID **0x152** = 338) |

So gate 291 = virtual device `ANE-SYS-V`, gate 338 = virtual device `ANE-SYS-V-PMP`,
both parented (`+0x04` low byte) to `ANE_SYS` `0x3c`; `ANE_SYS` itself carries power-domain
byte `0x2f`, shared with `DISP_SYS` (index 57) — the "media/display+ANE" domain.
Corroboration for `0x3c`: `hw-dpe-reg`'s single ANE energy record is
`addr 0x4_019780E0, deviceID 0x3c, dieMask 1, readOnly 1, regWidth 0x40`.

Note the clock ids are **not** in this table: no record has deviceID `0x145`/`0x146`/`0x147`
(max deviceID present is `0x157`), confirming they live in the separate clock namespace.

### 3.4 The ANE clock-generator register group (closest thing to "ANE clock block")
`ApplePMGR` has ANE-specific accessors that are *not* the `clkG` path but name the ANE
clock block precisely:

* `ApplePMGR::readANEClkGenReg(u32 idx, u32 off, u32 die)` `0xfffffe000946540c` →
  `RegGroup = 0x71 + idx`, `offset = off`, tail-call `vtable+0x1050`
  (`add w1,w2,#0x71` at `0x9465448`).
* `writeANEClkGenReg(u32 val, u32 off, u32 idx, u32 die)` `0xfffffe000946547c` →
  same group, `vtable+0x1058` (`add w1,w3,#0x71` at `0x94654bc`).
* `waitANEClkGenReg(...)` `0xfffffe00094654f0` → `vtable+0x1060`.
* GFX uses fixed `RegGroup 0x73` (`0x94652ec`), so 0x71/0x72 are the two ANE clockgens.
* PMGR `__cstring` names the corresponding register groups:
  `SNE_ANECLKGEN_TOP`, `SNE1_ANECLKGEN_TOP`, `GRAPHICS_AGXBMPRCLKGEN_TOP`,
  `PMGRCLKGEN_SOC_TOP`, `PMS_MPMGRCLKGEN_SOC`, `PMGRCLKGEN_AON_TOP`,
  `PMS_PMGRCLKGEN_SOUTH`, `PMGRCLKGEN_SOC_TOP_DVFMCOP`, `PMGR_SOC_PWRGATE`.
* `AppleT8140PMGR::initRegGroups` `0xfffffe00098372cc` (10 `initRegGroup` calls, fully
  dumped) contains `initRegGroup(group=0x20, map=0x71, offset=0x1000)` — i.e. an ANE
  clockgen group at **`RegMap 0x71 + 0x1000`** — plus
  `(2,0x62,0) (3,0x67,0) (8,0x69,0x4000) (9,0x69,0x8000) (0x16,0x6d,0x4000)
  (0x17,0x5d,0x49c) (0x22,0x81,0x800000) (0x23,0x81,0x900000) (0x24,0x81,0x100000)`.
  The base class `AppleBringUpPMGR::initRegGroups` `0xfffffe00094803bc` instead reads
  three DT properties — `cluster-ctl-offset` (`0x7583eed`), `misc-cores-offset`
  (`0x7583f00`), `misc-acg-offset` (`0x7583f9b`) — none of which exist in this ADT.

## 4. Task 4: the PMGR register block on T8140

`/arm-io/ranges` gives the only translation: **child → physical is `+0x2_1000_0000`** for
the whole `[0, 0x2_f000_0000)` child range (entry 0: child `0x0` → phys `0x210000000`,
size `0x2f0000000`); the remaining six ranges are identity mappings.

`/arm-io/pmgr` `reg` = 44 windows (`#address-cells=2`, `#size-cells=2`). The ones that
matter:

| # | child | phys | size |
| --- | --- | --- | --- |
| 0 | `0x00f0700000` | **`0x0300700000`** | `0x0cc000` |
| 1 | `0x00f8280000` | `0x0308280000` | `0x080000` |
| 2 | `0x00f0000000` | `0x0300000000` | `0x078000` |
| 3 | `0x00f8200000` | `0x0308200000` | `0x07c000` |
| 4 | `0x00f8004000` | `0x0308004000` | `0x001000` |
| 21 | `0x00f0700000` | `0x0300700000` | `0x018000` |
| 22 | `0x00f0724000` | `0x0300724000` | `0x004000` |
| 30 | `0x00f82c8000` | `0x03082c8000` | `0x004000` |
| 34 | `0x00f03c0000` | `0x03003c0000` | `0x030000` |
| 41 | `0x0001000000` | `0x0211000000` | `0xff4000` |

`/arm-io/ane` `reg` = 6 windows, and **three of them are slices of the PMGR block**:

| # | phys | size | matches pmgr window |
| --- | --- | --- | --- |
| 0 | **`0x0400000000`** | `0x2000000` | ANE engine aperture |
| 1 | `0x0300700000` | `0x018000` | pmgr[21] |
| 2 | `0x0300724000` | `0x004000` | pmgr[22] |
| 3 | `0x03003c0000` | `0x030000` | pmgr[34] |
| 4 | `0x0211000000` | `0xff4000` | pmgr[41] |
| 5 | `0x03082c8000` | `0x004000` | pmgr[30] |

The engine aperture is independently confirmed: the `hw-dpe-reg` ANE energy counter is
`0x4_019780E0` = `0x4_0000_0000 + 0x19780E0`, and pmgr windows 36/38/39/40 are
`0x4_0187_8000`, `0x4_0195_c000`, `0x4_0196_0000`, `0x4_0197_4000` — exactly the
`0x1878000`/`0x195c000`/`0x1960000`/`0x1974000` blocks the HAL touches.

**Conclusion for the address:** the register file to write is
**`0x3_0070_0000` (PMGR, 0xcc000 bytes)** and the ANE-shared slices are
`+0x00000…0x18000` and `+0x24000…0x28000` of it. The off-chip-visible structure of a gate
register, from `_printClocks` `0xfffffe0009465fc4` (`0x9466138`…`0x94661ac`):

```
entry  = regGroupTable[ClockData[+1]]          ; 16 B/entry at this+0x60f8
base   = entry[+4]                             ; u32 group offset
width  = entry[+0xd] & 0xff                    ; 0..3
stride = width==2 ? 0x1c : (width==3 ? 0x20 : 0x10)
off    = base + {0x100 if CLK, 0x2c if PWR} + ClockData[+0] * stride
```

i.e. **clock gate = group_base + 0x100 + n·stride**, **power gate =
group_base + 0x2c + n·stride**, with `n` the per-group index. But `base` is the `RegMap`
base, resolved at runtime through the `this+0x29e8` table indexed by the `RegMap` enum
(0x62/0x67/0x69/0x6d/0x71/0x81 per `initRegGroups`), and **I could not resolve
`RegMap 0x71 → physical window`**: the only three direct `initRegMap` callsites in the
whole ApplePMGR text are `0x9480334` (map 0), `0x948034c` (map 1), `0x9480364` (map 2) —
everything above map 2 is populated by a path I did not locate, and `_findRegMap`
`0xfffffe00094505b4` only *searches* the table (`0..0x88`) by window id.

## 5. Task 5: what is not in the kernelcache / ADT (explicit "not found")

Checked by string scan of the whole 114 MB image and by property-name scan of all 325 ADT
nodes:

1. **`clock-frequencies-regs` — 0 occurrences in the kernelcache.** The only place the
   text `clock-frequencies` appears (`0x15eb3e`) is inside `AppleARMPlatform`'s
   `kalloc_type_view` signature blob (`AppleARMIO` … `1211…` … `clock-frequencies` …
   `clock-frequencies-nclk` … `site.const OSSymbol *` … `fclk/aclk/hclk/pclk/nclk/...`),
   and **no pointer or instruction anywhere references that address** (checked: no
   chained-fixup word with target `0x15eb3e`, no plain 64-bit VA, no `adrp`/`add`
   immediate pair in `__TEXT`/`__TEXT_EXEC`). So macOS never reads it.
2. **ADT properties that the PMGR code requires are absent:**
   `clocks` — absent (needed by `_initPerfCountersV1`, `0x9451e04`).
   `perf-regs` — absent (needed at `0x9451cac`).
   `pwrgate-regs` — absent (string exists in ApplePMGR `__cstring`; `_pwrGateRegGroups` /
   `_printPwrGateRegGroups` / `_setPwrGateSleepDepth` / `_setPwrGateRetention` are built
   from it). Consequence: on this machine the pwr-gate retention register groups are
   empty, so gate writes must go through the PS-register path (`ps-groups`, which *is*
   present, 24 B = `{0,0,0,1,0,0}`).
   There is **no id-indexed table for clock ids 325/326/327 or gate ids 291/338 anywhere
   in the ADT or the kernelcache** that I could find.
3. **`AppleARMIODevice`'s enable* are not in the driver kext**: the ANE driver
   (`com.apple.driver.AppleH16ANEInterface`) issues no PMGR MMIO at all; the only
   clock/power operations are the two IOKit virtual calls at `0x8f2a060` / `0x8f2a0a8`
   (slots `0x8a8`/`0x8b0` of `[this+0x2b0]`), consistent with the earlier receipt.
4. **The `clkG`/`pwrG` function object's real MMIO is not resolved**: the six virtual
   slots `ApplePMGRFunctionClockGate::callFunction` uses do not match any `__ZTV` in
   `ApplePMGR`/`ApplePMGRv2`/`ApplePMGRBase`/`AppleBringUpPMGR`/`ApplePMGRNub` under
   either vtable-base convention. **This is the single blocking unknown.** It means the
   `gate id → (RegGroup, index)` translation is not statically recovered, even though the
   ids themselves and the register-file address are.

## 6. What would settle it (ordered by cost)

1. **Run macOS once on the M2 and dump the table.** `ioreg -l -w0 -p IODeviceTree` will
   not show it (it is not a DT property), but
   `sudo dtrace -n 'fbt::ApplePMGRFunctionClockGate::callFunction:entry { printf("%x %x", arg1, arg2) }'`
   plus `fbt::ApplePMGRv2:_getPTDRangeInfo:entry`-style probes, or simply
   `sudo kpmutil`/`lldb -k` reading `pmgr+0x29e8` (the 0x89 × 0x60 `RegMap` table) and
   `pmgr+0x60f8` (the 16-byte `RegGroup` table) after boot, gives the map→window→offset
   mapping directly. This is one `lldb` session and would close the question.
2. **Differential boot trace on the M2**: read the whole `0x3_0070_0000`+0xcc000 block
   under macOS before/after an ANE power-on (e.g. via a kext or `ioreg`-driven ANE
   activity), and diff. Any bit that changes in the `+0x100`-strided clock-gate window for
   `ANE_SYS` (device 0x3c) is the answer, regardless of the table.
3. **Decode the ADT's own write script.** If `clock-frequencies-regs` really is what iBoot
   uses to program the clock generators (the stride-4 address + constant payload pattern
   in §3.1), then an iBoot/ADT-aware trace (m1n1 `adt` + a boot-time MMIO trace) maps it
   to registers. Costly; needs an M2 boot with tracing.
4. **Another SoC's ADT** (e.g. a T8112 or T602x dump from a machine with the same PMGR
   revision) would let us test whether `pwrgate-regs`/`clocks` exist there — that would
   tell us whether their absence on J700 is a machine state difference (e.g. properties
   added by a later iBoot) or a genuine T8140 layout difference.

## 7. Reproduction

```bash
# ADT: property table + physical translation
python3 /var/tmp/opencode/adt_dump.py /arm-io/ane /arm-io/pmgr
# (adt_dump.py uses parse_adt/walk from the aurora parser)

# kernelcache: kext parse (segments + LC_SYMTAB)
python3 /var/tmp/neo/kext.py com.apple.driver.AppleARMPlatform
python3 /var/tmp/neo/kext.py com.apple.driver.AppleT8140
python3 /var/tmp/neo/kext.py com.apple.driver.ApplePMGR
python3 /var/tmp/neo/kext.py com.apple.driver.AppleT8140PMGR

# the decisive disassembly
python3 /var/tmp/neo/dz.py kext_com_apple_driver_AppleARMPlatform.pkl \
    0xfffffe000896a304 0xfffffe000896a530      # enableDeviceClock / enableDevicePower
python3 /var/tmp/neo/dz.py kext_com_apple_driver_AppleARMPlatform.pkl \
    0xfffffe000896a8c0 0xfffffe000896af10      # initDeviceWithProvider
python3 /var/tmp/neo/dz.py kext_com_apple_driver_AppleT8140.pkl \
    0xfffffe0009791310 0xfffffe00097914c4      # H17P platform IO
python3 /var/tmp/neo/dz.py kext_com_apple_driver_ApplePMGR.pkl \
    0xfffffe00094864f4 0xfffffe00094866e4      # FunctionClockGate::callFunction
python3 /var/tmp/neo/dz.py kext_com_apple_driver_AppleT8140PMGR.pkl \
    0xfffffe00098372cc 0xfffffe00098375a0      # T8140 initRegGroups
```

Helper scripts written for this analysis: `/var/tmp/neo/dz.py` (range disassembler with
adrp/add and ldr-literal resolution), `/var/tmp/neo/kext.py` (MH_FILESET kext parser),
`/var/tmp/neo/vt.py` (chained-fixup vtable slot resolver), `/var/tmp/opencode/adt_dump.py`.

## 8. Claims ledger

| Claim | Basis |
| --- | --- |
| `enableDeviceClock` indexes `clock-gates`, not `clock-ids` | compiled fact (`0x896a308`, `0x896a394`, `0x896a3b4`) + property literal `0x7162d49` |
| gate ids 291/338 are PMGR deviceIDs `0x123`/`0x152` = `ANE-SYS-V`/`ANE-SYS-V-PMP` | ADT `devices` decode (§3.3) — fact |
| clock ids 325/326/327 are unused during ANE bring-up | follows from row 1 (fact), given two gates exist |
| `this+0x380 == 0` at the ANE call sites | **inference** (2 gates, `+1` second vote) |
| PMGR physical base `0x3_0070_0000` | ADT `reg` + `ranges` (§4) — fact |
| ANE aperture `0x4_0000_0000`, `0x200_0000` | ADT `reg` + `ranges`; corroborated by `hw-dpe-reg` `0x4_019780E0` — fact |
| clock gate = `group_base+0x100+n·stride`, power gate `+0x2c+n·stride` | `_printClocks` `0x9466138…0x94661ac` — compiled fact; the group base is the unresolved part |
| `clock-frequencies-regs` is a write script, not an id map | arithmetic + byte-structure evidence (§3.1); **inference** on intent |
| no static clock-gate/ClockData table in either kext | exhaustive scan of the two kexts' `__const`/`__DATA_CONST`/`__data` — fact |
| the `clkG` virtual slot → register mapping | **not found** (§2.4) |

Nothing here is device qualification, and none of it has executed on an ANE.
