# J700 / T8140 (H17) — the ordered MMIO sequence up to the first `CPU_CONTROL` write

Offline Mach-O analysis of the retained kernelcache only. **No device access, no
module load, no MMIO, no reboot.** Nothing below is device qualification; it is
compiled-source observation plus static table decode.

## Evidence identities

| Item | Value |
| --- | --- |
| Kernelcache | `/var/tmp/neo/kernelcache.bin`, MH_FILESET (magic `0xfeedfacf`, filetype 12, 366 entries) |
| ANE driver | `com.apple.driver.AppleH16ANEInterface`, entry VA `0xfffffe00073af850`, file off `0x3ab850` |
| Driver `__TEXT_EXEC.__text` | VA `0xfffffe0008eeb820` … `0xfffffe0008ff8dd8` (size `0x10d5b8`), file off `0x01ee7820` |
| Driver symbols | LC_SYMTAB symoff 91908160, nsyms 9142 (real `__ZN…` names, not stripped) |
| HAL kext | `com.apple.driver.AppleT8140ANEHAL`, entry VA `0xfffffe000761fcf0`, file off `0x61bcf0` |
| HAL `__text` | VA `0xfffffe0009799040` … `0xfffffe00097a9620` (size `0x105e0`), file off `0x02795040` |
| HAL symbols | LC_SYMTAB symoff 93679744, nsyms 1548 |

Top-level segment map used for VA↔file: `__TEXT` +0x0000, `__PRELINK_TEXT` off `0x8000`,
`__DATA_CONST` off `0xc6c000`, `__TEXT_EXEC` off `0x1830000`, `__DATA` off `0x5168000`,
`__LINKEDIT` off `0x55bc000`.

## 0. Two corrections to the prior round that everything below depends on

**(a) "ane-type `0x200`" is the ADT value; the driver's internal family is `0xe0`.**
`initializeANESoCConfig` (`0xfffffe0008f64ae0`) reads ADT `ane-type` into
`this+0x1cc` (`0x8f64c38`, `0x8f64cc4`) and then, at `0x8f64f90`–`0x8f64f9c`:

```
fffffe0008f64f90  cmp  w8, #0x200
fffffe0008f64f94  b.ne 0xfffffe0008f64a38        ; -> default/unsupported
fffffe0008f64f98  mov  w8, #0xe0
fffffe0008f64f9c  str  w8, [x21]                 ; this+0xe270 := 0xe0   <- internal family
```

`this+0xe270` is what every later power/register switch tests. So prior receipts'
"ane-type 0x200 block at `0x8f294b0`" is the `EnableCPUClocksAndPower` jump-table
**entry 11 (family `0xe0`)** at `0xfffffe0008f29498`, and `0x8f294b0` is its first
store. Verified: the jump table at `0xfffffe0008f29bc0` decodes to 15 cases over
`(v-0x30)>>4 <= 0xe`; index 11 → `0x8f29498`.

**(b) The first engine write is not `CPU_CONTROL`.** It is a SCRATCH clear, and
before that the driver makes a *provider* clock/power vote and hands the ANE window
to a *second kext* that writes ~45 registers. Details in §3.

## 1. Object layout this report relies on

`ANEHWDevice` `this`-relative fields (all resolved by constant propagation, not by
guessing):

| Field | Meaning | Set at |
| --- | --- | --- |
| `0xe270` | internal hw family (J700 = `0xe0`) | `0x8f64f9c` |
| `0xe278` | log-name index 0..3 | `0x8f64b14`, `0x8f65498`, … |
| `0xe280` | family name string `"h17"` (`0xfffffe00073b5038`) | `0x8f64fb0` |
| `0xe320` | `ANERegisterControl*` **ANE** window | `0x8f53b20` |
| `0xe328` | `ANERegisterControl*` **PS** window | `0x8f53b58` |
| `0xe330` | `ANERegisterControl*` **PWGATE** window | `0x8f53b88` |
| `0xe338` | `ANERegisterControl*` **PTD** | `0x8f53bb8` (not created here) |
| `0xe348` | `ANERegisterControl*` **ANEHAL1** | `0x8f53bec` |
| `0xe350` | `ANERegisterControl*` **ANEHAL2** | `0x8f53c20` |
| `0xe358` | `ANERegisterControl*` **ANEHAL3** | `0x8f53c54` |
| `0x388` | **ANE window mapped VA** (`get_register_base()`) | `0x8f53b28` |
| `0x390` | bit0 = PS window exists | `0x8f53b2c` reader |
| `0x398` | ANEHAL1 window VA | `0x8f53bf4` |
| `0x3a8` | ANEHAL2 window VA | `0x8f53c28` |
| `0x3b8` | ANEHAL3 window VA | `0x8f53c5c` |
| `0x2b0` | provider `AppleARMIODevice*` | `0x8f52b38` |
| `0x3f8` | `IOService*` name-matched **`"AppleT8140ANEHAL"`** | `0x8f516ac` |
| `0x200` | bit0 = **RTBuddy client present** (0 on a direct-path boot) | `0x8f52b84` |
| `0x3a20` | J700 = `3` | `0x8f64294` |
| `0x3760` | clock/power refcount | `0x8f6426c` (zero) |
| `0x3765`,`0x3766` | "CPU clocks on" / "ANE clocks on" | `0x8f29a2c`, `0x8f2a814` |
| `0x1cc` | raw ADT `ane-type` (`0x200`) | `0x8f64c38`/`0x8f64cc4` |

Job700 (family `0xe0`) register-offset table, decoded from the constant pool at
`0xfffffe00073b02d0`, `0xfffffe00073b02f0`, `0xfffffe00073b03b0`,
`0xfffffe00073b0480`, `0xfffffe00073b04a8`, `0xfffffe00073b0260` and stored by
`initializeANESoCConfig` at `0x8f6507c`–`0x8f650a4`:

| Field | ANE-window offset | Function that uses it |
| --- | --- | --- |
| `0xfc`  | `0x1880020` | SCRATCH0 (clear) |
| `0x100` | `0x1880024` | SCRATCH1 (clear) |
| `0x104` | `0x1880028` | SCRATCH2 (clear) |
| `0x108` | `0x188002c` | SCRATCH3 (clear) |
| `0x10c` | `0x1880030` | SCRATCH4 (clear) |
| `0x110` | `0x1880034` | SCRATCH5 (clear, and `this[0xa4]` write) |
| `0x114` | `0x1880038` | SCRATCH6 |
| `0x118` | `0x188003c` | SCRATCH7 |
| `0x11c` | `0x1880060` | `setBit32(.,0)` late in `ANE_Init` (`0x8f3e5f4`) |
| `0x128` | `0x1880058`,`0x188005c` | — |
| `0x148` | `0x1370000`,`0x1370004` | MCW-ish pair |
| `0x150` | `0x188c000` | interrupt status (`0x8f5810c`) |
| `0x154` | `0x1890000` | interrupt ack (`0x8f581cc`) |
| `0x158` | `0x1884000`,`0x1600048` | doorbell / **CPU_STATUS** (matches the live read) |
| `0x160` | **`0x1600044`** | **CPU_CONTROL** |
| `0x164` | *never set on family `0xe0`* | therefore `0` (see §6, unresolved) |
| `0xf8`  | `0xc010` (PS window) | MPM power domain |
| `0x124` | `0x1880064` | — |

`0x1050000` (RVBAR) is written as a literal, not through the table.

`ANERegisterControl` (`0xfffffe0008f73cd4` ctor) is `{ AppleARMIODevice* provider
@+0, void* mapped_base @+8, … }`. Every accessor indexes `[obj+8]`:

```
fffffe0008f74134  ldr x8,[x0,#8]        ; write32(off,val)
fffffe0008f74138  str w2,[x8,w1,uxtw]
fffffe0008f740f0  ldr x8,[x0,#8]        ; write64(off,val)
fffffe0008f740f4  str x2,[x8,w1,uxtw]
fffffe0008f74144  ldr x8,[x0,#8]        ; setBit32(off,bit)  = read-modify-write
fffffe0008f74148  ldr w9,[x8,w1,uxtw]
fffffe0008f74158  str w9,[x8,w1,uxtw]
```

So every driver-issued MMIO write is `str w2,[base+off]` — **no read-modify-write**
except `setBit32`/`clearBit32`, and **none of those occur before `CPU_CONTROL`**.

## 2. Two provider-method call sites (not MMIO) — this is the gate

`ANEHWDevice::EnableANEClocksAndPower(bool enableCPU, bool enablePerf)`
`0xfffffe0008f29ec4`. On the first call (`this+0x3760 == 0`, `0x8f29f18`) it takes
the branch at `0x8f29fb8`:

| # | Instruction | Call |
| --- | --- | --- |
| 1 | `0x8f2a060` | `blraa` through `[ [this+0x2b0] + 0x8a8 ]` with `x0=[this+0x2b0]`, `w1=#1`, `w2=[this+0x380]` |
| 2 | `0x8f2a0a8` | `blraa` through `[ [this+0x2b0] + 0x8b0 ]` with `x0=[this+0x2b0]`, `w1=#1`, `x2=&sp+0x1c`, `w3=[this+0x380]` |

`[this+0x2b0]` is the `AppleARMIODevice` provider (`0x8f52b38`). Slot `0x8a8` is
the receipt-identified `AppleARMIODevice::enableDeviceClock`, slot `0x8b0`
`enableDevicePower`; corroborated here by argument shape (bool, uint32 index) and
(bool, uint32* out, uint32 index). These are the only two steps in the whole
bring-up that put the ANE block's clock and power gates into the requested state,
and **they are IOKit virtual calls, not MMIO**. There is no imported symbol for
them — the driver's LC_SYMTAB contains only `__ZN16AppleARMIODevice9metaClassE`
of that class, i.e. pure vtable dispatch.

`AppleARMIODevice` translates index `[this+0x380]` through tables it parsed from the
DT properties `clock-ids`, `clock-gates`, `power-gates`, `service-gates`,
`service-reset`, `service-name`, `pin-groups`
(`AppleARMIODevice::initDeviceWithProvider` at `0xfffffe000896a8c0`).

## 3. The ordered sequence

Everything below is in program order for a **direct-path J700 boot**
(ADT `ane-type = 0x200`, no RTBuddy client so `this[0x200] bit0 = 0`).
`W32` = `ANERegisterControl::write32`, `W64` = `write64`, `RMW` = read-modify-write.

### Phase A — `ANEHWDevice::start` (`0xfffffe0008f52dd0`), no MMIO

| Step | Function | Address | Note |
| --- | --- | --- | --- |
| A1 | `InitializeProvider` | `0x8f52eac` | creates ANE/PS/PWGATE(/PTD)/HAL1/HAL2/HAL3 controls; **no writes** |
| A2 | `initializeANEProperties` | `0x8f52eb8` | `this+0x3760 := 0`, `this+0x3a20 := 3` |
| A3 | `initializeANESoCConfig` | `0x8f52ec0` | sets family `0xe0`, SCRATCH/CPU offsets, `this+0x36f8`/`0x3700` table |
| A4 | `get_register_base` ×4 | `0x8f53b24`,`0x8f53bf0`,`0x8f53c24`,`0x8f53c58` | captures window VAs into `0x388`,`0x398`,`0x3a8`,`0x3b8` |
| A5 | `EnableANEClocksAndPower(this,1,1)` | `0x8f53fac` | → Phase B |

### Phase B — `EnableANEClocksAndPower(this,1,1)`, first-enable path

| Step | Address | Window | Offset | Value | Kind |
| --- | --- | --- | --- | --- | --- |
| B1 | `0x8f2a060` | **provider** | — | `enableDeviceClock(1, this[0x380])`, slot `0x8a8` | virtual call |
| B2 | `0x8f2a0a8` | **provider** | — | `enableDevicePower(1, &out, this[0x380])`, slot `0x8b0` | virtual call |
| B3 | `0x8f2a610` | **HAL kext** | — | `callPlatformFunction("AppleANETunableApplyFunction", 0, this[0x388], this[0x398], this[0x3a8], …)` on `this[0x3f8]` = `AppleT8140ANEHAL` | virtual call → Phase C |
| B4 | `0x8f2a68c` | ANE | `this[0x134]` | **skipped** — `this[0x134] = 0` on family `0xe0` (`0x8f64e10`) | — |
| B5 | `0x8f2a6a0` | — | — | `InitANEScratchRegisters` **skipped** — gated on `this[0x200] bit0` (RTBuddy) | — |
| B6 | `0x8f2a6b0` | — | — | `EnableCPUClocksAndPower()` → Phase D | — |
| B7 | `0x8f2a760` | — | — | `EnableSubblocksClocksAndPower()` → Phase E | — |
| B8 | `0x8f2a80c`,`0x8f2a814` | RAM | — | `this+0x3760 := 1`, `this+0x3765 := 1`, `this+0x3766 := 1` | — |
| B9 | `0x8f2a820` | — | — | `DisableANEClocksAndPower(this, 0)` → refcount 1→0; **no MMIO write** (only reads PS `this[0x3a10]`/`this[0x3a14]` at `0x8f2c910`/`0x8f2c998` and a re-vote of slot `0x8a8` at `0x8f2c7b4`) | — |

### Phase C — HAL tunable program

`AppleT8140ANEHAL::callPlatformFunction` `0xfffffe00097a94b8`:

```
fffffe00097a94c4  ldr x8,[0xc445878]              ; OSSymbol "AppleANETunableApplyFunction"
fffffe00097a94cc  cmp x1,x8 ; b.eq 0x97a94f8
fffffe00097a94d4  ldr x8,[0xc445880]              ; OSSymbol "AppleANETunableApplyFunctionLegacy"
fffffe00097a94dc  cmp x1,x8 ; b.ne 0x97a9500
```

`[0xc445878]` is built in `__GLOBAL__sub_I_AppleT8140ANEHAL.cpp` `0x97a9520`–`0x97a9530`
from the string at `0xfffffe0007620b0a` = `"AppleANETunableApplyFunction"`. The driver
passes exactly that symbol (`__GLOBAL__sub_I_ANEClocksPower.cpp` `0x8f2d5f4`–`0x8f2d604`
creates it from `0xfffffe00073b0c80`, stored at driver `__bss 0xfffffe000c3f9428`;
read at `0x8f2a468`), so the **online** variant runs, not
`AppleANETunableApplyFunctionLegacy`.

`AppleANETunableApplyFunction(aneBase)` `0xfffffe00097a9008`, `aneBase = [this+0x388]`.
All offsets are ANE-window-relative; **all four handshake accesses are `ldr`/`str`
through `aneBase`**, i.e. real MMIO:

| Step | Address | Window | Offset | Value | Kind |
| --- | --- | --- | --- | --- | --- |
| C1 | `0x97a906c` | ANE | `0x18a4010` | read, `v = rd32 & 3` | read |
| C2 | `0x97a907c` | ANE | `0x18a4018` | read, `s = rd32` | read |
| C3 | `0x97a9088` | — | — | if `((v ^ 3) & s) != 0` → **panic** (line `0x6a`, `0x97a91c0`) | — |
| C4 | `0x97a9090` | — | — | if `v == 3` skip to C9 (already unlocked) | — |
| C5 | `0x97a90a4` | ANE | **`0x18a4000`** | `v ^ 3` | **W32** |
| C6 | `0x97a90bc` | ANE | `0x18a4008` | poll `rd32 & 3 == (v ^ 3)`, ≤ `0x1389` (5001) polls × `_IODelay(10)` | read loop |
| C7 | `0x97a9104` | ANE | `0x18a4018` | read64; if `& (v^3)` → **panic** (line `0x74`, `0x97a91e0`) | read |
| C8 | `0x97a9110` | ANE | **`0x18a4008`** | `v ^ 3` | **W64** |
| C9 | `0x97a9128` | ANE | see table below | 35 RMWs | **RMW** |
| C10 | `0x97a9130` | ANE | see table | 5 RMWs | **RMW** |
| C11 | `0x97a9138` | ANE | see table | 5 RMWs | **RMW** |
| C12 | `0x97a913c` | — | — | if `HAL[0x90] bit0`: `0x97a916c`–`0x97a9184`, `0x1137c88`…`0x1137d90` | RMW |

Record format: 8 bytes `{u32 offset, u32 value}` at the addresses below, terminated
by an all-zero record; the applier ORs `value` into `base+offset`
(`_tunableh_apply_configuration` `0xfffffe00097a876c` → `_tunableh_write`
`0xfffffe00097a8930` → `str[b/h/w/x] w4,[x2]`).

**C9** — `sne_af_aneaxi2af` default, cfg `0xfffffe000813baf0`, wrapper
`0x979942c` → real applier `0x97990e0`:

`0x11378b8 + 0xc·k` for k = 0…33, each ← `0x00100000` (34 registers,
`0x11378b8 … 0x1137a44`), then `0x1137a50` ← `0x00200000`.

**C10** — `sne_af_aneaxi2af` `secure_lock_wr_dis`, cfg `0xfffffe000813bc58`
(`0x9799574` → `0x9799344`):

| Offset | OR-in |
| --- | --- |
| `0x1137a5c` | `0x00100000` |
| `0x1137a74` | `0x00100000` |
| `0x1137a8c` | `0x00100000` |
| `0x1137aa4` | `0x00100000` |
| `0x1137abc` | `0x00280000` |

**C11** — `sne_af_aneaxi2af` `secure_lock_lock`, cfg `0xfffffe000813bc28`
(`0x97994d0` → `0x9799220`):

| Offset | OR-in |
| --- | --- |
| `0x1137a68` | `0x00100000` |
| `0x1137a80` | `0x00100000` |
| `0x1137a98` | `0x00100000` |
| `0x1137ab0` | `0x00100000` |
| `0x1137ac8` | `0x00200000` |

**C12** — `c1ppt_rc` group, base `aneBase + 0x197c000` (`0x97a9148`), chain
`0x979b144` (default) → `0x979b098` (`dpe_ane`) → `0x979b2fc` (`secure_lock_lock`)
→ `0x979b220` (`secure_lock_wr_dis`); cfg tables `0x813bda0`, `0x813bdb0`,
`0x813bdc0`, `0x813be10`, `0x813be20`, `0x813be30`, `0x813be40`, `0x813be50`,
`0x813be98` → registers `0x1137c88` … `0x1137d90`, values `0x00100000`,
`0x00200000`, `0x00280000`. Gated on the HAL instance's own byte `0x90`; the value
of that byte was **not** determined (see §6).

### Phase D — `EnableCPUClocksAndPower()` (`0x8f29080`), family `0xe0` / type `0x200`

Entry `0x8f29498`, `this+0x1cc == 0x200` check at `0x8f294a8`, body from `0x8f294b0`.

| Step | Address | Window | Offset | Value | Kind |
| --- | --- | --- | --- | --- | --- |
| D1 | `0x8f294bc` | PS | **`0xc000`** | `0x0000000f` | **W32** |
| D2 | `0x8f294d8` | PS | `0xc000` | validate `== 0x000000ff`, mask `0xff`, 5000 | polled read |
| D3 | `0x8f294ec` | — | — | `this[0x3a20] bit1` (`=1` on J700) → continue, else jump to `0x8f29220` | — |
| D4 | `0x8f294fc` | PWGATE | **`0x268c`** | `0x00000000` | **W32** |
| D5 | `0x8f29978` | PWGATE | `0x268c` | validate `== 0`, mask `3`, 5000 | polled read |
| D6 | `0x8f29990` | PS | **`0xc000`** | `0x1000000f` | **W32** |
| D7 | `0x8f29b0c` | PS | `0xc000` | validate `== 0x100000ff`, mask `0x100000ff`, 5000 | polled read |
| D8 | `0x8f29a2c` | RAM | — | `this[0x3765] := 1` | — |

**PWGATE `0x26b0` is not written on the on-path** — that is power-off only.

### Phase E — `EnableSubblocksClocksAndPower()` (`0x8f2740c`)

Family switch: `0xe0` → `0x8f277ac` → `0x8f27a9c`; `this+0x1cc == 0x200` → `0x8f27ab0`.

| Step | Address | Window | Offset | Value | Kind |
| --- | --- | --- | --- | --- | --- |
| E1 | `0x8f27abc` | PS | **`0xc008`** | `0x0000000f` | **W32** |
| E2 | `0x8f27e48` | PS | `0xc008` | validate `== 0x000000ff`, mask `0xff`, 5000 | polled read |

The `0xc010`, `0xc018`, `0xc020`, `0xc028`, `0xc030`, `0xc038`, `0x5b8`, `0x5c0`,
`0x4008`–`0x4030`, `0x3d0`, `0x3f00`–`0x3f20`, `0x10020` writes in this function
belong to **other families** (`0x40`, `0x60`, `0x70`, `0x80`, `0x220`, `0x300`, …),
not to J700.

### Phase F — `setPowerStateGatedPriv` (`0x8f59b84`) → `power_on_hardware` (`0x8f58994`)

| Step | Address | Note |
| --- | --- | --- |
| F1 | `0x8f58b80` | `blraa` slot `0x170` on the object at `this+0xe2b8` |
| F2 | `0x8f58b90` | `EnableANEClocksAndPower(this,1,1)` — **second** call, refcount path: D1–D8 again, E1–E2 again, `StartPerformanceCounters` (`0x8f58b90`-path `0x8f29f68`), then `this+0x3760 := 1`, `this+0x3765/0x3766 := 1`, `DisableANEClocksAndPower(this,0)` |
| F3 | `0x8f58bf8` | `this[0x3a20] != 0` → `EnableMPMClocksAndPower` at `0x8f58c10` is **skipped** (that would have written PS `0xc010 ← 0xf`, `0x8f2cb0c`) |
| F4 | `0x8f58d18` | `ANE_Init(this)` → Phase G |

### Phase G — `ANE_Init()` (`0x8f3dc28`), direct path (`this[0x200] bit0 == 0` → `0x8f3dd70`)

| Step | Address | Window | Offset | Value | Kind |
| --- | --- | --- | --- | --- | --- |
| G1 | `0x8f61244` | ANE | **`0x1880020`** | `0` | **W32** |
| G2 | `0x8f61254` | ANE | **`0x1880024`** | `0` | **W32** |
| G3 | `0x8f61264` | ANE | **`0x1880028`** | `0` | **W32** |
| G4 | `0x8f61274` | ANE | **`0x188002c`** | `0` | **W32** |
| G5 | `0x8f61284` | ANE | **`0x1880030`** | `0` | **W32** |
| G6 | `0x8f61294` | ANE | **`0x1880034`** | `0` | **W32** |
| G7 | `0x8f612a4` | ANE | **`0x1880038`** | `0` | **W32** |
| G8 | `0x8f612cc` | ANE | **`0x188003c`** | `0` | **W32** (tail `b` into `write32`) |
| G9 | `0x8f3dd84` | ANE | **`0x1880038`** | `0x00000001` | **W32** ← first non-zero engine write |
| G10 | `0x8f3de98` | ANE | **`0x188003c`** | `0x00000001` | **W32** |
| G11 | `0x8f3df18` | ANE | **`0x188003c`** | `0x00000000` | **W32** |
| G12 | `0x8f3df68` | ANE | `0x188003c` | read (log only) | read32 |
| G13 | `0x8f3e00c` | ANE | **`0x1050000`** | read64 one-shot; `tbnz w0,#0` → skip to G16 | read64 |
| G14 | `0x8f3e144` | ANE | **`0x1050000`** | `(([this+0x490]->[0x18]) & 0xff7dfffffffff800) \| 0x0081000000000001` — mask `0x8f3e014`/`0x8f3e018`, OR-constant `0x8f3e01c`/`0x8f3e020`, both combined at `0x8f3e038`/`0x8f3e0c0` | **W64** |
| G15 | *(skipped)* | — | — | `this[0xdb] bit0` = 0 → G13/G14 run; if set, both are skipped | — |
| **G16** | **`0x8f3e154`** | **ANE** | **`0x1600044`** | **`0x00000000`** | **W32 ← first CPU_CONTROL write** |
| G17 | `0x8f3e164` | ANE | `0x1600044` | `0x00000010` | **W32** |
| G18 | `0x8f3e17c` | ANE | `0x1880034` | `this[0xa4]` (guarded: `cbz w2` at `0x8f3e170`) | W32 |
| G19 | `0x8f3e190` | ANE | `0x188003c` | compared against `0x08042006` | read32 |

## 4. Direct answer: what we do that Apple also does

| Our current step | Apple equivalent | Match |
| --- | --- | --- |
| raise `ps_ane_sys` genpd | *no counterpart in the driver* — see below | **extra, not a substitute** |
| PS `0xc000 := 0xf` | D1 `0x8f294bc` | ✔ |
| PWGATE `0x268c := 0` | D4 `0x8f294fc` | ✔ |
| PS `0xc000 := 0x1000000f` | D6 `0x8f29990` | ✔ |
| CPU_CONTROL `:= 0` | G16 `0x8f3e154` | ✔ value, ✘ position |

## 5. What is missing before the first engine write

Ordered by how likely each is to be the cause of the hang.

**M1 — the provider clock and power votes (steps B1, B2).** These are the only
operations in the entire Apple bring-up that change the ANE block's gating, and they
happen *before* any ANE MMIO. They are IOKit virtual calls on `AppleARMIODevice`
(slots `0x8a8` / `0x8b0`) driven by DT `clock-ids` / `power-gates` / `service-gates`.
Linux raises the PMGR power domain `ps_ane_sys` but never the ANE clock gates, and
`ane` has no `clocks` property on this DTS. A write into a clock-gated Apple
peripheral block hangs the fabric with no kernel message — exactly the symptom
observed. **This is the prime suspect** and it is *not reproducible by raw MMIO*
until the PMGR gate identity behind clock ids 327/326/325 is resolved.

**M2 — the HAL tunable program (Phase C).** ~45 read-modify-writes into
ANE+`0x11378b8`…`0x1137d90`, plus the `0x18a4000`/`0x18a4008` state-machine
handshake, all performed by `com.apple.driver.AppleT8140ANEHAL` and reached through
`IOService::callPlatformFunction`, i.e. **another provider method, not driver MMIO**.
We do none of this. Note the ordering: Apple writes ANE `0x18a4000` and
`0x11378b8` **before** PS `0xc000`, which is only coherent if the provider clock
vote in B1/B2 already made the block accessible.

**M3 — position of the SCRATCH clears (G1–G8).** Apple writes SCRATCH0–7 = 0 and
then `0x1880038 := 1` *before* `CPU_CONTROL`. We write `CPU_CONTROL` first. This is
an ordering difference, not an omission, but it means our first write is a register
Apple touches ~10 MMIO operations later, after the SNE AF block has been configured
and locked.

**M4 — PS `0xc008 := 0xf` (E1).** Apple writes it on every enable. It already reads
`0xf` on the live part, so this is probably benign, but it is part of the sequence.

**Not required for J700** (present in the code, provably not taken):

- `InitANEScratchRegisters` inside `EnableANEClocksAndPower` (`0x8f2a6a8`) — gated on
  `this[0x200] bit0` (RTBuddy). Skipped.
- the ANE write at `0x8f2a698` — gated on `this[0x134]`, which family `0xe0` leaves at
  `0`.
- the 3-entry register table at `0xfffffe000c3f6e40` (`this+0x36f8`, count 3, offsets
  `0x748`/`0x848`/`0x948` with values `0x200030`/`0x100030`/`0x1000100`) — reachable
  only via `0x8f2a474`, which requires `this[0xeb] bit0 == 0`; J700 sets it to `1`
  at `0x8f650f0`. **The table is dead for J700.**
- `EnableMPMClocksAndPower` (PS `0xc010 := 0xf`) — gated on `this[0x3a20] == 0`;
  J700 has `3`.
- `Enable_ANESYS_PS` (`0x8f28bc0`, would write PS `0x9e0 := 0xf`) — **never called
  from anywhere in this kext** (zero `bl` sites to `0x8f28bc0`).
- the `AppleANETunableApplyFunctionLegacy` program — which is where the
  `sne_iso_gapf` (`0x18ac000`), `sne_mcw` (`0x1984000`), `sne_ctrl` (`0x1880000`),
  `sne_ctrl_lock` (`0x1988000`), `sne_soc_dpe_temp` (`0x186e000`) and
  `0x1960000`/`0x1974000`/`0x195c000` groups live, plus direct stores to
  `0x1960000`, `0x1974200`, `0x195c284`, `0x195c288`. The driver only selects this
  path when `this[0xec] bit0 == 0`, and J700 sets it to `1`.
  **So on J700 those blocks are not touched by this call at all** — they must be
  reached some other way, or not at all on this boot path.

## 6. Explicit "not found" statements

Searched the whole of `com.apple.driver.AppleH16ANEInterface` `__TEXT_EXEC`
(`0xfffffe0008eeb820`…`0xfffffe0008ff8dd8`, 275 822 instructions) and the whole of
`com.apple.driver.AppleT8140ANEHAL` `__text` (16 760 instructions):

- **No PMP / `function-pmp_control` / `"CPMP"` access.** The only PMP artefact is the
  DT property name `"pmp-virt-notify"` (`0xfffffe00073b4f5d`), read in
  `initializeANESoCConfig` `0x8f64b50`–`0x8f64b9c`, whose boolean result gates the
  *second* `enableDeviceClock` vote (clock index +1). No MMIO, no unlock.
- **No secure-lock *unlock* write in the ANE driver.** The driver's only
  "secure" strings are `aneExclaveSecureTransitionEventHandler`,
  `aneVnodeSecureLookup`, `ANE_ExclavePreparePowerOff` — exclave/IPC, not register
  locks. The `secure_lock_lock` / `secure_lock_wr_dis` machinery exists **only** in
  `AppleT8140ANEHAL`, and it *locks* (OR `0x100000`/`0x200000`/`0x280000`), it does
  not unlock. The only unlock-shaped operation in either kext is the
  `0x18a4000`/`0x18a4008` 2-bit state machine in C5–C8.
- **No hardware semaphore / spinlock register.** No atomic (`ldaxr`/`stxr`/`ldadd`/
  `cas`) instruction targets an ANE window address anywhere in either kext; all
  locking is `IOLock`/`IORecursiveLock`/`IOWorkLoop` gates on RAM.
- **No `setBit32` or `clearBit32` before `CPU_CONTROL`.** The only `setBit32` in
  `ANE_Init` is at `0x8f3e5f4` (ANE `this[0x11c]` = `0x1880060`, bit 0), after
  G16/G17. All earlier writes are plain `str`.
- **No other write to `0x1600044`.** `write32` call sites whose resolved offset is
  `this[0x160]` exist only at `0x8f3e154` and `0x8f3e164`.
- **No direct MMIO store in the driver that bypasses `ANERegisterControl`** on the
  bring-up path. `this+0x388`/`0x398`/`0x3a8`/`0x3b8` are loaded exactly 11 times,
  all inside `EnableANEClocksAndPower` (and once in `ANE_Init` at `0x8f438a4`,
  outside this path), and always passed as arguments — never used as a store base.
- **No `AppleARMIODevice` method reached as a symbol.** Only
  `__ZN16AppleARMIODevice9metaClassE` is imported. Slot numbers `0x8a8` / `0x8b0` /
  `0x6e8` are the only dispatch; the first two are identified as
  `enableDeviceClock` / `enableDevicePower` by argument shape plus the prior
  receipt, the third is `IOService::callPlatformFunction` by exact signature
  (`(OSSymbol*, bool, void*, void*, void*, void*)`) and by the callee comparing
  against OSSymbols.

## 7. What this report could not determine

1. **Whether M1 alone is sufficient to unfreeze the block**, and the PMGR gate
   identity behind clock ids 327/326/325. Not recoverable from this artifact: the
   index `[this+0x380]` is runtime, and the `AppleARMIODevice` translation tables are
   built from DT properties that do not exist on the Linux node.
2. **The HAL instance byte `0x90`** that gates the `c1ppt_rc` group (C12) in the
   online path. Set in `AppleT8140ANEHAL::start`/`init` from properties I did not
   decode; it decides whether registers `0x1137c88`…`0x1137d90` are touched.
3. **The tunable record stride.** `_tunableh_apply` (`0xfffffe00097a84a4`) computes a
   stride from the descriptor at `this+0x30` (byte `+0` → `0xc`, byte `+1` → `8`,
   else 0). The static tables at `0x813baf0`…`0x813bfb0` are only self-consistent as
   **8-byte `{offset, value}` records terminated by an all-zero record**, and that is
   what §3 lists; the descriptor resolution as I traced it yields `0xc`. I could not
   reconcile the two, so treat the C9–C12 register lists as *extracted from the
   static tables* rather than *proven to be the records the walker consumes*.
4. **`this+0x164`.** Never stored on family `0xe0` (the only writer, `0x8f67044`,
   belongs to a different family's case), so `ANE_Init`'s `write32(this[0x164], 1)`
   at `0x8f3e5c0` resolves to ANE offset `0x0`. Whether that is intended or a
   family-`0xe0` gap is unresolved. It is after `CPU_CONTROL`, so it does not affect
   the answer above.
5. **What `0x18a4010`/`0x18a4018` physically are.** They read as a 2-bit state plus
   a mask; `v == 3` means "already unlocked". Not identified against any published
   register map, and no M1 counterpart was searched.
6. **Whether the driver ever applies the Legacy tunable set on J700 through some
   other call.** I only proved it is not reached from `EnableANEClocksAndPower` on
   `this[0xec] bit0 == 1`.
7. **No device behaviour is established.** Every address above is a compiled offset.
   A covered offset is not a covered neighbourhood.
