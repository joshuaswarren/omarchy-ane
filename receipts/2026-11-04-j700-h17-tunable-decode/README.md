# J700 / h17 — `AppleANETunableApplyFunction` tunable decode (verified)

Reconstructed from `com.apple.driver.AppleT8140ANEHAL` in the MH_FILESET kernelcache.
Every number below was re-derived from the raw bytes and the disassembly in this
session; the decode was cross-checked against the binary's own mangled symbol names
for **all 67 table rows / 62 distinct descriptor elements**, matching 62/62.

* `kernelcache.bin` — SHA-256 `7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`
* `hal_text.S` — objdump of `__TEXT_EXEC.__text`, base VA `0xfffffe0009799040`
* Decoders/checks in this directory: `verify_final.py`, `symcheck.py`, `j700_decode.py`
  (self-checks are asserted, not eyeballed; `symcheck.py` exits non-zero on mismatch)

Offline analysis only: no hardware access, no module load, no device write.

---

## 0. The previous analysis was wrong — exactly how

`0x11378b8` is **not** a register offset and **not** the start of a register list.
It is the **file offset of the HAL's `__DATA_CONST.__const` section**:

```
SEG __DATA_CONST  vm=fffffe000813b828 vsize=0x30b0 fileoff=01137828 filesize=0x30b0
   SEC __const     addr=fffffe000813b8b8 size=0x2fe0 off=011378b8
```

So `VA 0xfffffe000813b8b8` ⟷ `file offset 0x11378b8`. The 12 bytes at that file
offset are **one 12-byte table element**, not a register descriptor list:

```
fileoff 0x11378b8:  w0 = 0x40000000   w1 = 0x0061c3c0   w2 = 0x00180000
```

`w0 = 0x40000000` → **width nibble = 4 (4 bytes / 32 bits), register offset = 0x0**.
The *next* element is 12 bytes later at `0x11378c4` with `w0 = 0x40000004` → offset `0x4`.
Therefore `0x11378b8 + 0xc*k` enumerates **element addresses**, not register offsets.

**Note on the proposed sanity check — it does not catch this bug.** `0x11378b8` is
`18,053,304`, which is *less* than `0x2000000` (`33,554,432`), and it is 4-byte aligned.
A "< 0x2000000 and aligned" filter therefore passes the wrong answer. What actually
discriminates is: (i) the offset comes from `word0 & 0xfffffff`, and element #1's
`word0` is `0x40000000` → offset **0**, not `0x11378b8`; (ii) consecutive "offsets"
12 bytes apart and clustered at `0x1137xxx` are `__DATA_CONST.__const` addresses
(VA `0x813b8b8+`), not aperture addresses; (iii) the mangled symbol names encode
`offset 0x0/0x4/0x8/…`; (iv) the real table's offsets span `0x0 … 0x401c`.

### 0.1 Where the bad table came from — the exact misread

`receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md`
(§C9–C11) states:

> Record format: 8 bytes `{u32 offset, u32 value}` … terminated by an all-zero record;
> the applier ORs `value` into `base+offset` … `0x11378b8 + 0xc·k` for k = 0…33,
> each ← `0x00100000` (34 registers, `0x11378b8 … 0x1137a44`), then `0x1137a50` ← `0x00200000`.

That reads each **8-byte pointer slot** of the level-1 table as a write record:

```
slot at file 0x1137af0 :   b8 78 13 01 | 00 10 00 00
  read as            :   { offset = 0x011378b8, value = 0x00100000 }   <-- wrong
  actually is        :   u64 chained pointer 0x00100000_011378b8
                         target (36 bits) = 0x011378b8 -> element VA 0xfffffe000813b8b8
                         next   (12 bits) = 2          -> stride 8 bytes
```

So the reported "offset" is the fixup's **target**, i.e. the address of the element
inside `__DATA_CONST.__const`, and the reported "value" `0x00100000` is the fixup's
**`next` field** (bit 52), not data — nothing writes `0x00100000` anywhere. The slope
is 8 bytes per entry, not `0xc`; the trailing "`0x1137a50 ← 0x00200000`" is simply the
final slot's `next = 4` (a 16-byte hop to the next table's first fixup). The same
confusion produced the c1ppt addresses `0x1137c88…0x1137d90`, which are in fact the
*legacy* element group, not the c1ppt tables (`0x1137e10…0x1137e90`).

The true write for that slot is
`reg[0x0] = (0x8000001b & 0x800000ff) | (reg[0x0] & ~0x800000ff)`,
and the table is §5.1.

---

## 1. Method: VA ↔ file offset, and how the tables are addressed

Two address spaces matter, and conflating them is what produced the earlier error.

1. **Kernelcache image.** For this cache `VA = 0xfffffe0007004000 + file_offset`.
   Verified two ways: `LC_SEGMENT_64` (`__DATA_CONST` `vmaddr 0xfffffe000813b828`
   / `fileoff 0x1137828`), and `0xfffffe00076203c0 - 0x61c3c0 = 0xfffffe0007004000`,
   where `0xfffffe00076203c0` is `__TEXT.__const` of the same kext.
2. **ANE engine aperture.** All tunable "offsets" are byte offsets inside the ANE
   register aperture. On the online path the base is the function's `uint64_t`
   argument (`cfg->base`, `_tunableh_apply_configuration` `0xfffffe00097a87bc`
   `ldr x25, [x1]`), and the HAL's own compiled-in config accessors hardcode
   `0x400000000` (e.g. `0xfffffe0009799084` `mov x9, #0x400000000`).

### 1.1 Chained-fixup pointers in `__const`

Table slots and element payloads hold `DYLD_CHAINED_PTR`-style rebases. The rule that
reproduces every pointer observed in this kext:

```
VA      = 0xfffffe0007004000 + (u64 & 0xFFFFFFFFFFF) + (((u64 >> 36) & 0xFF) << 56)
next    = (u64 >> 51) & 0xFFF        # stride in 4-byte units
```

Proof by construction: element #2's payload field is `0x00180000_0061c3d0`
(`next = 3` → 12 bytes, i.e. the next element) and resolves to VA `0xfffffe00076203d0`,
which is exactly the `{mask, value, expr}` record for `_fig_4_32_80ff00ff_800c000d_null`
(`0x80ff00ff`, `0x800c000d`). All 67 element payload pointers resolve into
`__TEXT.__const` `0xfffffe00076203c0…0x76206a0`.

---

## 2. (a) Table layout — proof

### 2.1 Three levels, not one

The three constants named in the task are **level-1 tables** (arrays of pointers):

| symbol | VA | extent | slots | elements |
|---|---|---:|---:|---:|
| `_tunableh_af_aneaxi2af_figs_4.figs` | `0xfffffe000813bc10` | `0x018` | 3 | 2 |
| `_tunableh_af_aneaxi2af_figs_3.figs` | `0xfffffe000813bc28` | `0x030` | 6 | 5 |
| `_tunableh_af_aneaxi2af_figs_2.figs` | `0xfffffe000813bc58` | `0x030` | 6 | 5 |
| `_tunableh_af_aneaxi2af_figs_1.figs` | `0xfffffe000813baf0` | `0x120` | 36 | 35 |

Level 1 — **table**: `u64` slots, stride **8**, terminated by a **NULL slot**
(`_tunableh_apply_figs_is_end`, `0xfffffe00097a8888`: `ldr x8,[x0]; cmp x8,#0; cset w0,eq`)
and dereferenced by `_tunableh_apply_figs_get` (`0xfffffe00097a887c`: `ldr x0,[x0]`).
Extents tile exactly into the next table/symbol, which independently confirms the
counts: `0x813baf0+0x120 = 0x813bc10`, `+0x18 = 0x813bc28`, `+0x30 = 0x813bc58`,
`+0x30 = 0x813bc88` (= first `_fig_*` symbol of the legacy group).

Level 2 — **element** (12 bytes, `stride 0xc`, packed and shared between tables):

```
+0x0  u32 word0 : bits 31:28 = width in BYTES (1/2/4/8); bits 27:0 = engine offset
+0x4  u64 fixup : -> level-3 record
```

Level 3 — **record**, `{mask, value, expression-callback}` sized by the element width:

```
+0        mask      (width bytes)
+width    value     (width bytes)
+2*width  u64 expr  (callback address, 0 = no expression)
```
→ 16 bytes for width 4 (`mask` +4, `value` +4, `expr` +8). `__const` records are laid
out 16 bytes apart: `0x76203c0, 0x76203d0, 0x76203e0, …`.

### 2.2 Ground truth from the symbol names

Every element carries its own mangled name of the form
`_fig_<offset>_<widthbits>_<mask>_<value>_<callback>`. Example, element #2:

```
symbol  _fig_4_32_80ff00ff_800c000d_null
bytes   0x11378c4:  40000004  001800000061c3d0
decode  width 4 (32 bits), offset 0x4, record 0x76203d0 -> {0x80ff00ff, 0x800c000d}
```

The decoded `offset / width / mask / value` equals the symbol name for **62 of 62**
distinct elements (67 table rows) across all ten tables in this kext — 0 mismatches,
all four fields, checked mechanically by `symcheck.py`. The only mismatch risk would
be a byte/bit confusion in the width field; the name field is in bits (`32`), the
word0 nibble is in bytes (`4`) — `32 = 8*4`.

### 2.3 Which element form is live (this is the third trap)

`_tunableh_apply` chooses the array stride and element-field form from a 2-byte flag
entry selected by `_tunableh_module_finalize(cfg, id)`:

```
0x97a809c  lsl  x8, x1, #1
0x97a80c0  str  x10, [x0, #48]      ; cfg->0x30 = 0xfffffe0007620a30 + 2*id
0x97a80c4  cmp  x1, #2
0x97a80d0  str  x8, [x0, #56]       ; cfg->0x38 = -1  (=> 0x30 already final)
```
The flag table is at `__TEXT.__const 0xfffffe0007620a30` (file `0x61ca30`):

```
offset :  0x30 0x31 0x32 0x33 0x34 0x35 0x36 0x37 0x38
bytes  :  00   00   01   00   01   01   00   00   01
entry  :  e0=----   e1=----   e2=----   e3=----   e4=----
byte0  :  00   00   01   00   01       (per 2-byte entry)
byte1  :  00   01   01   00   01
```
Every `af_aneaxi2af` applier calls `_tunableh_module_finalize` with **`w1 = 2`**
(`0x9799440`, `0x97994e4`, `0x9799588`: `mov w1, #2`), i.e. **entry 2 = bytes
`{0x01, 0x01}` at `0xfffffe0007620a34/35`**.

`_tunableh_apply` then takes the **byte1** test first:

```
0x97a85ac  ldr  x8, [x19, #48]        ; cfg->0x30
0x97a85b0  ldrb w9, [x8, #1]          ; flag byte1 = 0x01
0x97a85b8  adrp x16, ... 0x97a8888    ; is_end  = NULL check
0x97a85d8  tbz  w9, #0, 0x97a85e8     ; not taken
0x97a85dc  mov  x27, x16              ; get     = _tunableh_apply_figs_get (deref)
0x97a85e0  mov  w28, #8               ; stride  = 8
```
So the live form for this family is the **8-byte pointer array / NULL terminator /
12-byte pointer-form elements**, *not* the 12-byte inline "legacy" array
(`0x97a85e8…0x97a8618`, `w28 = 0xc`, `_tunableh_apply_legacy_figs_*`).

Byte0 of the same entry (`0x34 = 0x01`) is what makes the element helpers dereference
`elem+4` instead of reading mask/value inline (`_tunableh_configuration_mask`
`0x97a8108`: `0x97a8110 ldr x9,[x1,#48]; 0x97a8114 ldrb w9,[x9]; 0x97a8118 tbz w9,#0`).
Both bits are 1, so element = `{u32 word0; u64 -> record}`.

---

## 3. (b) Applier semantics — masked read-modify-write of an expression

`_tunableh_apply_configuration` (`0xfffffe00097a876c`), argument order
`(x0 = apply-site identity, x1 = cfg, x2 = element, x3 = &errflag)`:

```
0x97a8790  mov  x19, x2            ; x19 = element
0x97a8794  mov  x20, x1            ; x20 = cfg
0x97a8798  mov  x21, x0            ; x21 = apply-site token (not the HW device)
0x97a879c  ldr  x8, [x1, #24]      ; cfg->0x18 : writer override
0x97a87a4  add  x16, x16, #0x930   ; 0x97a8930 = _tunableh_write
0x97a87b4  csel x16, x16, x8, eq   ; writer = cfg->0x18 ?: _tunableh_write
0x97a87b8  str  x16, [sp, #8]
0x97a87bc  ldr  x25, [x1]          ; x25 = cfg->0x0 = ENGINE BASE
0x97a87c0  ldr  w8, [x2]           ; element word0
0x97a87c4  and  x26, x8, #0xfffffff; x26 = offset
0x97a87c8  lsr  w22, w8, #28       ; w22 = width
```

The four operand sources, in evaluation order:

| call | address | function | returns |
|---|---|---|---|
| `0x97a87d0` | `bl 0x97a83f8` | `_tunableh_configuration_applied_value` | **current register contents** at `base+offset`, width-sized |
| `0x97a87e0` | `bl 0x97a8108` | `_tunableh_configuration_mask` | mask |
| `0x97a87f4` | `bl 0x97a81d0` | `_tunableh_configuration_get_value` | expression callback result |
| `0x97a8804` | `bl 0x97a82ec` | `_tunableh_configuration_value` | literal value |

Combine:

```
0x97a87e8  bic  x27, x23, x0       ; x27 = applied & ~mask      (preserve)
0x97a8808  orr  x8,  x0,  x23      ; value | get_value()
0x97a880c  and  x8,  x8,  x24      ; ... & mask
0x97a8810  add  x2,  x25, x26      ; addr = base + offset
0x97a8814  orr  x4,  x8,  x27      ; new  = ((value|expr) & mask) | (old & ~mask)
0x97a8820  mov  x3,  x22           ; width
0x97a8858  braa x16, x17           ; tail-call writer(x0,x1=cfg,x2=addr,x3=width,x4=new)
```

### Answer to the explicit question

It is **not** an OR into `base+offset`, **not** a plain write, and **not** a mask of a
single literal alone. It is a **read-modify-write of a masked merge whose "new" bits are
the OR of a literal and an optional expression callback**:

```
reg[off] = ((value() | get_value()) & mask) | (reg[off] & ~mask)
```

Proof that a read really happens: `_tunableh_configuration_applied_value`
(`0xfffffe00097a83f8`) computes `cfg->0x0 + (word0 & 0xfffffff)` and loads from it —
width 4 case:

```
0x97a8470  ldr  x9, [x1]              ; base
0x97a8474  and  x8, x8, #0xfffffff    ; offset
0x97a8478  add  x8, x9, x8            ; base+offset
0x97a847c  ldr  w0, [x8]              ; current value (width 1/2/8 at 0x845c/0x8424/0x8448)
```

Proof that the final store itself is unmasked: `_tunableh_write` (`0xfffffe00097a8930`)
is a bare width-dispatched store — `0x97a896c strb w4,[x2]`, `0x97a894c strh w4,[x2]`,
`0x97a8974 str w4,[x2]`, `0x97a8964 str x4,[x2]`, else `0x97a8978 ret`. The mask is
applied only in the applier, above.

Proof that mask/value/expr come from the level-3 record (flag byte0 = 1), quoted for
width 4:

* **mask** — `_tunableh_configuration_mask` `0x97a8108`: `0x97a8120 ldr w9,[x8]` /
  `0x97a8124 lsr w9,w9,#28` (element width) → `0x97a8178 b.eq 0x97a81bc`;
  `0x97a81bc ldur x8,[x8,#4]` (record pointer) ; `0x97a81c0 ldr w0,[x8]` → `record+0`.
* **value** — `_tunableh_configuration_value` `0x97a82ec`: `0x97a835c b.eq 0x97a83a0`;
  `0x97a83a0 ldur x8,[x8,#4]` ; `0x97a83a4 ldr w0,[x8,#4]` → `record+width`.
* **expression pointer** — `_tunableh_configuration_get_value` `0x97a81d0`:
  `0x97a8248 b.eq 0x97a82bc`; `0x97a82bc ldur x8,[x8,#4]` ; `0x97a82c0 ldr x8,[x8,#8]`
  → `record+2*width`; `0x97a82c4 cbz x8,0x97a82b4` (`mov x0,#0`), else `0x97a82c8
  mov x0,x1` (cfg) and `0x97a82d0 blraa x8, x17` calls it. A NULL pointer yields 0.

For **every** element in every table reached here the expression field is `0`
(verified: 62/62), so the write reduces to
`reg[off] = (value & mask) | (reg[off] & ~mask)`.

### 3.1 Loop and the `&errflag`

`_tunableh_apply` (`0xfffffe00097a84a4`) per element:

```
0x97a86b4  blraa x26          ; is_end(slot)
0x97a86c0  blraa x27          ; slot -> element
0x97a86e0  blraa x23          ; pre-hook  cfg->0x8  (default 0x97a849c = nop)
0x97a86fc  blraa x25          ; APPLIER   cfg->0x10 (default 0x97a876c)
0x97a8714  blraa x24          ; post-hook cfg->0x20 (default 0x97a849c = nop)
0x97a8720  add  x21, x21, x28 ; advance stride (8)
```

`cfg->0x8`, `cfg->0x10`, `cfg->0x18`, `cfg->0x20` are chosen at `0x97a854c`,
`0x97a858c`, `0x97a85a8`-region (`csel x23/x25/x24, <builtin>, cfg->field, eq`).
The online caller zeroes those fields, so the builtins are used:
`0x97a8540 add #0x49c` → `_tunableh_apply_nop_callback`, `0x97a857c add #0x76c` →
`_tunableh_apply_configuration`. The `x3 = &errflag` byte (`0x97a8568`,
`0x97a86e4`) is only ever *read*; nothing on this path sets it, so no early exit occurs.

---

## 4. Which of the two tunable functions runs (online vs Legacy)

Both are methods of the same class, and both are proven present:

| VA | symbol |
|---|---|
| `0xfffffe00097a9008` | `__ZN16AppleT8140ANEHAL28AppleANETunableApplyFunctionEy` → `AppleANETunableApplyFunction(uint64_t)` |
| `0xfffffe00097a91e4` | `__ZN16AppleT8140ANEHAL35AppleANETunableApplyFunction_LegacyEyyy` → `..._Legacy(uint64_t,uint64_t,uint64_t)` |

The **only** dispatcher is
`__ZN16AppleT8140ANEHAL20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_`
(`0xfffffe00097a94b8`) — `callPlatformFunction(const OSSymbol *key, bool, void*, void*, void*, void*)`:

```
0x97a94c4  adrp x8, 0xfffffe000c445000
0x97a94c8  ldr  x8, [x8, #2168]        ; g key_online  = [0xfffffe000c445878]
0x97a94cc  cmp  x1, x8
0x97a94d0  b.eq 0x97a94f8              ; --- ONLINE ---
0x97a94d4  adrp x8, 0xfffffe000c445000
0x97a94d8  ldr  x8, [x8, #2176]        ; g key_legacy  = [0xfffffe000c445880]
0x97a94dc  cmp  x1, x8
0x97a94e0  b.ne 0x97a9500              ; unknown key -> return 0, do nothing
0x97a94e4  mov  x1, x3                 ; --- LEGACY ---
0x97a94e8  mov  x2, x4
0x97a94ec  mov  x3, x5
0x97a94f0  bl   0xfffffe00097a91e4     ; _Legacy(a,b,c)
0x97a94f8  mov  x1, x3                 ; --- ONLINE ---
0x97a94fc  bl   0xfffffe00097a9008     ; ApplyFunction(a)
```

The two globals are `OSSymbol`s built at load time from the kext's own C strings
(`__GLOBAL__sub_I_AppleT8140ANEHAL.cpp`, `0xfffffe00097a9514`):

```
0x97a9520  adrp x0, 0xfffffe0007620000
0x97a9524  add  x0, x0, #0xb0a         ; "AppleANETunableApplyFunction"       @0x7620b0a
0x97a9528  bl   0xfffffe000bb6b5f8     ; OSSymbol::withCString-shaped call
0x97a9530  str  x0, [x8, #2168]        ; -> 0xfffffe000c445878
0x97a9538  add  x0, x0, #0xb27         ; "AppleANETunableApplyFunctionLegacy" @0x7620b27
0x97a953c  bl   0xfffffe000bb6b5f8
0x97a9544  str  x0, [x8, #2176]        ; -> 0xfffffe000c445880
```

**Conclusion (proven):** the choice between the two functions is made by *name* at the
`callPlatformFunction` boundary — whichever of the two symbol names the platform
(the ADT/DT property set) passes in. There is **no branch on `ane-type` and no
comparison against family `0xe0`** anywhere: a scan of both `__text` sections for
`cmp/mov …, #0xe0` found nothing, and the only `0x200` compares in the interface kext
are unrelated (`0x8fac874`, `0x8fb7fe8`, …). The premise "ane-type 0x200 → internal
family 0xe0" is therefore **not corroborated** by this binary; treat `0xe0` as an
external convention (see §7).

**Robustness of §5 either way (proven):** `_Legacy` calls the *same three*
`af_aneaxi2af` appliers in the same order —

```
0x97a9268  bl 0xfffffe000979942c   ; default_apply_to
0x97a9270  bl 0xfffffe0009799574   ; secure_lock_wr_dis_apply_to
0x97a9278  bl 0xfffffe00097994d0   ; secure_lock_lock_apply_to
```
— exactly the calls the online function makes at `0x97a9128`, `0x97a9130`, `0x97a9138`.
So **the `af_aneaxi2af` write list in §5 is identical on both paths**; `_Legacy`
additionally applies other families (§7.11), which is the only difference that matters.

---

## 5. (c) Complete verified write list — online path

Entry point `0xfffffe00097a9008`, `B = arg1` (engine aperture base).

### 5.0 Phase 0 — CE handshake, before any tunable (not table-driven)

```
0x97a906c  ldr  x9, [x8]      ; r = *(u64*)(B+0x18a4010)
0x97a907c  ldr  x8, [x8]      ; m = *(u64*)(B+0x18a4018)
0x97a9080  and  x9, x9, #3
0x97a9084  eor  x21, x9, #3   ; k = (r & 3) ^ 3
0x97a9088  tst  x21, x8
0x97a908c  b.ne 0x97a91a4     ; -> log "CE in error state", RETURN, no tunables
0x97a9090  cmp  x9, #3
0x97a9094  b.eq 0x97a9114     ; already 3 -> skip the kick
0x97a90a4  str  x21, [x8]     ; *(u64*)(B+0x18a4000) = k
   ... poll up to 5001x (w22 = 0x1389), delay(10) @0x97a90cc: (*(u64*)(B+0x18a4008) & 3) == k
0x97a9108  tst  x8, x21
0x97a910c  b.ne 0x97a91c4     ; -> log "Fail to trigger CE", RETURN
0x97a9110  str  x21, [x9]     ; *(u64*)(B+0x18a4008) = k
```

| step | engine offset | width | operation | value |
|---|---|---:|---|---|
| 0a | `0x18a4000` | 8 | plain write, **only if** `(B[0x18a4010] & 3) != 3` | `((B[0x18a4010] & 3) ^ 3)` |
| 0b | `0x18a4008` | 8 | plain write, after poll success | `((B[0x18a4010] & 3) ^ 3)` |

Both are `< 0x2000000` (`0x18a4000`, `0x18a4008`). Guard `m = B[0x18a4018]` must satisfy
`k & m == 0` at both points, else the function returns an error and applies nothing.

### 5.1 AF group — `af_aneaxi2af`, base `B`

Applied in this order: table `0x813baf0` (always, 35 writes), then the mode-dependent
set. Element order is table order (stride 8, NULL-terminated).

**Always applied — `_tunableh_af_aneaxi2af_figs_1.figs` @ `0xfffffe000813baf0`
(extent `0x120`, 35 entries + NULL @ `0x813bc08`), applied at `0x97a9118` via
`0x97990e0`:**

| # | engine offset | width | mask | value | element VA | mask/value rec |
|---:|---|---:|---|---|---|---|
| 1 | `0x00000` | 4 | `0x800000ff` | `0x8000001b` | `0xfffffe000813b8b8` | `0xfffffe00076203c0` |
| 2 | `0x00004` | 4 | `0x80ff00ff` | `0x800c000d` | `0xfffffe000813b8c4` | `0xfffffe00076203d0` |
| 3 | `0x00008` | 4 | `0x80ff00ff` | `0x800c000d` | `0xfffffe000813b8d0` | `0xfffffe00076203d0` |
| 4 | `0x0000c` | 4 | `0x80ff00ff` | `0x800c000d` | `0xfffffe000813b8dc` | `0xfffffe00076203d0` |
| 5 | `0x00010` | 4 | `0x80ff00ff` | `0x80000003` | `0xfffffe000813b8e8` | `0xfffffe00076203e0` |
| 6 | `0x00014` | 4 | `0x80ff00ff` | `0x80000003` | `0xfffffe000813b8f4` | `0xfffffe00076203e0` |
| 7 | `0x00018` | 4 | `0x800000ff` | `0x80000013` | `0xfffffe000813b900` | `0xfffffe00076203f0` |
| 8 | `0x0001c` | 4 | `0x800000ff` | `0x80000020` | `0xfffffe000813b90c` | `0xfffffe0007620400` |
| 9 | `0x00020` | 4 | `0x80ff00ff` | `0x8012000c` | `0xfffffe000813b918` | `0xfffffe0007620410` |
| 10 | `0x00024` | 4 | `0x80ff00ff` | `0x80180006` | `0xfffffe000813b924` | `0xfffffe0007620420` |
| 11 | `0x00028` | 4 | `0x80ff00ff` | `0x80070003` | `0xfffffe000813b930` | `0xfffffe0007620430` |
| 12 | `0x0002c` | 4 | `0x80ff00ff` | `0x80070003` | `0xfffffe000813b93c` | `0xfffffe0007620430` |
| 13 | `0x00030` | 4 | `0x800000ff` | `0x80000015` | `0xfffffe000813b948` | `0xfffffe0007620440` |
| 14 | `0x00034` | 4 | `0x800000ff` | `0x80000011` | `0xfffffe000813b954` | `0xfffffe0007620450` |
| 15 | `0x00038` | 4 | `0x800000ff` | `0x80000011` | `0xfffffe000813b960` | `0xfffffe0007620450` |
| 16 | `0x00100` | 4 | `0x000000ff` | `0x000000ff` | `0xfffffe000813b96c` | `0xfffffe0007620460` |
| 17 | `0x00108` | 4 | `0x00000006` | `0x00000000` | `0xfffffe000813b978` | `0xfffffe0007620470` |
| 18 | `0x00304` | 4 | `0x3f3f0000` | `0x02020000` | `0xfffffe000813b984` | `0xfffffe0007620480` |
| 19 | `0x00314` | 4 | `0x80000000` | `0x80000000` | `0xfffffe000813b990` | `0xfffffe0007620490` |
| 20 | `0x0031c` | 4 | `0x80000000` | `0x80000000` | `0xfffffe000813b99c` | `0xfffffe0007620490` |
| 21 | `0x00324` | 4 | `0x80000000` | `0x80000000` | `0xfffffe000813b9a8` | `0xfffffe0007620490` |
| 22 | `0x00408` | 4 | `0xffff0020` | `0x00120020` | `0xfffffe000813b9b4` | `0xfffffe00076204a0` |
| 23 | `0x00700` | 4 | `0x00000400` | `0x00000400` | `0xfffffe000813b9c0` | `0xfffffe00076204b0` |
| 24 | `0x00748` | 4 | `0x07ff07ff` | `0x00200030` | `0xfffffe000813b9cc` | `0xfffffe00076204c0` |
| 25 | `0x00800` | 4 | `0x00000400` | `0x00000400` | `0xfffffe000813b9d8` | `0xfffffe00076204b0` |
| 26 | `0x00848` | 4 | `0x07ff07ff` | `0x00100030` | `0xfffffe000813b9e4` | `0xfffffe00076204d0` |
| 27 | `0x00900` | 4 | `0x00000400` | `0x00000400` | `0xfffffe000813b9f0` | `0xfffffe00076204b0` |
| 28 | `0x00948` | 4 | `0x07ff07ff` | `0x01000100` | `0xfffffe000813b9fc` | `0xfffffe00076204e0` |
| 29 | `0x00b00` | 4 | `0xffffffff` | `0x7fffffff` | `0xfffffe000813ba08` | `0xfffffe00076204f0` |
| 30 | `0x00b04` | 4 | `0x00000100` | `0x00000100` | `0xfffffe000813ba14` | `0xfffffe0007620500` |
| 31 | `0x04008` | 4 | `0x00000001` | `0x00000000` | `0xfffffe000813ba20` | `0xfffffe0007620510` |
| 32 | `0x0400c` | 4 | `0x00000001` | `0x00000000` | `0xfffffe000813ba2c` | `0xfffffe0007620510` |
| 33 | `0x04014` | 4 | `0x00000001` | `0x00000000` | `0xfffffe000813ba38` | `0xfffffe0007620510` |
| 34 | `0x04018` | 4 | `0x00000001` | `0x00000000` | `0xfffffe000813ba44` | `0xfffffe0007620510` |
| 35 | `0x0401c` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813ba50` | `0xfffffe0007620520` |

**Mode A — device descriptor all-zero (this is what runs; see §5.4).**
Extra, appended after the 35 above, from `0x97990e0` `0x9799140` →
`figs_4.figs` @ `0xfffffe000813bc10` (extent `0x018`, 2 entries + NULL @ `0x813bc20`):

| # | engine offset | width | mask | value | element VA | mask/value rec |
|---:|---|---:|---|---|---|---|
| 36 | `0x00338` | 4 | `0xffffffff` | `0x00050020` | `0xfffffe000813bad4` | `0xfffffe0007620530` |
| 37 | `0x0033c` | 4 | `0xffffffff` | `0x000a0030` | `0xfffffe000813bae0` | `0xfffffe0007620540` |

Gate (`0x979911c…0x9799130`):
`ldr x8,[dev,#8]; cbnz -> skip` · `ldrb w8,[dev,#0x20]; cbnz -> skip` ·
`ldrb w8,[dev,#0x21]; cbz -> apply`.

**Mode B — `dev[8]==1 && dev[0x20]==0 && dev[0x21]==1`.** Replaces rows 36–37 with
10 writes: `0x9799574` → `0x9799344` → `figs_2.figs` @ `0xfffffe000813bc58`
(extent `0x030`, 5 entries + NULL @ `0x813bc80`), then `0x97994d0` → `0x9799220` →
`figs_3.figs` @ `0xfffffe000813bc28` (extent `0x030`, 5 entries + NULL @ `0x813bc50`):

| # | engine offset | width | mask | value | element VA | mask/value rec |
|---:|---|---:|---|---|---|---|
| 36 | `0x00df8` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813ba5c` | `0xfffffe0007620520` |
| 37 | `0x00ef8` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813ba74` | `0xfffffe0007620520` |
| 38 | `0x00ff8` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813ba8c` | `0xfffffe0007620520` |
| 39 | `0x010f8` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813baa4` | `0xfffffe0007620520` |
| 40 | `0x011f8` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813babc` | `0xfffffe0007620520` |
| 41 | `0x00dfc` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813ba68` | `0xfffffe0007620520` |
| 42 | `0x00efc` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813ba80` | `0xfffffe0007620520` |
| 43 | `0x00ffc` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813ba98` | `0xfffffe0007620520` |
| 44 | `0x010fc` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813bab0` | `0xfffffe0007620520` |
| 45 | `0x011fc` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813bac8` | `0xfffffe0007620520` |

Any other combination of `dev[8]/dev[0x20]/dev[0x21]` → only the 35 always-applied writes.

Note rows 36–40 and 41–45 deliberately interleave: `figs_2` selects the `…f8` lanes and
`figs_3` the `…fc` lanes of the same five 0x100-byte register groups.

### 5.2 Conditional `c1ppt_rc` group — base `B + 0x197c000`

Entered only if `[this+0x90] & 1` (`0x97a913c ldrb w8,[x20,#144]; 0x97a9140 tbz w8,#0,0x97a9188`).
That byte is set in `AppleT8140ANEHAL::start` from the kernel boot argument
**`pmgr-c1ppt`**:

```
0x97a8f2c  strb wzr, [x19, #144]       ; this->0x90 = 0            (default)
0x97a8f34  add  x0, x0, #0xc21         ; "pmgr-c1ppt"  @0xfffffe0007620c21
0x97a8f38  add  x1, sp, #0xc           ; &u32 result
0x97a8f3c  mov  w2, #4
0x97a8f40  bl   0xfffffe000bccad94     ; boot-arg parser (name, &val, 4)
0x97a8f44  cbz  w0, 0x97a8f7c          ; absent -> leave 0
0x97a8f48  ldr  w8, [sp, #12]
0x97a8f4c  cmp  w8, #0
0x97a8f50  cset w8, ne
0x97a8f54  strb w8, [x19, #144]        ; this->0x90 = (val != 0)
```

So by default (no `pmgr-c1ppt=`) the whole group is skipped. Its cfg base is
`B + 0x197c000` (`0x97a9144 mov w8,#0xc000; 0x97a9148 movk w8,#0x197,lsl#16;
0x97a9154 add x8,x19,x8; 0x97a9164 str x8,[sp,#32]`).

Applied in this order when enabled: `0x979b144` (`figs_27` @ `0x813be50`, 8) →
`0x979b098` (`0x979acec` guard → `figs_26` @ `0x813be30` **or** `figs_28` @ `0x813be40`) →
`0x979b2fc` (`figs_3` @ `0x813be20`, 1) → `0x979b220` (`figs_4` @ `0x813be10`, 1).

`figs_41` (`0x813be98`, 8 entries) is **not** on this path: its wrapper `0x979bb78` is
referenced only from `0x979b4ac / 0x979b534 / 0x979b7ec / 0x979b874`, i.e. the
`…subsystem3_graphics_agx…c1ppt_rc_mgpu` family. Do not include it in a J700 ANE list.

Default (`dev[8]==0`) gives 11 writes:

| # | engine offset | width | mask | value | element VA | mask/value rec |
|---:|---|---:|---|---|---|---|
| 1 | `B+0x197c000+0x00020` | 4 | `0x0000ffff` | `0x00001068` | `0xfffffe000813bd30` | `0xfffffe0007620620` |
| 2 | `B+0x197c000+0x00024` | 4 | `0x0000ffff` | `0x00000c4c` | `0xfffffe000813bd3c` | `0xfffffe0007620630` |
| 3 | `B+0x197c000+0x00028` | 4 | `0xffffffff` | `0x00000000` | `0xfffffe000813bd48` | `0xfffffe0007620640` |
| 4 | `B+0x197c000+0x0002c` | 4 | `0x000000ff` | `0x00000003` | `0xfffffe000813bd54` | `0xfffffe0007620650` |
| 5 | `B+0x197c000+0x00040` | 4 | `0x000003ff` | `0x000000bf` | `0xfffffe000813bcc4` | `0xfffffe00076205a0` |
| 6 | `B+0x197c000+0x00044` | 4 | `0x000003ff` | `0x0000019a` | `0xfffffe000813bd60` | `0xfffffe0007620660` |
| 7 | `B+0x197c000+0x00048` | 4 | `0x0001ffff` | `0x00000000` | `0xfffffe000813bd6c` | `0xfffffe0007620670` |
| 8 | `B+0x197c000+0x00060` | 4 | `0x00001f1f` | `0x00000103` | `0xfffffe000813bce8` | `0xfffffe00076205d0` |
| 9 | `B+0x197c000+0x00004` | 4 | `0x00000003` | `0x00000000` | `0xfffffe000813bd24` | `0xfffffe0007620610` |
| 10 | `B+0x197c000+0x03ff0` | 4 | `0x007fffff` | `0x0077ebf9` | `0xfffffe000813bd00` | `0xfffffe00076205f0` |
| 11 | `B+0x197c000+0x03ffc` | 4 | `0x00000001` | `0x00000001` | `0xfffffe000813bd0c` | `0xfffffe0007620520` |

Row 9 is the mode-dependent one (`0x979acec`), and it is the *only* difference:

| device descriptor | table applied at step 2 | row 9 |
|---|---|---|
| `dev[8]==0` (bss default) | `figs_26` @ `0x813be30` | `0x4` mask `0x3` value `0x0` |
| `dev[8]==1 && dev[0x20]==0 && dev[0x21]==1` | `figs_28` @ `0x813be40` | `0x4` mask `0x3` value `0x1` |
| `dev[8]==1 && dev[0x20]==1 && dev[0x21]==0` | `figs_26` | `0x4` mask `0x3` value `0x0` |
| `dev[8]==1 && dev[0x20]∉{0,1}` or `dev[0x21]∉{0,1}` | none | — |
| `dev[8]∉{0,1}` | none | — |

So the `c1ppt_rc` group is **11 writes in either live mode** — never 13; only row 9's
value changes. Fragment of `0x979acec`: `0x979ad04 ldr x8,[x0,#8]; 0x979ad08 cmp x8,#1;
0x979ad0c b.eq 0x979ad4c` → `0x979ad14`/`0x979ad18` = `figs_26`; `0x979ad4c ldrb w8,[x0,#32];
0x979ad50 cmp w8,#1; 0x979ad54 b.eq 0x979ad80`; `0x979ad5c ldrb w8,[x0,#33];
0x979ad60 cmp w8,#1; 0x979ad64 b.ne 0x979ad84`; `0x979ad68`/`0x979ad6c` = `figs_28`.

### 5.3 Totals

| group | writes |
|---|---:|
| Phase 0 CE | 0–2 (2 typical: `0x18a4000`, `0x18a4008`) |
| AF, dev all-zero (A) | **37** (35 + 2) |
| AF, dev `1/0/1` (B) | **45** (35 + 5 + 5) |
| c1ppt (only if `pmgr-c1ppt` ≠ 0) | **+11** in either mode |

Grand total: Mode A without the boot-arg = **37**; Mode A with it = **50**;
Mode B without = **45**; Mode B with = **58**.

### 5.4 Which mode is live — evidence

`_tunableh_device` (`0xfffffe00097a808c`) returns `0xfffffe000c445850`
(`0x97a8090 adrp x0, 0xfffffe000c445000; 0x97a8094 add x0, x0, #0x850`), which lies in
`__DATA.__bss` (`addr 0xfffffe000c4449b0`, `size 0xed8`, i.e. up to `0xfffffe000c445888`).
Only `_tunableh_init` (`0xfffffe00097a806c`) writes it (copies 40 bytes from its argument).

**`_tunableh_init` has no caller in this kernelcache**: no `bl 0xfffffe00097a806c`
anywhere in `__TEXT_EXEC.__text`, and no chained pointer to it in any data section.
Therefore the descriptor stays bss-zero → `dev[8]==0`, `dev[0x20]==0`, `dev[0x21]==0` →
**Mode A**, i.e. 35 + 2 = **37 AF writes**.

---

## 6. (d) Self-check results

Produced by `verify_final.py` / `symcheck.py` (asserted, not prose):

```
offsets < 0x2000000            : PASS (max 0x401c)
width==4 offsets 4-byte aligned: PASS
widths subset of {1,2,4,8}     : PASS (observed [4])
count == extent/8 - 1          : PASS
no offset >= 0x2000000         : PASS (weak test — see caveat)
all elements inside HAL __const: PASS
expression callbacks present   : 0 (all zero => pure masked RMW)
table rows=67 distinct elements=62 checked=62 mismatches=0
```

**Caveat on the numeric bound — it is necessary, not sufficient.** `0x11378b8`
is `18,053,304 < 0x2000000` and 4-byte aligned, so the bound+alignment test alone
**passes** the earlier wrong answer. The checks that actually discriminate are the
ones below (symbol-name equality and offset provenance).

* Every decoded `(offset, width, mask, value)` equals the mangled symbol name for
  **62/62 distinct elements** (67 table rows), 0 mismatches over all four fields.
  This validates the width/offset split independently of how `word0` is read.
* Provenance: every offset in §5 was read out of a `word0 & 0xfffffff` field at a
  12-byte element inside `__DATA_CONST.__const`; no offset was taken from an address.
  Element #1 at `0xfffffe000813b8b8` yields **offset `0x0`**, the single clearest
  refutation of the "`0x11378b8 + 0xc*k`" reading.
* Table extents tile with no gaps or overlaps and end exactly on the next symbol:
  `0x813baf0+0x120=0x813bc10`, `+0x18=0x813bc28`, `+0x30=0x813bc58`, `+0x30=0x813bc88`,
  `0x813be10+0x10=0x813be20`, `+0x10=0x813be30`, `+0x10=0x813be40`, `+0x10=0x813be50`,
  `+0x48=0x813be98`, `+0x48=0x813bee0`.
* Offsets in §5 never exceed `0x401c`, each is the low 28 bits of a `word0` at a known
  element address, and `0x11378b8` never appears in an offset field.
* Terminator slots (all NULL): `0x813bc08`, `0x813bc20`, `0x813bc50`, `0x813bc80`,
  `0x813be18`, `0x813be28`, `0x813be38`, `0x813be48`, `0x813be90`, `0x813bed8`.
* Shared-record consistency: e.g. `0x7620510` is referenced by elements
  `0x813ba20/ba2c/ba38/ba44` and `0x7620520` by fifteen elements — consistent, since
  only the offset differs.

Note: this directory also contains `decode_tunables.py` and `selfcheck.txt` written by
a concurrent/earlier process (timestamps predate this report, provenance not
attributable from this session). They describe the same layout and their 59 rows are
exactly the on-path subset here (they likewise exclude `figs_41`); this report does not
depend on them and its numbers are the ones verified by `symcheck.py`.
`README.concurrent-writer.md` preserves another report that was written to this path
during this session.

---

## 7. (e) Unresolved / not proven, and what is inference

**Proven from bytes + disassembly**

1. `0x11378b8` = file offset of `__DATA_CONST.__const` (VA `0xfffffe000813b8b8`);
   the 12 bytes there are element #1 with `word0 = 0x40000000` → offset `0x0`, width 4.
2. The three-level structure (§2), the 8-byte/NULL-terminated table form and the
   12-byte pointer-form element, selected by the `{0x01,0x01}` flag entry at
   `0xfffffe0007620a34` for `id = 2`.
3. The applier is a masked read-modify-write whose new bits are
   `(value | expression) & mask`; the final store is an unmasked width-sized store (§3).
4. Every offset/width/mask/value in §5, all 62 distinct elements / 67 rows, plus the
   terminator addresses.
5. The online/legacy dispatch is a name comparison in `callPlatformFunction`
   (`0x97a94c4…0x97a9508`) against OSSymbols built from
   `"AppleANETunableApplyFunction"` / `"AppleANETunableApplyFunctionLegacy"`.
6. `_tunableh_init` is unreferenced, so the mode descriptor is bss-zero → Mode A.
7. The `c1ppt_rc` gate `this+0x90` is the `pmgr-c1ppt` boot argument (`0x97a8f2c…0x97a8f54`),
   default 0 → group skipped.

**Inference (stated as such, not proven)**

8. **`B == 0x400000000`.** The online function takes `B` from its caller; the value is
   not fixed in the HAL. The HAL's own compiled-in accessors hardcode `0x400000000`
   (`0x9799084`, `0x9799478`, `0x979951c`, …) and the aperture is `0x2000000`, so
   `B = 0x400000000` is strongly indicated but not proven here. If `B` differs, all
   offsets are unchanged and stay relative to `B`.
9. **Which key the platform passes.** Nothing in the kernelcache decides it — the DT/ADT
   does. I proved the branch that reacts to the choice, not the choice itself. It does
   not affect §5, because `_Legacy` applies the same `af_aneaxi2af` tables
   (`0x97a9268/0x97a9270/0x97a9278`).
10. **The `ane-type 0x200 → family 0xe0` premise.** Not corroborated: there is no
    `0xe0` comparison in either `__text` and no `ane-type` value feeds the tunable
    selection. The only `ane-type` consumer located is the SoC-config/HAL-class picker
    in the interface kext (`initializeANESoCConfig`; error string "No valid ane-type in
    EDT and no boot-arg override, aborting init" @ `0xfffffe00073e07fe`). Treat `0xe0`
    as external naming; a decode that depends on it needs the ADT to confirm.
11. **`_Legacy`'s extra families** are enumerated by call site only, not decoded here:
    `0x97a57ac` / `0x97a53d0` / `0x97a5318` (soc_dpe_temp, soc_dpe_lee),
    `0x97a7d18` / `0x97a7c2c` / `0x97a7ef0` / `0x97a7e04` (ane_dpe_cpms),
    `0x97a67b4` / `0x97a66f8` / `0x97a698c` / `0x97a68a0` (ane_dpe_sys),
    `0x979b144` / `0x979b098` / `0x979b2fc` / `0x979b220` (c1ppt_rc),
    `0x979d6f4` (mcw).
12. **Third element form not decoded.** If a config id gives flag bytes `{0,0}`,
    `_tunableh_apply` takes the stride from a table at `0xfffffe0007620a80`
    (`0x97a8658…0x97a8674`, `sub x8, x8, #1; lsl x8,x8,#3`) with mask/value read
    inline at `elem+4`. No table on this path uses it (`{0x01,0x01}` is selected), so it
    was not decoded; a decoder must still reject that case rather than guess.
13. Whether any element offset falls inside a *reserved* part of the aperture is a
    device-semantics question. The offsets are arithmetically valid (`0x401c` max for
    AF, `0x19bfffc` max for c1ppt) but I make no claim about register behaviour.

---

## 8. Reproduction

```sh
sha256sum /var/tmp/neo/kernelcache.bin    # 7d4ca8ba...44fe
cd receipts/2026-11-04-j700-h17-tunable-decode
python3 verify_final.py                   # extents + self-checks + AF ordered list
python3 symcheck.py                       # 62/62 element-vs-symbol-name check (exit 0)
python3 j700_decode.py                    # per-element decode with element/record VAs
```

Key disassembly ranges (`/var/tmp/neo/hal_text.S`):

```
fffffe00097a806c..fffffe00097a80d8   tunableh_init / device / module_finalize
fffffe00097a80d8..fffffe00097a849c   reg/offset/addr/mask/get_value/value/applied_value/is_end
fffffe00097a8484..fffffe00097a84a4   is_end + nop callback
fffffe00097a84a4..fffffe00097a876c   _tunableh_apply (the loop)
fffffe00097a876c..fffffe00097a885c   _tunableh_apply_configuration (the RMW)
fffffe00097a885c..fffffe00097a889c   legacy/ptr figs get+is_end
fffffe00097a8930..fffffe00097a897c   _tunableh_write
fffffe00097a8e00..fffffe00097a8f94   AppleT8140ANEHAL::start  (pmgr-c1ppt boot-arg)
fffffe00097a9008..fffffe00097a91e0   AppleANETunableApplyFunction        (online)
fffffe00097a91e4..fffffe00097a94b8   AppleANETunableApplyFunction_Legacy
fffffe00097a94b8..fffffe00097a9510   callPlatformFunction  (the selection branch)
fffffe00097990e0..fffffe0009799178   figs_13 applier (table 0x813baf0, then 0x813bc10)
fffffe0009799220..fffffe000979929c   figs_3 applier  (0x813bc28) + its gate
fffffe0009799344..fffffe00097993c0   figs_2 applier  (0x813bc58) + its gate
fffffe000979acec..fffffe000979ad94   c1ppt_enable gate (figs_26 vs figs_28)
```

---

## Provenance and independent verification (added after the fact)

This directory was written by **two concurrent analysis agents**: I launched
a second one after a machine shutdown without confirming the first had died.
Both wrote here. That is a process error on my part; nothing was corrupted,
but the provenance matters, so it is recorded.

Kept artifacts and what they are:

| file | origin | status |
|---|---|---|
| `README.md`, `decode_tunables.py`, `selfcheck.txt` | agent B (authoritative) | `decode_tunables.py` reproduces `selfcheck.txt` **byte-identically** |
| `j700_decode.py`, `verify_final.py`, `symcheck.py` | agent A (concurrent) | independent cross-checks; agree with B on every shared table |

Three independent confirmations of the decoded tables:

1. `symcheck.py` checks every element against its own mangled symbol name
   `_fig_<offset>_<widthbits>_<mask>_<value>`: **62 elements, 0 mismatches**.
2. My own from-scratch parse of the raw table pointers reproduces tables
   A(35)/B(2)/C(5)/D(5) and the union of 47 offsets matches the separately
   produced HAL block table exactly.
3. `decode_tunables.py` output is byte-identical to the committed
   `selfcheck.txt`.

## Correction to the applied set

The README body lists 47 offsets as the union of the block's fig lists. The
**applied** set is smaller: tables C and D are gated off on this
configuration, for **37 writes total** — 35 masked RMW in A plus 2 in B.

Raw payload bytes for B (verified by hand):

```
0x61c530: ffffffff 20000500 ...   mask 0xffffffff  value 0x00050020
0x61c540: ffffffff 30000a00 ...   mask 0xffffffff  value 0x000a0030
```

My first reading of these was wrong — I read the ASCII hex `20000500` as a
little-endian word instead of bytes `20 00 05 00`. The agent's
`0x00050020` / `0x000a0030` is correct. With `mask = 0xffffffff` the masked
RMW degenerates to a plain store, so operation is unambiguous.

Applied order, as implemented in `ane/h17/ane_h17_hal_probe.c`: tables A
and B, immediately after the CE handshake and before the PS/PWGATE work,
matching the call order in `AppleANETunableApplyFunction`.
