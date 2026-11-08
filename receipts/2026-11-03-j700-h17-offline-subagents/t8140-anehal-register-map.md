# AppleT8140ANEHAL — register, power and clock map (offline, binary only)

Target: `com.apple.driver.AppleT8140ANEHAL`, J700 / T8140 (H17), Darwin 25.6.0,
xnu-12377.161.13~4, RELEASE_ARM64_T8140.
Source artefacts: `/var/tmp/neo/kernelcache.bin`, `/var/tmp/neo/hal_text.bin`,
`/var/tmp/opencode/adt.bin` / `adt.txt`.
Nothing here is device execution. No hardware, module or device was touched.

Analysis scripts (this directory):

| script | purpose |
|---|---|
| `kcparse.py` | MH_FILESET walk, LC_FILESET_ENTRY scan |
| `kextparse.py` | LC_SEGMENT_64 / LC_SECTION_64 map of the HAL |
| `syms.py`, `syms2.py` | LC_SYMTAB dump; `syms_all.txt` = 1251 defined symbols |
| `hstrings.py` | `__cstring` / `__os_log` with VAs |
| `adis.py` | capstone disassembly of `__text` with symbol interleave |
| `verify2.py` | every resolvable memory access in `__text` + immediate sweep |
| `expr.py`, `final3.py`, `final4.py`, `figmap3.py` | fig-record / expression decoding |

Capstone 5.0.7. `objdump -D -b binary -m aarch64 --adjust-vma=0xfffffe0009799040`
(`hal_text.S`) agrees with capstone on every site quoted below.

---

## 1. Fileset entry and segment map

`LC_FILESET_ENTRY` (cmd `0x80000035`) index 164, command at file offset `0x2e70`,
name inline at `cmd+0x20` = `com.apple.driver.AppleT8140ANEHAL`.
Entry vmaddr `0xfffffe000761fcf0`, entry file offset `0x61bcf0` (Mach-O header,
filetype 11 = MH_KEXT_BUNDLE, 10 load commands, sizeofcmds `0x668`).

| segment | vmaddr | vmsize | fileoff | filesize | prot |
|---|---|---|---|---|---|
| `__TEXT` | `0xfffffe000761fcf0` | `0x1266` | `0x61bcf0` | `0x1266` | r-x |
| `__TEXT_EXEC` | `0xfffffe0009799040` | `0x105e0` | `0x2795040` | `0x105e0` | r-x |
| `__DATA` | `0xfffffe000c4448b0` | `0xfd8` | `0x54408b0` | `0xfd8` | rw- |
| `__DATA_CONST` | `0xfffffe000813b828` | `0x30b0` | `0x1137828` | `0x30b0` | rw- (init) |
| `__LINKEDIT` | `0xfffffe000c65c000` | `0x17035d4` | `0x5658000` | `0x17035d4` | r-- |

Sections:

| section | addr | size | fileoff |
|---|---|---|---|
| `__TEXT,__const` | `0xfffffe00076203c0` | `0x700` | `0x61c3c0` |
| `__TEXT,__cstring` | `0xfffffe0007620ac0` | `0x2fa` | `0x61cac0` |
| `__TEXT,__os_log` | `0xfffffe0007620dba` | `0x19c` | `0x61cdba` |
| `__TEXT_EXEC,__text` | `0xfffffe0009799040` | `0x105e0` | `0x2795040` |
| `__TEXT_EXEC,__auth_stubs` | `0xfffffe00097a9620` | `0` | `0` |
| `__DATA,__data` | `0xfffffe000c4448b0` | `0xc8` | `0x54408b0` |
| `__DATA,__common` | `0xfffffe000c444978` | `0x38` | (zerofill) |
| `__DATA,__bss` | `0xfffffe000c4449b0` | `0xed8` | (zerofill) |
| `__DATA_CONST,__auth_got` | `0xfffffe000813b828` | `0x68` | `0x1137828` |
| `__DATA_CONST,__got` | `0xfffffe000813b890` | `0x18` | `0x1137890` |
| `__DATA_CONST,__mod_init_func` | `0xfffffe000813b8a8` | `0x8` | `0x11378a8` |
| `__DATA_CONST,__mod_term_func` | `0xfffffe000813b8b0` | `0x8` | `0x11378b0` |
| `__DATA_CONST,__const` | `0xfffffe000813b8b8` | `0x2fe0` | `0x11378b8` |
| `__DATA_CONST,__kalloc_type` | `0xfffffe000813e898` | `0x40` | `0x113a898` |

`__text` is `0x105e0` = 67040 bytes, identical to `hal_text.bin`, and
`0x105e0/4 = 16760` instructions decode with no gaps.

---

## 2. Entry points

There are no exported Mach-O symbols; the kext is entered through IOKit.

| what | address |
|---|---|
| `__mod_init_func[0]` (module entry, chained-fixup target `0x27a5514`) | `0xfffffe00097a9514` `__GLOBAL__sub_I_AppleT8140ANEHAL.cpp` |
| ctor `AppleT8140ANEHAL::AppleT8140ANEHAL(OSMetaClass const*)` | `0xfffffe00097a8be8` |
| `init(OSDictionary*)` | `0xfffffe00097a8c40` |
| `probe(IOService*, SInt32*)` | `0xfffffe00097a8d38` |
| `start(IOService*)` | `0xfffffe00097a8e00` |
| `stop(IOService*)` | `0xfffffe00097a8f94` |
| `free()` | `0xfffffe00097a8cb8` |
| `callPlatformFunction(OSSymbol const*, bool, void*, void*, void*, void*)` | `0xfffffe00097a94b8` |
| `AppleANETunableApplyFunction(uint64_t)` | `0xfffffe00097a9008` |
| `AppleANETunableApplyFunction_Legacy(uint64_t,uint64_t,uint64_t)` | `0xfffffe00097a91e4` |
| `__start` / `__stop` (mod-init array bounds) | `0xfffffe00097a959c` / `0xfffffe00097a95fc` |

`__GLOBAL__sub_I_...` (`0xfffffe00097a9514`) creates two `OSSymbol`s with
`OSSymbol::withCStringNoCopy` from
`__cstring` `0xfffffe0007620b0a` = `"AppleANETunableApplyFunction"` and
`0xfffffe0007620b27` = `"AppleANETunableApplyFunctionLegacy"`, storing them in
`__bss` `0xfffffe000c445878` / `0xfffffe000c445880`.

`AppleT8140ANEHAL::callPlatformFunction` (`0xfffffe00097a94b8`) compares the
symbol argument against those two and dispatches:

* `0xfffffe00097a94f8`: `x1 = x3` then `bl 0xfffffe00097a9008`
* `0xfffffe00097a94f0`: `x1 = x3, x2 = x4, x3 = x5` then `bl 0xfffffe00097a91e4`

So the ANE register base is **passed in by the caller** as the first data
pointer. The HAL never maps device memory: `start()` (`0xfffffe00097a8e00`) only
calls the superclass `start`, registers the service, and parses a boot argument.
There is no `mapDeviceMemoryWithIndex`, `getDeviceMemoryWithIndex` or `IODeviceMemory`
call anywhere in `__text` (see §6).

`__cstring` names `/` type strings:

| VA | string |
|---|---|
| `0xfffffe0007620ac0` | `%s: addr=0x%llx mask=0x%llx value=0x%llx...` |
| `0xfffffe0007620aed` | `tunableh_print_configuration` |
| `0xfffffe0007620b0a` | `AppleANETunableApplyFunction` |
| `0xfffffe0007620b27` | `AppleANETunableApplyFunctionLegacy` |
| `0xfffffe0007620b4a` | `AppleT8140ANEHAL` |
| `0xfffffe0007620b5b` | `site.AppleT8140ANEHAL` |
| `0xfffffe0007620b71` | `1211111212221212112` |
| `0xfffffe0007620b85` | `virtual bool AppleT8140ANEHAL::init(OSDictionary *)` |
| `0xfffffe0007620bb9` | `virtual void AppleT8140ANEHAL::free()` |
| `0xfffffe0007620bdf` | `virtual IOService *AppleT8140ANEHAL::probe(IOService *, SInt32 *)` |
| `0xfffffe0007620c21` | `pmgr-c1ppt` |
| `0xfffffe0007620c2c` | `IOReturn AppleT8140ANEHAL::AppleANETunableApplyFunction(uint64_t)` |
| `0xfffffe0007620c6e` | `"AppleT8140ANEHAL: CE in error state, Error:0x%llx\n" @%s:%d` |
| `0xfffffe0007620cab` | `AppleT8140ANEHAL.cpp` |
| `0xfffffe0007620cc0` | `"AppleT8140ANEHAL: Fail to trigger CE, Error:0x%llx\n" @%s:%d` |
| `0xfffffe0007620cfe` | `IOReturn AppleT8140ANEHAL::AppleANETunableApplyFunction_Legacy(uint64_t, uint64_t, uint64_t)` |
| `0xfffffe0007620d5b` | `"ANEHWDevice::Fail Validate ANE register offset %x with value: %x\n" @%s:%d` |
| `0xfffffe0007620da7` | `AppleT8140ANEHAL.h` |

`__os_log` (`0xfffffe0007620dba`) additionally carries
`AppleT8140ANEHAL::probe`, `AppleT8140ANEHAL::start, provider: %p`,
`AppleT8140ANEHAL::start, res: %d`, ` Done AppleT8140ANEHAL`,
`Boot-arg override: ANE C1PPT enabled:%d`, `AppleT8140ANEHAL::stop`.

---

## 3. The register-program engine

The HAL does not hard-code (offset, value) stores. It interprets tables in
`__DATA_CONST,__const` (`0xfffffe000813b8b8`, `0x2fe0` bytes) with this code:

| helper | address | semantics (decoded) |
|---|---|---|
| `_tunableh_configuration_offset` | `0xfffffe00097a80e0` | `fig[0] & 0x0fffffff` = register offset |
| width code | `fig[0] >> 28` | `1`=byte `2`=half `4`=word `8`=dword (`tunableh_write`, `0xfffffe00097a8930`) |
| `_tunableh_configuration_addr` | `0xfffffe00097a80f0` | `*(u64*)ctx + offset` — `ctx+0x00` holds the MMIO base pointer |
| `_tunableh_configuration_mask` | `0xfffffe00097a8108` | mask/`value` pair via `fig[+4]` (file offset into `__TEXT,__const`) |
| `_tunableh_configuration_get_value` | `0xfffffe00097a81d0` | sized read (or the static payload when `[ctx+0x30]` gate is clear) |
| `_tunableh_configuration_apply_value` | `0xfffffe00097a83b4` | `get_value(...) \| value(...)` → sized write: **read-modify-write** |
| `_tunableh_configuration_is_end` | `0xfffffe00097a8484` | list terminator is `offset == 0x0fffffff` |
| `_tunableh_apply` | `0xfffffe00097a84a4` | walks a list, calls the apply callback per fig |
| `_tunableh_write` | `0xfffffe00097a8930` | `strb/strh/str/stur` by width code |

Record format, 12 bytes, confirmed against 341 records whose own symbol names
encode the same numbers:

```
struct fig { u32 offset_and_width;  /* width<<28 | offset */ 
             u32 payload_fileoff;   /* -> __TEXT,__const {u32 mask; u32 value;} */
             u32 flags; };          /* 0x180000 / 0x200000 / 0x280000 / 0x40000000 seen */
```

All 341 fig records are 32-bit (width code 4). `__TEXT,__const` holds 224
{mask, value} payload pairs. Every apply is a read-modify-write of one 32-bit
register; `_tunableh_configuration_apply_value` ORs the existing masked field
with the table value.

The apply entry points (`_tunableh_<block>_figs_<n>_apply_to`) take
`(ctx)` and select one fig (one "mode") from the block's list; the
`_default_apply`, `_secure_lock_lock_apply`, `_secure_lock_wr_dis_apply`,
`_dpe_ane_apply` entry points each select a fixed fig.

---

## 4. ANE register window: base and block map

The base pointer is the argument. Two `ADT` windows are large enough:
`/arm-io/ane` `reg[0]` = `0x1F0000000`, size `0x2000000` (32 MiB), and
`reg[4]` = `0x01000000`, size `0xff4000` (16 MiB). The highest offset the HAL
forms is `0x1988000` (25.5 MiB) — inside `0x2000000`, outside `0xff4000`.
**Deduction: the base is `reg[0]`, `0x1F0000000`.** Absolute addresses below
apply that base; the offsets themselves are exact and independent of it.

ADT `/arm-io/ane` (`adt.txt` line 3232) for reference:
`compatible = "ane,t8132exclave"`, `clock-ids = <0x147 0x146 0x145>`,
`clock-gates = <0x123 0x152>`, `power-gates = <0x123 0x152>`,
`exclave-reg = <0x41c00000 0x4 0x88000 0x0>`, `exclave-assigned`, 
`exclave-service = "com.apple.service.ANEExclave"`, `aot-power = 1`,
`pre-loaded = 1`, `pmp-virt-notify = 1`,
`reg = <0x1f0000000 0x2000000, 0xf0700000 0x18000, 0xf0724000 0x4000,
0xf03c0000 0x30000, 0x1000000 0xff4000, 0xf82c8000 0x4000>`.

### 4.1 Blocks (base formed by an immediate in `__text`)

| tunableh block | offset in window | absolute PA | instruction that forms the base |
|---|---|---|---|
| `s_sne_af_aneaxi2afh_af_aneaxi2af` | `0x0000000` | `0x1F0000000` | base is the argument itself |
| `s_sne_soc_dpe_temp` | `0x186e000` | `0x1F186E000` | `0xfffffe00097a55f4`, `0xfffffe00097a57fc` |
| `s_sne_sne_ctrl` | `0x1880000` | `0x1F1880000` | `0xfffffe000979d164`, `0xfffffe000979d2c4` |
| `s_sne_sne_iso_gapf` | `0x18ac000` | `0x1F18AC000` | `0xfffffe000979d5f4`, `0xfffffe000979d75c` |
| `s_sne_ane_dpe_sys` | `0x195c000` | `0x1F195C000` | `0xfffffe00097a594c`, `0xfffffe00097a6788` |
| `s_sne_anedpeppt_soc_dpe_lee` | `0x1960000` | `0x1F1960000` | `0xfffffe000979d850`, `0xfffffe00097a536c` |
| `s_sne_ane_dpe_cpms` | `0x1974000` | `0x1F1974000` | `0xfffffe00097a6b90`, `0xfffffe00097a7c9c` |
| `s_sne_anedpeppt_c1ppt_rc` | `0x197c000` | `0x1F197C000` | `0xfffffe0009799834`, `0xfffffe000979b0e8` |
| `s_sne_mcw` | `0x1984000` | `0x1F1984000` | `0xfffffe000979bf58`, `0xfffffe000979ced8` |
| `s_sne_sne_ctrl_lock` (`sne_intc_lock`) | `0x1988000` | `0x1F1988000` | `0xfffffe000979d3a8`, `0xfffffe000979d510` |

`sne_intc_lock` is a member of the `sne_sne_ctrl_lock` tunable object —
`__bss` symbols `_tunableh_die_0_soc_subsystem2_s_sne_sne_ctrl_lock.sne_intc_lock`
(`0xfffffe000c445458`) and `...sne_intc_lock_initialized` (`0xfffffe000c445450`),
so it shares base `0x1988000`.

### 4.2 Direct 64-bit accesses, exact instruction addresses

`AppleANETunableApplyFunction` (`0xfffffe00097a9008`), `x19` = base:

| ia | access | offset | absolute | note |
|---|---|---|---|---|
| `0xfffffe00097a906c` | `ldr x9,[x8]` | `0x18a4010` | `0x1F18A4010` | READ |
| `0xfffffe00097a907c` | `ldr x8,[x8]` | `0x18a4018` | `0x1F18A4018` | READ |
| `0xfffffe00097a90a4` | `str x21,[x8]` | `0x18a4000` | `0x1F18A4000` | WRITE, value `((v@0x18a4010 & 3) ^ 3)` |
| `0xfffffe00097a90bc` | `ldr x8,[x9]` | `0x18a4008` | `0x1F18A4008` | READ, polled ≤ `0x1389` (5001) times |
| `0xfffffe00097a90d0` | `bl 0xfffffe000bba00c0` (`w0=0xa`) | — | — | settle in the poll loop (inference: `IODelay`, which the kext imports) |
| `0xfffffe00097a9104` | `ldr x8,[x9,#0x10]` | `0x18a4018` | `0x1F18A4018` | READ after the ack |
| `0xfffffe00097a9110` | `str x21,[x9]` | `0x18a4008` | `0x1F18A4008` | WRITE, same value |

Failure paths: `0xfffffe00097a91a4` logs `"...CE in error state, Error:0x%llx"`
(`0xfffffe0007620c6e`) when `(v@0x18a4010 & 3)^3` intersects `v@0x18a4018`;
`0xfffffe00097a90dc`/`0xfffffe00097a91c4` log `"...Fail to trigger CE..."`
(`0xfffffe0007620cc0`) on poll timeout / ack mismatch and then call
`0xfffffe000bd10f08` (inference: `panic`, given the `"%s" @%s:%d` companion
arguments `AppleT8140ANEHAL.h` and line numbers `0x61` / `0x6a` / `0x74`). This
is the "CE" (compute-engine) doorbell/ack pair.

`AppleANETunableApplyFunction_Legacy` (`0xfffffe00097a91e4`), `x19` = base:

| ia | access | offset | absolute | value |
|---|---|---|---|---|
| `0xfffffe00097a92dc` | `str x22,[x8]` | `0x1960000` | `0x1F1960000` | `1` |
| `0xfffffe00097a9338` | `str xzr,[x8]` | `0x1974200` | `0x1F1974200` | `0` |
| `0xfffffe00097a933c` | `str x22,[x8]` | `0x1974200` | `0x1F1974200` | `1` |
| `0xfffffe00097a9398` | `str x22,[x8]` | `0x195c284` | `0x1F195C284` | `1` |
| `0xfffffe00097a939c` | `stur x22,[x8,#4]` | `0x195c288` | `0x1F195C288` | `1` |

(`x22 = 1` from `mov w22,#1` at `0xfffffe00097a92d8`.) Note `0x195c284` and
`0x195c288` overlap as 8-byte stores; treat both as writes of 1 to two adjacent
32-bit cells.

Cross-check against this repository's own prior art (T6021/T8152 modules, not
T8140): `ane/h18` documents the doorbell at *engine + `0x18a4000`* and an IPI
acknowledge at *engine + `0x18b0000`*, and `ane/h15` uses pending
*engine + `0x184c000`* / ack *engine + `0x1850000`*. The `0x18a4000` figure
matches the CE doorbell here exactly. The name `s_sne_sne_iso_gapf` for
`0x18ac000` in this HAL does **not** match the "IPI pending" role the M6 note
gives `0x18ac000`; the two are different SoC generations and the naming here is
the binary's own, so do not carry the M6 role across. `0x184c000`, `0x1850000`
and `0x18b0000` are not referenced anywhere in this HAL.

`AppleANETunableApplyFunction` then applies the `af_aneaxi2af` figs at
`base+0x0` (`0xfffffe000979942c`, `0xfffffe0009799574`, `0xfffffe00097994d0`
called at `0xfffffe00097a9128`/`0x130`/`0x138`) and, only if the `pmgr-c1ppt`
boot argument enabled it, the `c1ppt_rc` figs at `base+0x197c000`
(`0xfffffe00097a9148`, calls at `0xfffffe00097a916c/174/17c/184`).

`_Legacy` applies, in order: `af_aneaxi2af` @ +0 (`0xfffffe00097a9268/270/278`),
`soc_dpe_temp` @ +`0x186e000` (`0xfffffe00097a92a4`), `soc_dpe_lee` @
+`0x1960000` (`0xfffffe00097a92e4/2ec`), `dpe_cpms` @ +`0x1974000`
(`0xfffffe00097a931c/324`, direct writes at `0xfffffe00097a9338/33c`,
`0xfffffe00097a9344/34c`), `dpe_sys` @ +`0x195c000`
(`0xfffffe00097a937c/384`, direct writes at `0xfffffe00097a9398/39c`,
`0xfffffe00097a93a4/3ac`), `c1ppt_rc` @ +`0x197c000` when enabled
(`0xfffffe00097a93e0/3e8/3f0/3f8`), `sne_iso_gapf` @ +`0x18ac000`
(`0xfffffe00097a9424`), `mcw` @ +`0x1984000` (`0xfffffe00097a9448`),
`sne_ctrl` @ +`0x1880000` (`0xfffffe00097a9470`), `sne_ctrl_lock` @
+`0x1988000` (`0xfffffe00097a9494`).

### 4.3 Register offsets per block (32-bit read-modify-write)

Offset sets are the union of the block's fig lists; the last column is the fig
entry point that selects the quoted (mask, value) step.

| block (base) | offsets touched | selected steps (offset: mask -> value) |
|---|---|---|
| `af_aneaxi2af` (`0x0`) | `0x0,0x4,0x8,0xc,0x10,0x14,0x18,0x1c,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x100,0x108,0x304,0x314,0x31c,0x324,0x338,0x33c,0x408,0x700,0x748,0x800,0x848,0x900,0x948,0xb00,0xb04,0xdf8,0xdfc,0xef8,0xefc,0xff8,0xffc,0x10f8,0x10fc,0x11f8,0x11fc,0x4008,0x400c,0x4014,0x4018,0x401c` (47 records from `0x11378b8`) | `0x0:0x800000ff->0x8000001b` (`0xfffffe0009799624`); `0x338:0xffffffff->0x50020` (`0xfffffe0009799648`); `0xdf8:0x1->0x1` (`0xfffffe0009799690`); `0xdfc:0x1->0x1` (`0xfffffe000979966c`) |
| `soc_dpe_temp` (`0x186e000`) | `0x0,0x14` | `0x0:0x3->0x1` (`0xfffffe00097a5864`); `0x14:0xffffffff->0x30000` (`0xfffffe00097a5888`), `->0x8000` (`0xfffffe00097a58ac`), `->0x1000` (`0xfffffe00097a58d0`); figs_14 at `0xfffffe00097a5674`, `0xfffffe00097a56a4`, `0xfffffe00097a56fc`, `0xfffffe00097a5728` selects all four |
| `sne_ctrl` (`0x1880000`) | `0x1c0f4` | `0x1c0f4:0x3f->0x0` (`0xfffffe000979d344`, `0xfffffe000979d304`) |
| `sne_iso_gapf` (`0x18ac000`) | `0x4c` | `0x4c:0x1->0x0` (`0xfffffe000979d7dc`, `0xfffffe000979d79c`) |
| `dpe_sys` (`0x195c000`) | `0xc,0x10,0x40,0x28c,0x2fc` | `0xc:0xffffffff->0x23e1` (`0xfffffe00097a6738`); `0x10:0x1ff->0x19` (`0xfffffe00097a6718`); `0x40:0xfff->expr soc_expression_504` (`0xfffffe00097a686c`); `0x28c:0x7ff->0x7ff` (`0xfffffe00097a6a44`); `0x2fc:0x1->0x1` (`0xfffffe00097a6958`) |
| `soc_dpe_lee` (`0x1960000`) | `0x0,0x4,0x8,0x34,0x38,0x44,0x48,0x54,0x58,0x64,0x68,0x74,0x78,0x84,0x88,0x94,0x98,0xa4,0xa8,0xb4,0xb8,0xc4,0xc8,0xd4,0xd8,0xe4,0xe8,0xf4,0xf8,0x104,0x108,0x114,0x118,0x124,0x128` | `0x0:0x1e->0xc` (`0xfffffe00097a5500`); `0x4:0xff->0x4` (`0xfffffe00097a5494`); `0x8:0xff->0xa` (`0xfffffe00097a5524`); `0x34:0x3fffff->expr soc_dpe_lee_expression_1775` (`0xfffffe00097a5590`) |
| `dpe_cpms` (`0x1974000`) | `0x0,0x140,0x204,0x27c` | `0x0:0xf->0x0` (`0xfffffe00097a7ce4`); `0x140:0xffffffff->dynamic sys_expression_552` (`0xfffffe00097a7dd0`); `0x204:0x7->0x7` (`0xfffffe00097a7fa8`); `0x27c:0x1->0x1` (`0xfffffe00097a7ebc`) |
| `c1ppt_rc` (`0x197c000`) | `0x4,0x20,0x24,0x28,0x2c,0x40,0x44,0x48,0x60,0x700,0x3ff0,0x3ffc` | `0x4:0x3->0x2/0x3/0x1/0x0` (`0xfffffe000979ba64`, `0xfffffe000979baf4`, `0xfffffe000979bb3c`, `0xfffffe0009799904`); `0x20:0xffff->0x5dc` / `->0x1068` (`0xfffffe000979bb84`); `0x24:0xffff->0x544`/`0xc4c`; `0x28:0xffffffff->0x3200258`/`0x7d00708`; `0x2c:0xff->0x1`/`0x3`/`0x2`; `0x40:0x3ff->0xbf`; `0x44:0x3ff->0x200`/`0x19a`; `0x48:0x1ffff->0x7800`; `0x60:0x1f1f->0x103`; `0x700:0x7fff->0x2712`; `0x3ff0:0x7fffff->0x77ebf9`; `0x3ffc:0x1->0x1` |
| `mcw` (`0x1984000`) | `0x0` | `0x0:0x1f00->0x0` (`0xfffffe000979d108`, `0xfffffe000979cf18`) |
| `sne_ctrl_lock` (`0x1988000`) | `0x0` | `0x0:0x111->0x111` (`0xfffffe000979d590`, `0xfffffe000979d550`) |

The `c1ppt_rc` list is shared verbatim with `south_bridge_pms_soc_rc` /
`south_bridge_pms_soc_ro_rc` / `agx_c1ppt_rc_mgpu`, and the `mcw` list with
`sisp_isp_mcw` / `south_bridge_pms_mcw_0` / `display_ext_dispexttop_mcw` /
`display_pipe_disptop_mcw` / `agx_gfndr_system_agx_mcw` — a vendor-generated
generic PMGR tunable library, of which only the `sne_*` (ANE) instances are
wired to an ANE base in this binary.

### 4.4 Read-only expression registers

`_*_expression_*_get_value` functions feed dynamic values into figs. They read
the ANE window through `[ctx+0x48]` (the base) and take an 8-bit field from
bits 63:56 of a 64-bit register:

| ia (example) | access | offset | absolute |
|---|---|---|---|
| `0xfffffe000979d88c` | `ldr x8,[base+0xe78040]` | `0xe78040` | `0x1F0E78040` |
| `0xfffffe00097a6bcc` | `ldr x8,[base+0xe78040]` | `0xe78040` | `0x1F0E78040` |
| `0xfffffe00097a5988` | `ldr x8,[base+0xe78040]` | `0xe78040` | `0x1F0E78040` |

96 read sites, 16 distinct addresses, stride `0x40`:
`0xe78040, 0xe78080, 0xe780c0, 0xe78100, 0xe78140, 0xe78180, 0xe781c0,
0xe78200, 0xe78240, 0xe78280, 0xe782c0, 0xe78300, 0xe78340, 0xe78380,
0xe783c0, 0xe78400` (absolute `0x1F0E78040` .. `0x1F0E78400`).
All 64-bit reads, all read-only. Each reads `(v >> 56) & 0xff`, then a
multiply/divide chain (`0x4189375`, `0x10624dd3`, `0x28f5c29`, `0x4f463080` in
`_sys_expression_552_get_value` at `0xfffffe00097a6bbc`) — a scaled conversion of
a per-slice raw counter.

---

## 5. Power / clock / gating evidence

**What is modelled.** Three things, and only these:

1. **`sne_mcw` — "master clock wrapper"** at `0x1984000`, one 32-bit
   read-modify-write of offset `0x0` with `mask 0x00001f00, value 0`
   (`0xfffffe000979d108`, applied unconditionally from
   `_Legacy` at `0xfffffe00097a9448`). Inference: bits 8..12 of the MCW control
   word are a divider / clock-select field, cleared to 0. The MCW block is a
   generic SoC block, present for SVE, ISP, display and AGX with the same
   register; only the ANE instance (`0x1984000`) is programmed here.

2. **`c1ppt_rc` — "C1 PPT RC"** at `0x197c000`, 12 registers (§4.3), applied
   **only when the `pmgr-c1ppt` boot argument is present and non-zero**.
   `start()` `0xfffffe00097a8f30`: `x0 = "pmgr-c1ppt"` (`0xfffffe0007620c21`),
   `x1 = &val`, `w2 = 4`, `bl 0xfffffe000bccad94` (`PE_parse_boot_argn`); on
   success `strb (val != 0), [this+0x90]` at `0xfffffe00097a8f54` and the log
   `Boot-arg override: ANE C1PPT enabled:%d`. `[this+0x90]` gates the block at
   `0xfffffe00097a913c` (non-legacy) and `0xfffffe00097a93b0` (`_Legacy`).
   Default is **off** (`strb wzr,[x19,#0x90]` at `0xfffffe00097a8f2c`).
   Inference: PPT = power/performance throttling; "RC" = register/config block.
   `0x197c000` offsets `0x4` (2-bit field) and `0x20`/`0x24` (16-bit) look like
   throttling setpoints, and `0x3ff0`/`0x3ffc` are a magic unlock/lock pair.

3. **`sne_sne_ctrl_lock`** at `0x1988000`, offset `0x0`, mask/value `0x111`
   (write-once "lock the block" pattern), plus the aliases
   `sne_intc_lock` (`0xfffffe000979d590`). And **`af_aneaxi2af`** secure-lock
   figs at `0x100`/`0x108` and `0x304`/`0x314`/`0x31c`/`0x324`
   (`0xfffffe00097995b0`, `0xfffffe00097994d0`).

**What is NOT modelled.** No PMGR clock-enable, clock-gate, power-domain or
P-state register is touched. In particular the HAL never forms or accesses
`0xc000`, `0xc008`, `0xc010`, `0x268c` or `0x26b0`, and never references the
ADT clock IDs `325/326/327` (`0x145/0x146/0x147`) or gate IDs `291/338`
(`0x123/0x152`) as numbers. On T8140 the ANE node is
`compatible = "ane,t8132exclave"` with `exclave-assigned` and
`exclave-service = "com.apple.service.ANEExclave"`, so **inference**: clock and
power-gate voting for the ANE is done by the ANE platform device / PMGR driver
and/or the Exclave, not by this HAL.

---

## 6. Explicit "not found"

Searched the whole `__text` (16760 instructions, every immediate formed by
`mov/movz/movn/movk` chains, and every `ldr/str/ldur/stur/ldrb/strb/ldrh/strh/ldrsw`
displacement with a resolvable base):

| searched for | offset | result |
|---|---|---|
| scratch cells SCRATCH0..7 | `0x1880020`,`0x1880024`,`0x1880028`,`0x188002c`,`0x1880030`,`0x1880034`,`0x1880038`,`0x188003c` | **not found** (the only `sne_ctrl`/`0x1880000` access is offset `0x1c0f4`) |
| CPU control / status | `0x1600044`, `0x1600048` | **not found** |
| RVBAR | `0x1050000` | **not found** |
| mailbox CTRL/status | `0x1608110`, `0x1608114`, `0x1608800`, `0x1608830` | **not found** |
| mailbox base | `0x1608000` | **not found** |
| PMGR child/state | `0xc000`, `0xc008`, `0xc010` | **not found** as a register. `0xc000` does appear as an immediate 36 times, and in all 36 it is the low half of `mov wN,#0xc000; movk wN,#0x1xx,lsl#16` forming a block base (`0x197c000`, `0x1984000`, `0x195c000`, `0x186e000`, `0x18ac000`, `0x163bc000`, `0x123bc000`, `0x3f0000`, `0x470000`, `0x22d44000`). Zero unpaired occurrences. |
| PMGR | `0x268c`, `0x26b0` | **not found** |
| clock IDs | `325`, `326`, `327` (`0x145`, `0x146`, `0x147`) | **not found** |
| gate IDs | `291`, `338` (`0x123`, `0x152`) | **not found** |
| any clock-enable / clock-gate register write | — | **not found**; the only clock-shaped register is `mcw+0x0` (§5.1) |
| MMIO mapping (`mapDeviceMemoryWithIndex`, `getDeviceMemoryWithIndex`, `IODeviceMemory`) | — | **not found**; the base pointer is a caller argument |
| `ANERegisterControl` symbol/string | — | **not found**; the equivalent is the `tunableh_*` engine in §3 |
| `AppleH16ANEInterface` symbols inside the HAL | — | **not found**; the HAL has no back-reference to that kext |

Contrast (not part of the HAL, quoted only so the absence is meaningful): the
sibling `com.apple.driver.AppleH16ANEInterface` in the same fileset does contain
`mov w1, #0x1050000` — at `0xfffffe0008f2ebb4`, `0xfffffe0008f3e008`,
`0xfffffe0008f3e13c` in its `__text` — i.e. the RVBAR access lives in the
interface kext, not in `AppleT8140ANEHAL`.

---

## 7. Caveats

* Byte-level facts only. No result here has run on an ANE.
* The base-pointer identity (`0x1F0000000`, `reg[0]`) is a size deduction, not a
  measurement. All offsets are exact regardless.
* The meanings of the `sne_*` block names come from the binary's own symbol
  names (`_tunableh_die_0_soc_subsystem2_s_sne_*`), not from documentation.
  `sne` = the ANE subsystem in Apple's PMGR naming; `mcw`, `c1ppt`, `dpe`,
  `cpms`, `iso_gapf`, `aneaxi2af` are expanded only where labelled inference.
* The HAL's symbol table also contains fully-formed tunable blocks for
  `soc_subsystem1_0_south_bridge_pms_*`, `sisp_isp_mcw`,
  `soc_subsystem2_display_*_mcw` and `soc_subsystem3_graphics_agx_*`, with bases
  `0x3f0000`, `0x3f4000`, `0x470000`, `0x22d44000`, `0x163bc000`, `0x123bc000`.
  No ANE path reaches them; they are dead generic-library instances in this
  binary and are **not** part of the ANE register map.
