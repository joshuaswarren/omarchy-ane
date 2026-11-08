# J700 / H17 ANE tunable decode — `AppleT8140ANEHAL`

Offline static analysis of `/var/tmp/neo/kernelcache.bin`
(`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`) and
`/var/tmp/neo/hal_text.S`. No hardware was touched, no module loaded, no
device write issued.

Decoder + self-check: [`decode_tunables.py`](decode_tunables.py),
full output: [`selfcheck.txt`](selfcheck.txt).

---

## 0. Summary of the correction

The previous analysis reported **"registers at `0x11378b8 + 0xc*k`"**. That is
a category error, and it is the single bug that made the HAL tunable program
unrunnable.

`0x11378b8` is a **file offset inside `__DATA_CONST` of the kext**. It holds a
12-byte descriptor whose first word is `0x40000000`:

```
011378b8: 40000000 0061c3c0 00180000
01137ad4: 40000338 0061c530 00180000
```

Both of those bytes are correct — they *are* descriptors. But:

* word 0 is `width:4 | offset:28`, so `0x40000000` is **width 4, offset 0**
  and `0x40000338` is **width 4, offset `0x338`**;
* word 1 is an 8-byte **relocated pointer to the payload**, so the word that
  looks like part of the descriptor (`0x00180000`) is really the *top half of
  the pointer slot*;
* the descriptor is *not* a register. The register offset is 28 bits wide and
  never exceeds `0x401c` in this image.

The correct engine offsets for this table are `0x0 … 0x401c`, not `0x11378b8`.

---

## 1. Artifacts and mapping

| item | value |
|---|---|
| `kernelcache.bin` | MH_FILESET, LE, magic `0xfeedfacf`, filetype 12, 114,688,000 bytes |
| outer `__TEXT` | vm `0xfffffe0007004000`, fileoff `0` |
| **image fixup base** | **`VA = 0xfffffe0007004000 + file_offset`** |
| HAL kext | `com.apple.driver.AppleT8140ANEHAL`, fileoff `0x61bcf0`, vm `0xfffffe000761fcf0` |
| HAL `__TEXT` | vm `0xfffffe000761fcf0`, fileoff `0x61bcf0`, size `0x1266` |
| HAL `__TEXT_EXEC` | vm `0xfffffe0009799040`, fileoff `0x2795040` |
| HAL `__DATA_CONST` | vm `0xfffffe000813b828`, fileoff `0x1137828`, size `0x30b0` |
| HAL `__DATA` | vm `0xfffffe000c4448b0`, fileoff `0x54408b0` |
| engine aperture | `0x400000000`, size `0x2000000` |

The fixup base is proven two ways:

1. outer `__TEXT` is `vm=0xfffffe0007004000, fileoff=0` (read from its
   `LC_SEGMENT_64`, offset `0x20` of the outer Mach-O);
2. it is the only base that makes every relocation target in this kext land
   inside a mapped segment. E.g. table slot `0x1137af0` → target `0x011378b8`
   → VA `0xfffffe000813b8b8`, which is inside the kext's `__DATA_CONST`
   (`0xfffffe000813b828 + 0x90`). ✔

---

## 2. Table layout — proof

### 2.1 The symbol table names every piece

From `syms_HAL.json` (`__TEXT_EXEC`, sect 4):

```
fffffe00097a80e0  _tunableh_configuration_offset
fffffe00097a80f0  _tunableh_configuration_addr
fffffe00097a8108  _tunableh_configuration_mask
fffffe00097a81d0  _tunableh_configuration_get_value
fffffe00097a82ec  _tunableh_configuration_value
fffffe00097a83b4  _tunableh_configuration_apply_value
fffffe00097a83f8  _tunableh_configuration_applied_value
fffffe00097a8484  _tunableh_configuration_is_end
fffffe00097a849c  _tunableh_apply_nop_callback
fffffe00097a84a4  _tunableh_apply
fffffe00097a876c  _tunableh_apply_configuration
fffffe00097a8930  _tunableh_write
fffffe00097a809c  _tunableh_module_finalize
fffffe00097a808c  _tunableh_device
```

### 2.2 Word 0 of a descriptor = `width:4 | offset:28`

`_tunableh_configuration_offset` @ `0x97a80e0`:

```
fffffe00097a80e0: d503245f  bti  c
fffffe00097a80e4: b9400008  ldr  w8, [x0]
fffffe00097a80e8: 92406d00  and  x0, x8, #0xfffffff      <-- offset = bits 27:0
```

`_tunableh_configuration_addr` @ `0x97a80f0`:

```
fffffe00097a80f4: f9400028  ldr  x8, [x1]                 ; x1 = ctx, ctx[0] = base
fffffe00097a80f8: b9400009  ldr  w9, [x0]
fffffe00097a80fc: 92406d29  and  x9, x9, #0xfffffff
fffffe00097a8100: 8b090100  add  x0, x8, x9               ; base + offset
```

Width is the top nibble, taken in all four width-dispatching helpers with the
same sequence:

```
fffffe00097a8404: b9400108  ldr  w8, [x8]
fffffe00097a8408: 531c7d09  lsr  w9, w8, #28               ; <-- width = bits 31:28
fffffe00097a840c: 71000d3f  cmp  w9, #0x3
fffffe00097a8414: 7100051f  cmp  w9, #0x1   -> ldrb
fffffe00097a841c: 7100093f  cmp  w9, #0x2   -> ldrh
fffffe00097a8438: 7100113f  cmp  w9, #0x4   -> ldr  w
fffffe00097a8440: 7100213f  cmp  w9, #0x8   -> ldr  x
```

So the legal widths are exactly `{1,2,4,8}` and nothing else is accepted
(anything else falls through to `ret` with `w0 = 0`).

### 2.3 Descriptor = 12 bytes, packed

`_tunableh_apply` selects the element stride from a small record pointed to by
`ctx+48`. `_tunableh_module_finalize` @ `0x97a809c` builds that pointer:

```
fffffe00097a80a0: d37ff828  lsl  x8, x1, #1              ; 2 * variant
fffffe00097a80a8: 9128c129  add  x9, x9, #0xa30           ; x9 = 0xfffffe0007620a30
fffffe00097a80ac: 8b08012a  add  x10, x9, x8              ; &rec[2*variant]
fffffe00097a80c0: f900180a  str  x10, [x0, #48]           ; ctx->variant = ...
fffffe00097a80c4: f100083f  cmp  x1, #2
fffffe00097a80c8: 54000063  b.cc 0xfffffe00097a80d4
fffffe00097a80cc: 92800008  mov  x8, #-1
fffffe00097a80d0: f9001c08  str  x8, [x0, #56]           ; ctx->width = -1 (unused)
```

Every table consumer in the ANE path calls it with `w1 = 2`
(`0x9799444`, `0x97994e8`, `0x979958c`, `0x979b15c`, `0x979b0b0`,
`0x979b238`, `0x979b320`), so `ctx+48 = 0xfffffe0007620a34`, whose bytes are:

```
0061ca34: 01 01 00 00        ; byte0 = 1 (payload via pointer), byte1 = 1 (elements are pointers)
```

`_tunableh_apply` @ `0x97a85b0` therefore takes the "pointer element, stride 8"
arm:

```
fffffe00097a85b0: 39400509  ldrb w9, [x8, #1]             ; byte1 == 1
fffffe00097a85d8: 36000089  tbz  w9, #0, 0xfffffe00097a85e8
fffffe00097a85dc: aa1003fb  mov  x27, x16                 ; x27 = 0x97a887c = deref
fffffe00097a85e0: 5280011c  mov  w28, #0x8                 ; stride = 8 bytes
```

`0x97a887c`: `bti c; ldr x0,[x0]; ret` (at `0x97a8880`) — so the table element is
a **64-bit pointer** to a descriptor, and the pointer targets are the 12-byte
records. The 12-byte
size is independently confirmed by the *other* arm of the same function, the
inline-descriptor case, which hardcodes `mov w28, #0xc` at `0x97a8618`.

Confirmation from the data itself — table `0x813bc10`'s two targets are
`0x1137ad4` and `0x1137ae0`:

```
01137ad4: 40000338 0061c530 00180000
01137ae0: 4000033c 0061c540 00180000
```

`0x1137ae0 - 0x1137ad4 = 0xc`. 12 bytes. ✔

### 2.4 The pointer words are arm64e chained relocations

The word at `desc+4` is read as **8 bytes**:

```
fffffe00097a83a0: f8404108  ldur x8, [x8, #4]     ; width-4 payload getter
fffffe00097a83a4: b9400500  ldr  w0, [x8, #4]     ; value at payload+4
```

`ldur x8,[desc,#4]` spans `desc+4 … desc+11`, i.e. it *overlaps* the third
u32. That is why the third word looks like a constant `0x00180000`: it is the
high half of a `DYLD_CHAINED_PTR_ARM64E` slot.

Decoding `0x001800000061c530`:

| field | bits | value |
|---|---|---|
| `target` | 42:0 | `0x61c530` |
| `high8` | 50:43 | `0` |
| `next` | 61:51 | `3` → stride `3*4 = 12` bytes (exactly the descriptor stride) |
| `isBind` | 62 | 0 |
| `isAuth` | 63 | 0 |

`next = 3` is constant across every descriptor because the descriptor stride
is uniformly 12 — which is why the third u32 is always `0x00180000`. Table
*elements* decode with `next = 2` (stride 8), e.g. the slot at file `0x1137af0`
holds `0x00100000011378b8` → target `0x11378b8`, `next = 2`.

Note: the *inner* kext has only 10 load commands and no
`LC_DYLD_CHAINED_FIXUPS`; the fixup blob lives on the outer fileset image
(`LC_DYLD_CHAINED_FIXUPS` at outer cmd offset, blob at file `0x6d5c000`,
size `0x89c`). The relocation base is the outer `__TEXT` vmaddr
`0xfffffe0007004000`.

### 2.5 Payload = `{mask[W], value[W], get_value_fn[8]}`

`_tunableh_configuration_mask` @ `0x97a8108`, pointer arm (bit0 of
`*ctx->variant` is 1, which is our case):

```
fffffe00097a8120: b9400109  ldr  w9, [x8]              ; descriptor word 0
fffffe00097a8124: 531c7d29  lsr  w9, w9, #28            ; width
fffffe00097a8184: f8404108  ldur x8, [x8, #4]           ; payload pointer
fffffe00097a8188: f9400100  ldr  x0, [x8]               ; mask = payload[0]   (w==8)
fffffe00097a81c0: b9400100  ldr  w0, [x8]               ; mask = payload[0]   (w==4)
```

`_tunableh_configuration_value` @ `0x97a82ec`:

```
fffffe00097a83a0: f8404108  ldur x8, [x8, #4]
fffffe00097a83a4: b9400500  ldr  w0, [x8, #4]           ; value = payload[W]
```

`_tunableh_configuration_get_value` @ `0x97a81d0`:

```
fffffe00097a8254: f8404108  ldur x8, [x8, #4]
fffffe00097a8258: f9400910  ldr  x16, [x8, #16]         ; w==8: callback at payload+16
fffffe00097a82c0: f9400508  ldr  x8, [x8, #8]           ; w==4: callback at payload+8
fffffe00097a82c4: b4ffff88  cbz  x8, 0xfffffe00097a82b4 ; NULL -> return 0
```

So the record is `2*W + 8` bytes and every payload in this image has a NULL
`get_value` (word at `payload+8` is `0`), which collapses the formula below.

Raw first payloads (width 4 ⇒ 16-byte records):

```
0061c3c0: ff000080 1b000080 00000000 00000000   mask=0x800000ff value=0x8000001b  cb=0
0061c530: ffffffff 20000500 00000000 00000000   mask=0xffffffff value=0x00050020  cb=0
0061c520: 01000000 01000000 00000000 00000000   mask=0x00000001 value=0x00000001  cb=0
```

### 2.6 Terminator

`_tunableh_apply` with `byte1 == 1` uses `is_null`, not `is_end`:

```
fffffe00097a85b8: 91222210  add  x16, x16, #0x888       ; 0x97a8888 = is_null
fffffe00097a8888: d503245f  bti  c
fffffe00097a888c: f9400008  ldr  x8, [x0]
fffffe00097a8890: f100011f  cmp  x8, #0x0
fffffe00097a8894: 1a9f17e0  cset w0, eq               ; is_null: *p == 0
```

So a table is `{u64 ptr}… , 0x0000000000000000`. Entry count == extent, because
the walker uses a fixed stride of 8 and stops on the first zero slot. Every
table below was walked until that zero slot; the offsets are recorded in
`selfcheck.txt`.

(For the *other* encoding, inline 12-byte descriptors, the terminator is a
descriptor whose `width` nibble is 0 — `_tunableh_configuration_is_end` @
`0x97a8484`. Both terminators exist in the data because the linker packs a
zeroed slot at each table boundary; that is also why the descriptor at
`0x1137aec` `{0x00000000, 0x011378b8, 0x00100000}` overlaps table
`0x813baf0`'s start.)

---

## 3. Applier semantics

### 3.1 `_tunableh_apply_configuration` @ `0xfffffe00097a876c`

```
fffffe00097a879c: f9400c28  ldr  x8,  [x1, #24]        ; ctx->write, default 0x97a8930
fffffe00097a87b4: 9a880210  csel x16, x16, x8, eq
fffffe00097a87b8: f90007f0  str  x16, [sp, #8]         ; save write fn
fffffe00097a87bc: f9400039  ldr  x25, [x1]             ; base = ctx[0]
fffffe00097a87c0: b9400048  ldr  w8,  [x2]             ; descriptor word 0
fffffe00097a87c4: 92406d1a  and  x26, x8, #0xfffffff   ; offset
fffffe00097a87c8: 531c7d16  lsr  w22, w8, #28          ; width
```

The four helpers the brief asks about, in execution order:

| call site | target | symbol | role |
|---|---|---|---|
| `0x97a87d0` | `0x97a83f8` | `_tunableh_configuration_applied_value` | **reads the live register** at `base+offset`, width-sized |
| `0x97a87e0` | `0x97a8108` | `_tunableh_configuration_mask` | returns `mask` = `payload[0]` |
| `0x97a87f4` | `0x97a81d0` | `_tunableh_configuration_get_value` | returns the optional callback result from `payload[2W]` (NULL here ⇒ 0) |
| `0x97a8804` | `0x97a82ec` | `_tunableh_configuration_value` | returns `value` = `payload[W]` |

The read of the live register is unambiguous — it is the only one that
dereferences `ctx[0]`:

```
fffffe00097a8424: f9400029  ldr  x9, [x1]              ; base
fffffe00097a8428: 92406d08  and  x8, x8, #0xfffffff    ; offset
fffffe00097a842c: 8b080128  add  x8, x9, x8            ; base + offset
fffffe00097a8430: 79400100  ldrh w0, [x8]              ; width 2
fffffe00097a8454: f9400100  ldr  x0, [x8]              ; width 8
fffffe00097a8468: 39400100  ldrb w0, [x8]              ; width 1
fffffe00097a847c: b9400100  ldr  w0, [x8]              ; width 4
```

### 3.2 The combination — masked read-modify-write, then a plain store

```
fffffe00097a87d4: aa0003f7  mov  x23, x0               ; x23 = applied_value (live reg)
fffffe00097a87e4: aa0003f8  mov  x24, x0               ; x24 = mask
fffffe00097a87e8: 8a2002fb  bic  x27, x23, x0          ; x27 = live & ~mask
fffffe00097a87f8: aa0003f7  mov  x23, x0               ; x23 = get_value()
fffffe00097a8808: aa170008  orr  x8, x0, x23           ; x8  = value | get_value()
fffffe00097a880c: 8a180108  and  x8, x8, x24           ; x8 &= mask
fffffe00097a8810: 8b1a0322  add  x2, x25, x26          ; x2 = base + offset  (address)
fffffe00097a8814: aa1b0104  orr  x4, x8, x27           ; x4 = new value
fffffe00097a8820: aa1603e3  mov  x3, x22               ; x3 = width
fffffe00097a8824: f94007f0  ldr  x16, [sp, #8]         ; write fn (default 0x97a8930)
fffffe00097a8858: d71f0a11  braa x16, x17              ; tail-call
```

With `get_value() == 0` (true for all 59 descriptors in this image):

```
new = (live & ~mask) | (value & mask)
```

**It is not an OR into the register, not a plain write, and not an expression
evaluator.** It is a masked bitfield replace: bits selected by `mask` are
overwritten from `value`, all other bits keep their live value.

`_tunableh_write` @ `0x97a8930` then performs a pure store:

```
fffffe00097a896c: 39000044  strb w4, [x2]              ; width 1
fffffe00097a894c: 79000044  strh w4, [x2]              ; width 2
fffffe00097a8974: b9000044  str  w4, [x2]              ; width 4
fffffe00097a8964: f9000044  str  x4, [x2]              ; width 8
```

Note the consequence for a bring-up script: because the applier *reads first*,
every one of these 37 writes requires a readable engine register. If the
engine is not clocked/powered, `applied_value` faults or returns garbage that
lands in the preserved bits.

---

## 4. Which tunable function is chosen

The HAL exports exactly two platform functions and dispatches on `OSSymbol`
pointer identity — `AppleT8140ANEHAL::callPlatformFunction`, `0xfffffe00097a94b8`:

```
fffffe00097a94c4: 900164e8  adrp x8, 0xfffffe000c445000
fffffe00097a94c8: f9443d08  ldr  x8, [x8, #2168]      ; &"AppleANETunableApplyFunction"
fffffe00097a94cc: eb08003f  cmp  x1, x8
fffffe00097a94d0: 54000140  b.eq 0xfffffe00097a94f8
fffffe00097a94f8: aa0303e1  mov  x1, x3
fffffe00097a94fc: 97fffec3  bl   0xfffffe00097a9008  <-- ONLINE path
fffffe00097a94d4: 900164e8  adrp x8, 0xfffffe000c445000
fffffe00097a94d8: f9444108  ldr  x8, [x8, #2176]      ; &"...Legacy"
fffffe00097a94dc: eb08003f  cmp  x1, x8
fffffe00097a94e0: 54000101  b.ne 0xfffffe00097a9500
fffffe00097a94f0: 97ffff3d  bl   0xfffffe00097a91e4  <-- Legacy path
```

The two globals are filled at `0x9799514` from the strings at
`0xfffffe0007620b0a` and `0xfffffe0007620b27`
(`"AppleANETunableApplyFunction"`, `"AppleANETunableApplyFunctionLegacy"`,
verified by reading file `0x61cb0a` / `0x61cb27`).

So the selection is by **symbol name**, not by family: the caller asks for one
or the other. The family value only decides *which HAL object* exists.

`ane-type 0x200 → family 0xe0` is proven in
`AppleH16ANEInterface`, `ANEHWDevice::initializeANESoCConfig`
`0xfffffe0008f64ae0`:

```
fffffe0008f64f78: 71083d1f  cmp  w8, #0x20f
fffffe0008f64f7c: 54002c6c  b.gt 0xfffffe0008f65508
fffffe0008f64f90: 7108011f  cmp  w8, #0x200
fffffe0008f64f94: 5400d521  b.ne 0xfffffe0008f66a38     ; not ANE type 0x200
fffffe0008f64f98: 52801c08  mov  w8, #0xe0              ; <-- internal family 0xe0
fffffe0008f64f9c: b90002a8  str  w8, [x21]              ; family = 0xe0
fffffe0008f64fa0: 52800128  mov  w8, #0x9
fffffe0008f64fa4: b901ca68  str  w8, [x19, #456]        ; hw gen = 9
```

The consumer side calls the non-Legacy symbol for this family, e.g.
`ANEDriver` at `0xfffffe0008f690d0`:

```
fffffe0008f690d0: f9417660  ldr  x0, [x19, #744]       ; the ONLINE OSSymbol
fffffe0008f690e4: d2803f11  mov  x17, #0x1f8
fffffe0008f690f0: d2800001  mov  x1, #0x0
fffffe0008f69100: d73f0910  blraa x8, x16              ; callPlatformFunction(sym,...)
```

(`[x19+744]` / `[x19+752]` are the two symbols, stored at
`0xfffffe0008ef3f4c` / `0xfffffe0008ef3f60`.)

**Therefore the ONLINE path is `0xfffffe00097a9008`.**

---

## 5. `AppleANETunableApplyFunction` — complete ordered write list

`0xfffffe00097a9008`. Everything below is engine-relative; add
`0x400000000`.

### 5.1 CE handshake (unconditional, always first)

```
fffffe00097a905c: str  x19, [x20, #136]        ; self->dev = arg
fffffe00097a906c: ldr  x9,  [x19 + 0x18a4010]  ; 64-bit read
fffffe00097a907c: ldr  x8,  [x19 + 0x18a4018]  ; 64-bit read
fffffe00097a9080: and  x9,  x9, #3             ; v    = [0x18a4010] & 3
fffffe00097a9084: eor  x21, x9, #3             ; v3   = v ^ 3
fffffe00097a9088: tst  x21, x8
fffffe00097a908c: b.ne 0xfffffe00097a91a4      ; if (v3 & [0x18a4018]) -> "CE in error state", return
fffffe00097a9090: cmp  x9, #3
fffffe00097a9094: b.eq 0xfffffe00097a9114      ; if v == 3 skip the trigger entirely
fffffe00097a90a4: str  x21, [x19 + 0x18a4000]  ; WRITE 0x18a4000 = v3   (64-bit)
fffffe00097a90b4:  ... poll loop, w22 = 0x1389 = 5001 iterations
fffffe00097a90bc: ldr  x8,  [x19 + 0x18a4008]
fffffe00097a90c0: and  x8,  x8, #3
fffffe00097a90c4: cmp  x8,  x21                ; until ([0x18a4008] & 3) == v3
fffffe00097a90cc: mov  w0, #0xa ; bl 0xbba00c0 ; delay(10), else "Fail to trigger CE"
fffffe00097a9104: ldr  x8,  [x19 + 0x18a4018]
fffffe00097a9108: tst  x8,  x21
fffffe00097a910c: b.ne 0xfffffe00097a91c4      ; if ([0x18a4018] & v3) -> "CE in error state"
fffffe00097a9110: str  x21, [x19 + 0x18a4008]  ; WRITE 0x18a4008 = v3   (64-bit)
```

| # | engine offset | width | operation | value |
|---|---|---|---|---|
| 1 | `0x18a4000` | 8 | plain store (skip if `v == 3`) | `v ^ 3` where `v = [0x18a4010] & 3` |
| 2 | `0x18a4008` | 8 | plain store, after `([0x18a4008]&3)==v^3` poll | `v ^ 3` |

Note `v == 3 ⇒ v^3 == 0`, and in that case the code branches past **both**
writes. A bring-up that unconditionally writes `v^3` still lands on the same
observable state when `v == 3` (a write of 0 to `0x18a4008` is *not* skipped in
the script but is skipped by Apple); that difference is real and is the only
divergence between the current Linux sequence and Apple's for this block.

### 5.2 Where the base comes from

```
fffffe00097a9114: 6f00e400  movi v0.2d, #0x0
fffffe00097a9118: ad0483e0  stp  q0, q0, [sp, #144]     ; zero 64 bytes at sp+0x68
fffffe00097a9120: f90037f3  str  x19, [sp, #104]        ; CTX[0] = dev  (sp+0x68)
fffffe00097a9124: 9101a3e0  add  x0, sp, #0x68
fffffe00097a9128: 94003bc1  bl   0xfffffe000979942c    ; af default_apply_to
fffffe00097a912c: 9101a3e0  add  x0, sp, #0x68
fffffe00097a9130: 94003c20  bl   0xfffffe0009799574    ; af secure_lock_wr_dis_apply_to
fffffe00097a9134: 9101a3e0  add  x0, sp, #0x68
fffffe00097a9138: 94003b98  bl   0xfffffe00097994d0    ; af secure_lock_lock_apply_to
```

`CTX` is zeroed, so `ctx+8/+16/+24/+32` are NULL and `_tunableh_apply` falls
back to the built-in callbacks:

```
fffffe00097a8538: adrp x16, 0xfffffe00097a8000
fffffe00097a853c: add  x16, x16, #0x49c            ; 0x97a849c = nop callback
fffffe00097a8574: ldr  x8, [x19, #16]              ; ctx->apply, else:
fffffe00097a857c: add  x16, x16, #0x76c            ; 0x97a876c = _tunableh_apply_configuration
fffffe00097a8590: ldr  x8, [x19, #32]              ; ctx->post, else nop
```

### 5.3 Table A — `0xfffffe000813baf0` — 35 entries — UNCONDITIONAL

Reached from `0x97990e0` (`..._sne_af_aneaxi2af_default_apply` body):

```
fffffe00097990fc: adrp x2, 0xfffffe000813b000
fffffe0009799100: add  x2, x2, #0xaf0              ; table A
fffffe0009799110: mov  x0, x16                    ; paciza(0x9799618)
fffffe0009799114: mov  x1, x19                    ; ctx
fffffe0009799118: bl   0xfffffe00097a84a4
```

All entries are `width 4`, so each is a 32-bit masked RMW.

| # | engine offset | width | operation | mask | value |
|---|---|---|---|---|---|
| 3 | `0x00000000` | 4 | RMW | `0x800000ff` | `0x8000001b` |
| 4 | `0x00000004` | 4 | RMW | `0x80ff00ff` | `0x800c000d` |
| 5 | `0x00000008` | 4 | RMW | `0x80ff00ff` | `0x800c000d` |
| 6 | `0x0000000c` | 4 | RMW | `0x80ff00ff` | `0x800c000d` |
| 7 | `0x00000010` | 4 | RMW | `0x80ff00ff` | `0x80000003` |
| 8 | `0x00000014` | 4 | RMW | `0x80ff00ff` | `0x80000003` |
| 9 | `0x00000018` | 4 | RMW | `0x800000ff` | `0x80000013` |
| 10 | `0x0000001c` | 4 | RMW | `0x800000ff` | `0x80000020` |
| 11 | `0x00000020` | 4 | RMW | `0x80ff00ff` | `0x8012000c` |
| 12 | `0x00000024` | 4 | RMW | `0x80ff00ff` | `0x80180006` |
| 13 | `0x00000028` | 4 | RMW | `0x80ff00ff` | `0x80070003` |
| 14 | `0x0000002c` | 4 | RMW | `0x80ff00ff` | `0x80070003` |
| 15 | `0x00000030` | 4 | RMW | `0x800000ff` | `0x80000015` |
| 16 | `0x00000034` | 4 | RMW | `0x800000ff` | `0x80000011` |
| 17 | `0x00000038` | 4 | RMW | `0x800000ff` | `0x80000011` |
| 18 | `0x00000100` | 4 | RMW | `0x000000ff` | `0x000000ff` |
| 19 | `0x00000108` | 4 | RMW | `0x00000006` | `0x00000000` |
| 20 | `0x00000304` | 4 | RMW | `0x3f3f0000` | `0x02020000` |
| 21 | `0x00000314` | 4 | RMW | `0x80000000` | `0x80000000` |
| 22 | `0x0000031c` | 4 | RMW | `0x80000000` | `0x80000000` |
| 23 | `0x00000324` | 4 | RMW | `0x80000000` | `0x80000000` |
| 24 | `0x00000408` | 4 | RMW | `0xffff0020` | `0x00120020` |
| 25 | `0x00000700` | 4 | RMW | `0x00000400` | `0x00000400` |
| 26 | `0x00000748` | 4 | RMW | `0x07ff07ff` | `0x00200030` |
| 27 | `0x00000800` | 4 | RMW | `0x00000400` | `0x00000400` |
| 28 | `0x00000848` | 4 | RMW | `0x07ff07ff` | `0x00100030` |
| 29 | `0x00000900` | 4 | RMW | `0x00000400` | `0x00000400` |
| 30 | `0x00000948` | 4 | RMW | `0x07ff07ff` | `0x01000100` |
| 31 | `0x00000b00` | 4 | RMW | `0xffffffff` | `0x7fffffff` |
| 32 | `0x00000b04` | 4 | RMW | `0x00000100` | `0x00000100` |
| 33 | `0x00004008` | 4 | RMW | `0x00000001` | `0x00000000` |
| 34 | `0x0000400c` | 4 | RMW | `0x00000001` | `0x00000000` |
| 35 | `0x00004014` | 4 | RMW | `0x00000001` | `0x00000000` |
| 36 | `0x00004018` | 4 | RMW | `0x00000001` | `0x00000000` |
| 37 | `0x0000401c` | 4 | RMW | `0x00000001` | `0x00000001` |

Descriptor VAs are `0xfffffe000813b8b8 … 0xfffffe000813ba50`, payloads
`0xfffffe00076203c0 … 0xfffffe0007620520`. Note `#37` at `0x401c` is the last
one before the terminator — the table's extent is exactly 35, and `0x401c` is
followed by the zero slot at file `0x1137c08`.

### 5.4 Table B — `0xfffffe000813bc10` — 2 entries — GATED, but taken

Reached by the tail of `0x97990e0`:

```
fffffe000979911c: f9400688  ldr  x8, [x20, #8]        ; module = _tunableh_device()
fffffe0009799120: b50000a8  cbnz x8, 0xfffffe0009799134   ; m+8 != 0  -> stop
fffffe0009799124: 39408288  ldrb w8, [x20, #32]
fffffe0009799128: 35000068  cbnz w8, 0xfffffe0009799134   ; m+32 != 0 -> stop
fffffe000979912c: 39408688  ldrb w8, [x20, #33]
fffffe0009799130: 34000088  cbz  w8, 0xfffffe0009799140   ; m+33 == 0 -> apply
fffffe0009799140: adrp x2, 0xfffffe000813b000
fffffe0009799144: add  x2, x2, #0xc10                 ; table B
```

`_tunableh_device` @ `0x97a808c` returns the global block
`0xfffffe000c445850` (`adrp x0,0xfffffe000c445000; add x0,x0,#0x850`). That
block is **zero in the image** (file `0x5441850`, 40 bytes all `0x00`), and
nothing in this kext writes it: `_tunableh_init` @ `0x97a806c` is the only
writer (`stp q0,q1,[x9]; str x8,[x9,#32]`) and it has **no caller** in the
kext. So `m+8 = 0`, `m+32 = 0`, `m+33 = 0`, and table B **is applied**.

| # | engine offset | width | operation | mask | value |
|---|---|---|---|---|---|
| 38 | `0x00000338` | 4 | RMW | `0xffffffff` | `0x00050020` |
| 39 | `0x0000033c` | 4 | RMW | `0xffffffff` | `0x000a0030` |

These two are full-word writes (`mask = 0xffffffff`), so `#38` and `#39` are
effectively plain stores of `0x00050020` and `0x000a0030`.

### 5.5 Tables C and D — GATED and NOT taken here

Both require `m+8 == 1`, which is false, so on this image they are dead:

```
fffffe000979925c: add  x2, x2, #0xc28     ; C, in 0x9799220 (lock), gated m+8==1 && m+32==0 && m+33==1
fffffe0009799380: add  x2, x2, #0xc58     ; D, in 0x9799344 (wr_dis), same gate
```

| table | call site | engine offsets | width | mask | value |
|---|---|---|---|---|---|
| C `0x813bc28` | `0x9134` → `0x9799220` | `0xdfc, 0xefc, 0xffc, 0x10fc, 0x11fc` | 4 | `0x00000001` | `0x00000001` |
| D `0x813bc58` | `0x9130` → `0x9799344` | `0xdf8, 0xef8, 0xff8, 0x10f8, 0x11f8` | 4 | `0x00000001` | `0x00000001` |

If a future image sets `m+8 = 1` with `m+32 = 0, m+33 = 1`, they run in call
order **D then C**, giving 45 unconditional-plus-lock writes.

### 5.6 c1ppt group — `pmgr-c1ppt` boot-arg gated, default OFF

```
fffffe00097a8f2c: 3902427f  strb wzr, [x19, #144]         ; default 0
fffffe00097a8f34: add  x0, x0, #0xc21                     ; "pmgr-c1ppt"
fffffe00097a8f40: bl   0xfffffe000bccad94                 ; PE_parse_boot_argn(...,4)
fffffe00097a8f44: cbz  w0, 0xfffffe00097a8f7c             ; absent -> leave 0
fffffe00097a8f54: strb w8, [x19, #144]                    ; = (val != 0)
fffffe00097a913c: ldrb w8, [x20, #144]
fffffe00097a9140: tbz  w8, #0, 0xfffffe00097a9188         ; off -> return
```

With the boot arg set, base becomes `dev + 0x197c000`
(`fffffe00097a9144`–`0x97a9164`), and four applies run:

| order | call | table | engine offsets (base `0x197c000`) | mask / value |
|---|---|---|---|---|
| E | `0x9168` → `0x979b144` | `0x813be50` | `0x020, 0x024, 0x028, 0x02c, 0x040, 0x044, 0x048, 0x060` | `0xffff/0x1068`, `0xffff/0x0c4c`, `0xffffffff/0`, `0xff/0x3`, `0x3ff/0xbf`, `0x3ff/0x19a`, `0x1ffff/0`, `0x1f1f/0x103` |
| F | `0x9170` → `0x979acec` | `0x813be30` (`m+8==0`) / `0x813be40` (`m+8==1`) | `0x004` | `0x3/0x0` / `0x3/0x1` |
| H | `0x9178` → `0x979b2fc` (wr_dis) | `0x813be20` | `0x03ff0` | `0x7fffff/0x77ebf9` |
| G | `0x9180` → `0x979b220` (lock) | `0x813be10` | `0x03ffc` | `0x1/0x1` |

(Note the call order is E, F, G, H — wr_dis precedes lock in this group,
the opposite of the af group, because the call sites differ.)

---

## 6. Self-check output

Verbatim from `python3 decode_tunables.py` (`selfcheck.txt`):

```
============================================================================================================
SELF-CHECK
============================================================================================================
1. total descriptors decoded ............ 59
   per table ............................ A=35, B=2, C=5, D=5, E=8, F=1, I=1, G=1, H=1
2. widths seen .......................... [4]  OK
3. every engine offset < 0x2000000 ...... OK
4. width-4 offsets 4-byte aligned ....... OK
5. no offset equals a kernelcache file offset (0x11378b8 / 0x1137ad4) ............... OK
6. no offset lands in the table file range 0x1137ae8-0x1137e90 ... OK
7. every payload pointer resolves in image ... OK
8. table extents: each walk ended on an 8-byte NULL slot, so the entry
   count equals the extent: A 35@0x1137af0, B 2@0x1137c10, C 5@0x1137c28, D 5@0x1137c58, E 8@0x1137e50, F 1@0x1137e30, I 1@0x1137e40, G 1@0x1137e10, H 1@0x1137e20
9. duplicate (base,offset) among simultaneously-applied tables ... F and I both target 0x197c004 but are mutually exclusive (module[+8]==0 vs ==1), so no double write can occur

RESULT: ALL CHECKS PASS
```

Additional checks performed by hand:

* largest af-group offset is `0x401c`; largest c1ppt offset is `0x3ffc`
  (+ base `0x197c000` → `0x197fffc`). Both `< 0x2000000`, so the whole
  program is inside one `0x400000000` aperture.
* `0x18a4010` and `0x18a4018` are *reads only*; `0x18a4000` and `0x18a4008`
  are 64-bit stores.
* no engine offset in this report is ≥ `0x61bcf0`, so none can be confused
  with a kernelcache file offset (the failure mode being corrected).

---

## 7. Proven by bytes vs. inference

### Proven by bytes (directly readable, no interpretation left)

1. `VA = 0xfffffe0007004000 + file_offset` — outer `__TEXT` segment command.
2. Descriptor word 0 = `width:4 | offset:28` — `lsr #28` / `and #0xfffffff`
   at `0x97a8408`/`0x97a840c` and `0x97a80fc`.
3. Legal widths `{1,2,4,8}` — the four compares at `0x97a840c`–`0x97a8444`.
4. Descriptor = 12 bytes — the `#0xc` stride at `0x97a8618` and the
   `0x1137ad4 → 0x1137ae0` target spacing in table B.
5. Table element = 8-byte pointer, terminated by `0` — `mov w28,#8` at
   `0x97a85e0`, `ldr x0,[x0]` at `0x97a8880`, `is_null` at `0x97a8888`.
6. `mask` = `payload[0]`, `value` = `payload[W]`, `get_value` = `payload[2W]`
   — `0x97a8188`, `0x97a83a4`, `0x97a82c0`.
7. Operation = `(live & ~mask) | (value & mask)` — `bic x27,x23,x0` at
   `0x97a87e8`, `and x8,x8,x24` at `0x97a880c`, `orr x4,x8,x27` at
   `0x97a8814`; store is a plain `str` at `0x97a8930`.
8. Every table, entry count, offset, mask and value in §5 — emitted by
   `decode_tunables.py` straight from the bytes.
9. Selection of the non-Legacy function — `cmp x1,x8` chain at `0x97a94cc`
   and `0x97a94dc`.
10. `ane-type 0x200 → 0xe0` — `cmp w8,#0x200` / `mov w8,#0xe0` at
    `0x8f64f90`/`0x8f64f98` (i.e. `0xfffffe0008f64f90`).
11. `pmgr-c1ppt` default off — `strb wzr,[x19,#144]` at `0x97a8f2c`.
12. The module block gating tables B/C/D is zero in the image and has no
    writer in this kext.

### Inference (stated as such, not proof)

1. That "J700" corresponds to ADT `ane-type = 0x200`. The brief asserts it;
   nothing in this kernelcache names a board as J700. What *is* proven is the
   `0x200 → 0xe0` mapping and that the resulting object is
   `AppleT8140ANEHAL`.
2. That the fixup `target` field for these slots is an **image-relative file
   offset** rather than a slid VA. The `next=3`/`next=2` decoding is
   self-consistent and every target lands exactly on a plausibly-structured
   object under `VA = base + fileoff`. A loader-side confirmation is still
   worth doing (§8.1).
3. The semantic *meaning* of each field (i.e. what engine block `0x338` is).
   The report deliberately does not name registers — only offsets, widths,
   masks and values.

---

## 8. Unresolved / not guessed

1. **Fixup application at runtime.** The byte-level decode is unambiguous, but
   I did not emulate the XNU kext fixup pass. If the kernel applies the
   `high8`/`next` fields differently than assumed, only the *payload pointer
   values* shift, never the offsets, widths, masks or values. To settle it:
   confirm `0xfffffe00076203c0` holds `ff000080 1b000080` on a booted device.
2. **`_tunableh_init` has no caller.** The function at `0x97a806c` copies 40
   bytes into `0xfffffe000c445850`; nothing in `AppleT8140ANEHAL` calls it.
   If another kext or a boot-arg-driven path does, `m+8` could become 1 and
   tables C/D would activate. Until that is observed, §5.4 is the correct
   static reading, and §5.5 is the alternate.
3. **What `m+8 = 1` means.** It is compared against `1` and gates the two
   "secure_lock" table groups. It reads like a hardware revision / secure-boot
   class selector but the type is not recoverable from this binary.
4. **The `0x18a4010` low-2-bit value on real J700 silicon.** The CE writes
   depend on `v`; if `v == 3` no write happens at all. Not determinable
   statically.
5. **Effect of running the program in the wrong order.** Tables A and B are
   ordered and B depends on the same base. Whether A must complete before B is
   not stated by the code (no barrier between the two `_tunableh_apply` calls
   in `0x97990e0`), but nothing suggests reordering is safe.
6. **Fields at `desc+8` in the last descriptor of a table** (e.g.
   `0x1137a50`) read into the following table's first descriptor. They are
   never dereferenced by the applier, so this is harmless — noted only so a
   future reader does not "fix" it.

---

## 9. Practical consequence for the Linux bring-up

The current ordered bring-up already performs §5.1 correctly. What it has
never done is §5.3 + §5.4: **37 masked read-modify-writes on the engine
aperture, done before any RTKit/FIFO work**, plus the two error checks on
`0x18a4018` that Apple performs before and after the trigger.

Because the operation is read-modify-write, the program cannot be replayed as
a list of "write value" — each write must read the current register first.
That also means these writes will not succeed unless the engine block is
clocked and powered (`PS 0xc000`, `PWGATE`) at the time they run, which is
consistent with `0xc000 := 0x1000000f` preceding them in the existing
sequence.
