# J700 / T8140 (H17): `CPU_STATUS`, the reset vector, and the ANE firmware-load path

> **Branch correction:** the warm SCRATCH7:=1 and cold SCRATCH7:=0 writes
> are alternative paths, not a sequential 1-to-0 handshake. See
> `../2026-11-04-j700-h17-order-helper/README.md`. Do not execute the
> flattened write recipe below as an exact Apple sequence.

Offline binary analysis only. **No device access, no module load, no MMIO, no
writes outside this directory.** Every claim below carries an instruction
address (and raw bytes where it decides the answer) so it can be re-checked.

| item | identity |
|---|---|
| kernelcache | `/var/tmp/neo/kernelcache.bin`, 114 688 000 B, SHA-256 `7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`, `VA = 0xfffffe0007004000 + file_off` |
| ANE driver | `com.apple.driver.AppleH16ANEInterface`, `__TEXT_EXEC.__text` `0xfffffe0008eeb820`…`0xfffffe0008ff8dd8` |
| ANE HAL | `com.apple.driver.AppleT8140ANEHAL`, `__text` `0xfffffe0009799040`…`0xfffffe00097a9620` |
| ADT | `/var/tmp/opencode/adt.bin`, 425 984 B, 325 nodes / 4 985 props, parsed with `/home/admin/linux-aurora/tools/aurora-sep/aurora-adt-extract.py` |
| firmware | `/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho`, 1 572 864 B, SHA-256 `32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c` |

---

## 0. Bottom line

1. **`0x1600048` is not a doorbell.** The driver's own register dump names it
   `rANE_ASCWRAP_IDLE_STATUS` (`0x8f2dd70` → string `0xfffffe00073d37ef`). The
   doorbell is a *different* register, engine offset **`0x1884000`**: at
   `0x8f4172c` `ANEHWDevice::doorBellRing` loads `w1 = [.. #0x158]` (= `0x1884000`)
   and `0x8f41730..0x8f41744` builds `w2 = 1 << channel` and tail-branches into
   `write32`. §2, §3.1.
2. **`0x6a → 0x68` means "STOPPED cleared", not "running" and not "held".**
   Bit 1 (STOPPED) goes 1 → 0 and bit 0 (RUNNING) never reads 1 — the same
   transition macOS's own `RUN` write produces on M3/M6. The core is no longer
   held in stop. `CPU_STATUS` lives in the ASC wrapper, so this does **not**
   prove the CPU clock is running; and the driver never uses `CPU_STATUS` as a
   "the CPU ran" signal at all. §3.4.
3. **The reset vector is iBoot's, not the driver's, on this device.** The ADT
   sets `pre-loaded = 1`, and `ANE_Init` then *skips both the RVBAR read and the
   RVBAR write* (`0x8f3dffc: 68 6e 03 39` / `0x8f3e000: 48 0a 00 37`). The ADT
   `segment-ranges` says the firmware's `__TEXT` is remapped at DVA
   **`0x100_0000_0000`**, so RVBAR (if macOS ever wrote it) would be
   `(0x10000000000 & 0xff7efffffffff800) | 0x0081000000000001` =
   **`0x0081_0100_0000_0001`**. §4.
4. **Firmware instruction memory is DRAM behind the ANE DART, not inside the
   32 MiB engine aperture.** Text at physical `0x100_0077_4000` (0xC0000), data
   at `0x100_019A_C000` (0x2AC000); DART `dart-ane` (compatible `dart,t8110`,
   16 KiB pages, `vm-base 0x100_0000_0000`), mappers `mapper-ane` (phandle
   `0x104`) and `mapper-ane-mpm` (phandle `0x10c`). The aperture `reg[0]` is
   `0x1_f000_0000`/32 MiB → physical **`0x4_0000_0000`** and contains only
   registers. §4.3.
5. **The readiness signal is `SCRATCH7` == `0x08042006`, polled for ~1000 ms;
   there is no `CPU_STATUS` readiness poll anywhere in the kext.** `SCRATCH7`
   is written by the firmware itself (two sites in the retained image use the
   exact constant), and macOS's `SCRATCH7 := 1` is logged as *"Writing 1 to
   rANE_SCRATCH7 to effect a WARM Boot"*. §6.
6. For the two candidate blockers: **(b) as stated is wrong** — on macOS the
   firmware is not loaded by the driver either (iBoot does it), so "Linux never
   copied firmware / never set RVBAR" cannot be what differs. What Linux
   demonstrably does *not* do is the rest of `ANE_Init` — the PS/PMP clock and
   power votes and the SCRATCH handshake — and that, not a missing load, is the
   first thing to replicate. **(a) cannot be ruled out from `CPU_STATUS`**; the
   one measurement that separates them is `SCRATCH7`. §7.

---

## 1. The offset table, and the correction it forces

`ANEHWDevice::initializeANESoCConfig` (`0xfffffe0008f64ae0`) fills per-family
register-offset fields. For **family `0xe0` (J700, ADT `ane-type = 0x200`)**
the block is `0x8f64f90`…`0x8f650a4`, and its anchor is
`0x8f64de4: add x23, x19, #0x104` (`77 12 04 91`), so every
`stp qN, qM, [x23]` lands at `this+0x104` and `this+0x114`:

```
0x8f65048  adrp x8,#0x73b0000 ; 0x8f6504c  ldr q0,[x8,#0x2d0]
0x8f65084  adrp x8,#0x73b0000 ; 0x8f65088  ldr q1,[x8,#0x2f0]
0x8f6508c  68 8a 0a ad   stp  q0, q1, [x23]        ; this+0x104 .. this+0x120
0x8f65090  88 0c 80 52   mov  w8, #0x64
0x8f65094  08 31 a0 72   movk w8, #0x188, lsl #16  ; 0x1880064
0x8f65098  68 26 01 b9   str  w8, [x19, #0x124]
0x8f65080  61 42 8f 3c   stur q1, [x19, #0xf4]     ; this+0xf4 .. this+0x100
```

Constant pools (`/var/tmp/neo/kernelcache.bin`):

| pool VA | 8 / 16 bytes | words |
|---|---|---|
| `0xfffffe00073b03b0` | `01000000 10c00000 20008801 24008801` | `0x1`, `0xc010`, `0x1880020`, `0x1880024` |
| `0xfffffe00073b02d0` | `28008801 2c008801 30008801 34008801` | `0x1880028`, `0x188002c`, `0x1880030`, `0x1880034` |
| `0xfffffe00073b02f0` | `38008801 3c008801 00c09601 60008801` | `0x1880038`, `0x188003c`, `0x196c000`, `0x1880060` |
| `0xfffffe00073b0480` | `00408801 48006001 10000000 08000000` | **`0x1884000`**, **`0x1600048`**, `0x10`, `0x8` |
| `0xfffffe00073b0260` | `00003701 04003701 00c08801 00008901` | `0x1370000`, `0x1370004`, `0x188c000`, `0x1890000` |

`0x8f64fc8: orr w8,w8,#0x200000` on `0x01400044` gives `0x01600044`, stored at
`this+0x160`; and `0x8f64fd8: str d0,[x19,#0x158]` stores the
`0x73b0480` pair, i.e. `this+0x158 = 0x1884000`, `this+0x15c = 0x1600048`.

| field | value | used as |
|---|---|---|
| `0xf4` | `1` | — |
| `0xf8` | `0xc010` | PS window (MPM) offset |
| `0xfc`…`0x118` | `0x1880020`…`0x188003c` | **SCRATCH0 … SCRATCH7** (4-byte stride) |
| `0x11c` | `0x196c000` | `setBit32(.,0)` after the ready token |
| `0x120` | `0x1880060` | `write32(.,0)` after the ready token |
| `0x124` | `0x1880064` | — |
| `0x128`/`0x12c` | `0x1880058`/`0x188005c` | `write32(.,1)` after the ready token |
| `0x138` | `0x01c21060` | — |
| `0x148`/`0x14c` | `0x1370000`/`0x1370004` | — |
| `0x150`/`0x154` | `0x188c000`/`0x1890000` | — |
| **`0x158`** | **`0x1884000`** | **doorbell** (`doorBellRing`, `0x8f4172c`) |
| **`0x15c`** | **`0x1600048`** | **`CPU_STATUS` / ASCWRAP_IDLE_STATUS** |
| **`0x160`** | **`0x1600044`** | **`CPU_CONTROL`** |
| `0x164` | never stored on `0xe0` → `0` | `write32(0,1)` guard is dead |

> This corrects one row of
> `receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md`
> §1: `0x11c` is `0x196c000` and `0x120` is `0x1880060`; the earlier note had
> `0x11c → 0x1880060` and `0x124 → 0x1880064` (the latter *is* right, but it is
> a separate literal at `0x8f65098`, not part of the `q1` vector). Nothing in
> that receipt's conclusion depends on it.

MMIO accessors are plain, bounds-free loads/stores on `[obj+8]`:

```
0x8f73fe0  08 04 40 f9   ldr x8,[x0,#8]      ; ANERegisterControl::read32
0x8f73fe4  00 49 61 b8   ldr w0,[x8,w1,uxtw]
0x8f73ff0  08 04 40 f9   ldr x8,[x0,#8]      ; read64OneShot
0x8f73ff4  00 49 61 f8   ldr x0,[x8,w1,uxtw]
0x8f74134  08 04 40 f9   ldr x8,[x0,#8]      ; write32
0x8f74138  02 49 21 b8   str w2,[x8,w1,uxtw]
0x8f74148  … setBit32 = ldr w9 / orr (1<<bit) / str
```

---

## 2. Which window is which (physical addresses)

`/arm-io/ane` `reg[]` is six `{addr u64, size u64}` entries; `/arm-io` `ranges[]`
maps child `[0, 0x2f0000000)` to parent `+0x210000000`
(`python3 scripts/fw.py regs` prints both from the ADT):

| idx | driver name (`0x8f53b14`…) | DT address | size | **physical** |
|---|---|---|---|---|
| 0 | `ANE` | `0x1_f000_0000` | `0x0200_0000` | **`0x4_0000_0000`** |
| 1 | `PS` | `0xf0700000` | `0x18000` | **`0x3_0070_0000`** |
| 2 | `PWGATE` | `0xf0724000` | `0x4000` | `0x3_0072_4000` |
| 3 | `PTD` | `0xf03c0000` | `0x30000` | `0x3_003c_0000` |
| 4 | `ANEHAL1` | `0x01000000` | `0xff4000` | `0x2_1100_0000` |
| 5 | `ANEHAL2` | `0xf82c8000` | `0x4000` | `0x3_082c_8000` |

The names come from the strings passed to
`ANERegisterControl::createWithProvider` (`0x8f73edc`) in
`ANEHWDevice::InitializeProvider` (`0x8f5293c`): `0xfffffe00073b3780` =
`"ANE"`, `0xfffffe00073b421a` = `"PS"`, `0xfffffe00073b421d` = `"PWGATE"`,
`0xfffffe00073b4224` = `"PTD"`, `0xfffffe00073b4228` = `"ANEHAL1"`.

So the PS window is `/arm-io/pmgr` `reg[0]` itself — the same window the
2026-11-04 clkgen receipt derived independently (`0x3_0070_0000`). Every
"ANE offset" in the driver's ANE window is `0x4_0000_0000 + offset` on the bus;
`CPU_CONTROL` is `0x4_0160_0044`, `CPU_STATUS` `0x4_0160_0048`, `RVBAR`
`0x4_0105_0000`, `SCRATCH0..7` `0x4_0188_0020`…`0x4_0188_003c`, doorbell
`0x4_0188_4000`.

### 2.1 A negative result about the mailbox offset

`ane/h17/ane_h17_soc.c:75` carries `.mbox = 0x1608000` with the comment
*"candidate: T8132 template"*. **This kext never references `0x01608000`**:

* `scripts/scan.py str 0x1600 iface` — nothing (no `movz/movk` builds it);
* a 4-byte scan of the kext's whole `__const`
  (`0xfffffe00073aff70`…`0xfffffe00073b0c80`) finds no such word; the offsets
  family `0xe0` actually installs are `0x1880xxx` (SCRATCH/ready/doorbell),
  `0x1600044`/`0x1600048`, `0x1050000`, `0x196c000`, `0x1370000`/`0x1370004`,
  `0x188c000`, `0x1890000`;
* a 4-byte scan of the entire 114 688 000-byte kernelcache finds the word
  `0x01608000` exactly once, at `0xfffffe00076a7cbf`, outside this kext.

So `0x1608000` is not evidence for anything here; which register the firmware
command ring uses on J700 is *not* resolved by this receipt (§8, item 8).

---

## 3. Q1 — `CPU_STATUS` semantics

### 3.1 The driver's own name for `0x1600048`

`ANEHWDevice::DumpANERegisters` reads `[this+0x15c]` and prints it under the
label the driver picked:

```
0x8f2dd48  adrp x3,#0x73d3000 ; 0x8f2dd4c add x3,#0x7d6   ; "Starting CPU registers:\n"
0x8f2dd60  c0 92 41 f9   ldr  x0,[x22,#0x320]              ; ANE register control
0x8f2dd64  a1 5e 41 b9   ldr  w1,[x21,#0x15c]             ; == 0x1600048
0x8f2dd68  9d 18 01 94   bl   0x8f73fdc (read32)
0x8f2dd70  adrp x3,#0x73d3000 ; 0x8f2dd74 add x3,#0x7ef
             ; 0xfffffe00073d37ef = "   rANE_ASCWRAP_IDLE_STATUS = 0x%x\n"
```

The next line names a *literal* register `0x01010310`
(`0x8f2dd8c: mov w1,#0x310 ; 0x8f2dd90: movk w1,#0x101,lsl#16`) as
`rANE_ASC_CPU_EDPRCR` (`0xfffffe00073d3813`), and the eight SCRATCH reads above
it are labelled `rANE_SCRATCH0`…`rANE_SCRATCH7`
(`0xfffffe00073d370e`…`0xfffffe00073d37bd`).

So the register's driver-side identity is **ASC wrapper idle status**. Its
counterpart `0x1600044` is `CPU_CONTROL` (the only two writes to it are the two
in `ANE_Init`, §5.4).

### 3.2 Every read of it in the whole kext

`this+0x15c` is read at exactly **two** sites (scan of the full 275 822
instruction `__TEXT_EXEC`; `scripts/scan.py disp 0x15c iface`):

| VA | function | use |
|---|---|---|
| `0x8f2dd64` | `DumpANERegisters` | debug print only |
| `0x8f42ff4` | `ANE_deInit` | **the only poll** (§3.3) |

There is no write to it anywhere (no `write32`/`setBit32`/`clearBit32` call site
resolves to `this+0x15c`), and no code builds the literal `0x1600048`:
`scripts/scan.py str 0x1600 iface` and `… hal` both return nothing, so every
access goes through the offset field.

### 3.3 The one poll, and what it waits for

`ANE_deInit` (`0xfffffe0008f420b0`), around `0x8f42fe0`:

```
0x8f42fe0  28 0b 40 b9   ldr  w8,[x25,#8]
0x8f42fe4  1f bd 00 71   cmp  w8,#0x2f
0x8f42fe8  68 00 80 52   mov  w8,#3
0x8f42fec  17 c5 9f 1a   csinc w23,w8,wzr,gt        ; w23 = (f>0x2f) ? 3 : 1
0x8f42ff0  20 5f 40 f9   ldr  x0,[x25,#0xb8]
0x8f42ff4  81 5e 41 b9   ldr  w1,[x20,#0x15c]       ; 0x1600048
0x8f42ff8  f9 c3 00 94   bl   0x8f73fdc              ; read32
0x8f42ffc  f3 03 00 aa   mov  x19,x0
0x8f43000  1f 00 17 6a   tst  w0,w23                ; bit0, plus bit1 when f>0x2f
0x8f43004  61 02 00 54   b.ne 0x8f43050             ; -> proceed
0x8f43008  00 e2 84 52   mov  w0,#0x2710            ; 10000
0x8f4300c  2d 74 b1 94   bl   0xbba00c0              ; IODelay(10000 us)
0x8f43010  b5 06 00 11   add  w21,w21,#1
0x8f43014  bf 52 00 71   cmp  w21,#0x14             ; 20 tries = 200 ms
0x8f43018  c1 fe ff 54   b.ne 0x8f42ff0
```

The mask is `{1}` or `{1,3}` depending on a flag at `[x25+8] > 0x2f`; both
variants always test **bit 0**. So the driver's only semantic use of
`CPU_STATUS` is "wait until bit0 (and optionally bit1) reads 1 before tearing
down". A timeout is logged, not fatal.

### 3.4 Bit map, and the verdict on `0x6a → 0x68`

The kernelcache does not name individual bits. The bit names come from
measurements on other Tape-out parts, recorded in this repo, and are quoted
here for what they are:

* `ane/h15/ane_h15_main.c` (`ane/h15/run-stage-sources.md` pins the sources):
  *"m1n1 R_CPU_STATUS: STOPPED bit 1 and IDLE bit 5 set, RUNNING bit 0 clear …
  On the M6 the same RUN write took CPU_STATUS from 0xa to 0x8 in under 11 ms:
  STOPPED clears, RUNNING (bit 0) does not read 1"*; the constants are
  `ANE_H15_ST_RUNNING = BIT(0)`, `ANE_H15_ST_STOPPED = BIT(1)`.
* `ane/h13/ane_h13_perf.c` records the same encoding on H13
  (`#define ASC_CPU_STOPPED BIT(1)`, "0x2a parked").
* T6030 measured `0x0030002a` (stopped) → `0x00300028` (started).

Live J700 values:

| value | bits | reading |
|---|---|---|
| `0x6a` = `0b0110_1010` | 1, 3, 5, 6 | STOPPED set, IDLE set (+ bits 3, 6) |
| `0x68` = `0b0110_1000` | 3, 5, 6 | **STOPPED cleared**, IDLE set, bits 3/6 unchanged |

**Verdict: the `CPU_CONTROL := 0x10` write landed and cleared STOPPED.** The
delta is exactly the M6/M3 delta (bit 1 cleared, bit 0 never set, IDLE
untouched); no other bit moved. `0x68` therefore does **not** mean "held in
reset" — a held core on this family reads STOPPED set, as `0x6a` did.

**But it also does not mean "executing".** Two independent reasons:

1. Bit 0 (RUNNING) has never read 1 on any ANE measured so far, so "RUNNING
   set" is not an available test; the transferred state is "not stopped", and
   IDLE (bit 5) being **set** is the state a started-but-idle core shows.
2. `CPU_STATUS`/`CPU_CONTROL` are in the ASC *wrapper*, which is in an
   always-on domain — the driver reads it with the core stopped at boot. A
   wrapper state change proves the control path to the CPU is alive; it does
   **not** prove the CPU's own clock is running.

So: the write is not the thing that is broken, and `0x68` is not evidence of a
gated clock. It is evidence that the reset/stop release worked. Whether the
core then *fetches* is decided elsewhere (§6).

---

## 4. Q2 — the reset vector and where the firmware lives

### 4.1 The RVBAR block, verbatim

`ANEHWDevice::ANE_Init` (`0xfffffe0008f3dc28`), the RVBAR section:

```
0x8f3dffc  68 6e 03 39   ldrb w8,[x19,#0xdb]          ; fw-preloaded flag
0x8f3e000  48 0a 00 37   tbnz w8,#0,0x8f3e148         ; set -> skip RVBAR entirely
0x8f3e004  80 9b 40 f9   ldr  x0,[x28,#0x130]         ; ANE reg control
0x8f3e008  a1 20 a0 52   mov  w1,#0x1050000           ; RVBAR
0x8f3e00c  f8 d7 00 94   bl   0x8f73fec              ; read64OneShot
0x8f3e010  c0 09 00 37   tbnz w0,#0,0x8f3e148         ; bit0 already 1 -> skip
0x8f3e014  f5 ff 80 92   mov  x21,#-0x800
0x8f3e018  d5 ef ff f2   movk x21,#0xff7e,lsl#48      ; x21 = 0xff7efffffffff800
0x8f3e01c  34 00 80 d2   mov  x20,#1
0x8f3e020  34 10 e0 f2   movk x20,#0x81,lsl#48        ; x20 = 0x0081000000000001
0x8f3e02c  68 4a 42 f9   ldr  x8,[x19,#0x490]         ; ANESharedMemorySurfaceParams*
0x8f3e030  08 0d 40 f9   ldr  x8,[x8,#0x18]           ; -> its DART device address
0x8f3e034  08 01 15 8a   and  x8,x8,x21
0x8f3e038  15 01 14 aa   orr  x21,x8,x20
             ; (the log-path twin at 0x8f3e0b4..0x8f3e0c0 computes the same value)
0x8f3e138  80 9b 40 f9   ldr  x0,[x28,#0x130]
0x8f3e13c  a1 20 a0 52   mov  w1,#0x1050000
0x8f3e140  e2 03 15 aa   mov  x2,x21
0x8f3e144  ea d7 00 94   bl   0x8f740ec              ; write64(0x1050000, x21)
0x8f3e148  80 9b 40 f9   ldr  x0,[x28,#0x130]         ; <-- CPU_CONTROL follows
```

Both halves are proven by instruction stream. Two details that matter:

* **The mask is `0xff7e_ffff_ffff_f800`, not `0xff7d…`** as noted in the brief.
  `0x7e = 0111_1110` preserves address bit 49 and clears bit 48; `0x7d` would
  clear bit 49. The consequence: **bit 48 of the source address is discarded**
  and then forced to 1 by the OR; bits 11:0 are cleared and bit 0 forced to 1.
* Bit 0 is a "valid" flag the driver itself sets, and the skip at `0x8f3e010`
  tests that same bit — i.e. the write is idempotent across a warm restart.

### 4.2 What address that encodes

Source `[this+0x490]->[0x18]` is the DART device address of the "ANECPU
Firmware" surface: `DartMapSharedMemorySurface` (`0x8f4aed4`) passes
`x3 = params+0x18` (`0x8f4afa8: add x3,x19,#0x18`) as the out-pointer of the
mapping call and later logs it (`0x8f4b054: ldr x3,[x19,#0x18]`). The surface is
created in `ANEHWDevice::start` at `0x8f55fa0`…`0x8f55fd4`
(`AllocateSharedMemorySurface(size=this[0x3c0], &this[0x490], …, tag 0x4657494d)`),
with `this[0x3c0] = 0x380000` (`0x8f64fe8: mov w9,#0x380000`).

For the J700 firmware the DVA is not a guess — the ADT states it (§4.3):
`__TEXT` remap = `0x100_0000_0000`. Therefore

```
RVBAR = (0x0000_0100_0000_0000 & 0xff7e_ffff_ffff_f800) | 0x0081_0000_0000_0001
      =  0x0000_0100_0000_0000 | 0x0081_0000_0000_0001
      =  0x0081_0100_0000_0001
```

Reproduce: `python3 scripts/fw.py rvbar 0x10000000000`.

Two honest caveats:

* Bits 63:48 of the result are `0x0081` **only** because software forces them;
  any source address bits there are cleared first. What those two bits mean to
  the CPU (a boot-mode tag, a DART stream select, a validity pair) is **not
  derivable offline** — no other reader or writer of this field exists in the
  kext.
* On this device the kext never performs the write at all (§4.4), so the value
  actually in the register is iBoot's. `segment-ranges` record 0 carries
  `flags = 1`, which is exactly the bit-0 "valid" flag, so the live value is
  most plausibly `0x0000_0100_0000_0001` — the same encoding the M3 (T6030) and
  M6 (T8152) were measured with (`IORVBAR 0x10000000001`,
  `ane/h15/README-bringup.md`, `CHANGELOG.md`). A single `readq` of
  `0x401050000` settles it; it is not available offline.

### 4.3 Aperture or DART? — the ADT answers it

`/arm-io/ane` `segment-ranges` (64 B = two 32-byte records
`{phys, iova_offset, remap, size | flags<<32}`; the layout is the one decoded in
`ane/h15/h15-segments.py`, and the driver's own log strings confirm the four
fields — see §5.2):

| rec | phys | iova_offset | remap (DVA) | size | flags |
|---|---|---|---|---|---|
| 0 `__TEXT` | `0x0100_0077_4000` | `0x0` | **`0x0100_0000_0000`** | `0xc0000` | `1` |
| 1 `__DATA` | `0x0100_019A_C000` | `0xc0000` | `0x0100_000C_0000` | `0x2ac000` | `0` |

`0xc0000 + 0x2ac000 = 0x380000` = `this[0x3c0]` = the FWIM surface size. The
retained firmware image agrees to the byte:

```
$ python3 scripts/fw.py macho
filetype 5 (MH_PRELOAD)
SEG __TEXT       vmaddr 0x0       vmsize 0xc0000   fileoff 0x4000  filesize 0xc0000
SEG __DATA       vmaddr 0xc0000   vmsize 0x2ac000  fileoff 0xc4000 filesize 0xbc000
SEG __DATA_CONST vmaddr 0x36c000  vmsize 0x0
```

DART geometry, also from the ADT (`scripts/fw.py regs`, plus a phandle walk):

| item | value |
|---|---|
| `/arm-io/dart-ane` | compatible `dart,t8110`, `page-size 0x4000`, `sid <0 0xb 0xf 0xa 0xd>`, `sid-count 0x10`, `exclave-sid <1..7>`, `dart-id 0x14`, `vm-base <0x0 0x100>` = `0x100_0000_0000`, `vm-size <0x0 0x300>` |
| `/arm-io/ane` `iommu-parent` | `<0x104 0x10c>` → `mapper-ane` (reg 0) and `mapper-ane-mpm` (reg 0xb), both `compatible = iommu-mapper` |

**Verdict:** the firmware is DRAM, mapped into the ANE DART's device address
space at `0x100_0000_0000`/`0x100_000C_0000`. It is **not** inside the 32 MiB
engine aperture: `reg[0]` (`0x4_0000_0000`) holds only the register file the
driver pokes (`0x1600044`, `0x1880020…`, `0x1050000`, `0x11378b8…`).

### 4.4 Why the kext does not write it on J700

`initializeANEProperties` (`0xfffffe0008f6406c`) writes `this+0xdb = this+0xdc = 1`
(`0x8f641c8: mov w25,#0x101` / `0x8f641cc: sturh w25,[x19,#0xdb]`) and then
`ANEHWDevice::start` re-decides both from DT properties read on the provider
(slot `0x2d8`, `blraa` at `0x8f537f4` / `0x8f538d8`):

```
0x8f537e8  adrp x1,#0x73b4000 ; add x1,#0x201   ; "pre-loaded"     (0x73b4201)
0x8f537fc  cbz  x0,0x8f53838                    ; absent -> log + db=dc=0
0x8f53898  mov  w8,#1
0x8f5389c  68 6e 03 39   strb w8,[x19,#0xdb]     ; fw pre-loaded  -> 1
0x8f538c4  adrp x1,#0x73b4000 ; add x1,#0x20c   ; "ctrr-unlocked"  (0x73b420c)
0x8f539a8  68 72 03 39   strb w8,[x19,#0xdc]     ; -> 1 only when that property exists
0x8f539a4  7f 6e 03 39   strb wzr,[x19,#0xdb]    ; (the "not preloaded" path zeroes both)
```

with the log strings `0xfffffe00073dc670` *"%s: %s: usb get preloads the fw is
true, fEnableFwCTRR = true"*, `0xfffffe00073dc6f0` *"Device ctrr-unlocked is
set"*, `0xfffffe00073dc6af` *"… preloaded flag is not set, fEnableFwCTRR =
false"*.

The J700 ADT has `pre-loaded = <1>` and **no `ctrr-unlocked`**, so
`this[0xdb] = 1`, `this[0xdc] = 0`. Consequences, all proven by the branch at
`0x8f3e000` / `0x8f3e010`:

* `ANE_Init` performs **neither** `read64(RVBAR)` **nor** `write64(RVBAR)`.
* `ANE_LoadFirmware_gated` refuses to reload: `0x8f46434..0x8f46440`
  (`tbz` on `0xdb`, then `tbz` on `0xdc` → `0x8f46650`) logs
  *"FW reload is not allowed for CTRR enviorment, fEnableFwReload is not set in
  the decide tree"* (`0xfffffe00073d9420`) and returns an error without copying.
* `mapFwCTRRRegion` runs instead of any copy (§5.2).

---

## 5. Q3 — the firmware path

### 5.0 Which `ANE_Init` branch this device takes

`ANE_Init` has two shapes, selected by `this[0x200] bit0`:

```
0x8f3dd28  68 02 48 39   ldrb w8,[x19,#0x200]
0x8f3dd2c  28 02 00 36   tbz  w8,#0,0x8f3dd70     ; 0 -> direct path
0x8f3dd34  ab 50 00 94   bl   0x8f51fe0            ; InitializeRTBuddyEndpoints()
0x8f3dd38  80 21 00 37   tbnz w0,#0,0x8f3e168      ; ok -> SKIP all MMIO, go to the SCRATCH7 poll
```

and `this[0x200]` is only ever written 1 by `InitializeProvider`
(`0x8f52b84`, after `InitializeRTBuddyClient` returns 0 at `0x8f52aac`).
`InitializeRTBuddyClient` (`0x8f52634`) needs a firmware-provider nub; the J700
ADT has **no `iop-ane-nub`** (the only `iop-*-nub` nodes are
`/arm-io/gfx-asc/iop-gfx-nub` and `/arm-io/gfx1-asc/iop-gfx1-nub`), so the
RTBuddy branch is unavailable and `this[0x200] = 0`.

**Therefore J700 takes the direct path**, and `CPU_CONTROL` *is* part of
macOS's boot on this device. (Observational corollary, useful as a check on a
macOS MMIO capture: on a device that took the RTBuddy path the kext would never
write `0x1600044` at all, since those are its only two write sites.)

### 5.1 The two firmware paths

| | pre-loaded (`0xdb = 1`, J700) | not pre-loaded |
|---|---|---|
| who places the image | **iBoot** (physical `0x100_0077_4000` / `0x100_019A_C000`) | the kext copies it |
| who maps it | iBoot, re-created by `mapFwCTRRRegion` | `DartMapSharedMemorySurface` on the FWIM surface |
| who sets RVBAR | **iBoot** | `ANE_Init` (§4.1) |
| can macOS reload it | no (`0x8f46650`, "FW reload is not allowed") | yes, `ANE_LoadFirmware` |

### 5.2 Path A, pre-loaded: `mapFwCTRRRegion` (`0xfffffe0008f43dec`)

Proven by instruction stream, in order:

1. `0x8f43e30..0x8f43e70` — `provider->getProperty` (slot `0x2d8`) with
   `x1 = 0xfffffe00073b37bf…` = **`"segment-ranges"`**. Missing → error
   `0xE00002C7` and the log at `0xfffffe00073d8d3e`
   *"mapFwCTRRRegion segment-ranges is not in the device tree"*.
2. `0x8f43e74..0x8f43ed8` — the `OSData` bytes accessor
   (`bl 0xfffffe000bb29ac8`), then its length (virtual slot `0x160`), then
   `0x8f43ee4: cmp w0,#0x3f` — the property must be at least 64 B, i.e. the two
   32-byte records of §4.3.
3. Logs each segment with the driver's own field names (so the ADT layout of
   §4.3 is confirmed by the kext, not assumed):
   * `0xfffffe00073d8956` *"%s: %s: FW __TEXT segment phy: 0x%llx, virt:
     0x%llx, remap: 0x%llx, size: 0x%x"* (log call at `0x8f44024`)
   * `0xfffffe00073d89a6` the same for `__DATA` (`0x8f44098`); the arguments
     are `[rec+0]`, `[rec+0x10]`, `[rec+0x18]` loaded at
     `0x8f43ff0..0x8f43ffc` and `0x8f44064..0x8f44070`.
4. `0x8f4409c..0x8f440d4` — builds two `{phys, round_up(size, this[0x3620])}`
   pairs (`udiv`/`mul` by the mapper's page size; `this[0x3620]` is stored from
   IOMapper code slot `0x888` in `start` at `0x8f554fc`, i.e. 16 KiB for
   `dart-ane`).
5. `0x8f440f0` / `0x8f44114` — `bl 0xfffffe000bc1f7f0` twice with
   `(ptr-to-pair, 1, 0, 0, w4 = 0x21 for __TEXT / 0x23 for __DATA, x5 =
   this[0x3d0])`; the results (IOMemoryDescriptors) are stored at
   `this+0x270` and `this+0x278`.
6. `0x8f4431c..0x8f44368` — IOMapper (`this[0x3d0]`) code slot **`0x890`**,
   args `(x1=0, x2=0, w3=this[0x3c0]=0x380000, w4=3, x5..x7=0, [sp]=&out1,
   [sp+8]=&out2)`. Out1 (`[x29-0x68]`) is used as the DVA of `__TEXT`.
7. `0x8f44370..0x8f443b8` — IOMapper slot **`0x8a0`**,
   `(w1=1, x2=DVA, x3=0, x4/x5 = {phys,size} of __TEXT)`.
8. `0x8f44490..0x8f444a8` — `setDartAllocationInfo(DVA, size, 0x1014, …)`
   (`__ZN11ANEHWDevice21setDartAllocationInfoEyy20ANEResourceUsageType…`);
   then `0x8f444b8..0x8f444c4` advances the DVA by the size of `__TEXT`, so
   `__DATA` lands at `DVA + 0xc0000` — exactly `segment-ranges`' `iova_offset`.

Not resolved offline: **the concrete `IOMapper` method names behind slots
`0x890` and `0x8a0`.** `IOMapper` lives in `com.apple.kernel`, whose symbols are
not in the retained kernelcache, so the roles ("allocate a 0x380000-byte device
VA range", "insert a physical range at a device VA") are inferred from argument
shape, and from the fact that the allocate size equals
`this[0x3c0] = 0x380000` and the second call consumes
`{phys, round_up(size, page)}` plus the returned DVA. The *sequence* is proven;
the *names* are not.

Note what is **not** passed: the ADT `remap` value. It is only logged. macOS
therefore relies on the DART allocator returning the same base iBoot used
(`0x100_0000_0000`), which is what `vm-base` being the first allocatable address
gives. A Linux port cannot assume that and should install the mapping at the
`remap` address explicitly, or skip the mapping entirely if iBoot already
established it and RVBAR already matches.

### 5.3 Path B, cold load: `ANE_LoadFirmware_gated` (`0xfffffe0008f46154`)

Reached via `ANEHWDevice::ANE_LoadFirmware` (`0x8f5ba98`), which is a
tail-call shim that re-dispatches under `this[0x2c0]`'s lock. Proven order:

1. `0x8f462c8..0x8f46428` — `0x8f462c8` compares the requested size against
   `this[0x3c0]`, `0x8f462e0..0x8f462ec` loads
   `(x0=userAddr, x1=size, w2=2 /*kIODirectionOut*/, x3=task)` and
   `0x8f462f0: bl 0xfffffe000bc1f480` creates the
   `IOMemoryDescriptor::withAddressRange`; then virtual slot `0x218`
   (`w1=0`, prepare) at `0x8f46314` and slot `0x228` (`w1=0x1000`, map with
   4 KiB granule) at `0x8f46410`.
2. `0x8f46434..0x8f46440` — the `this[0xdb]` / `this[0xdc]` gate of §4.4.
3. Non-preloaded path, `0x8f464e0..0x8f4653c`:
   ```
   0x8f464e0  ldr  w1,[x20,#0x3c0]      ; 0x380000
   0x8f464e4  ldr  x8,[x20,#0x490]      ; FWIM surface
   0x8f464e8  ldr  x0,[x8,#0x38]        ; host VA of the surface
   0x8f464ec  bl   0xb394da0            ; bzero(dst, 0x380000)
   0x8f46514  add  x8,x16,#0x138
   0x8f46518  ldr  x9,[x16,#0x138]      ; IOMemoryMap::getAddress()
   0x8f46528  blraa x9
   0x8f46530  ldr  x8,[x20,#0x490]
   0x8f46534  ldr  x0,[x8,#0x38]
   0x8f46538  mov  x2,x23               ; size
   0x8f4653c  bl   0xb394bf0            ; memcpy(dst, src, size)
   0x8f46540  str  w24,[x20,#0x484]     ; this->fwLoadedSize = size
   ```
4. `0x8f46544..0x8f4655c` — the CTRR branch (`this[0xdb]`) or the plain path.
5. `0x8f46604..0x8f46700` — coredump / trace bookkeeping, `this+0x480 = 0`.

The FWIM surface itself is created in `ANEHWDevice::start`:

```
0x8f55fa0  ldr  w1,[x19,#0x3c0]     ; 0x380000
0x8f55fac  add  x2,x19,#0x490       ; &this->fwSurface
0x8f55fb8  mov  x0,x19
0x8f55fbc  mov  w3,#1
0x8f55fc0  mov  w4,#0x494d          ; 'MI'|'WF' tag 0x4657494d
0x8f55fc4  movk w4,#0x4657,lsl#16
0x8f55fd4  bl   0x8f4b5e0           ; AllocateSharedMemorySurface(...)
```

### 5.4 The ordered bring-up recipe (physical addresses)

`P` = pre-loaded (J700), `C` = cold-load path. Every row is a *register write the
instruction stream performs*; the "source" column says which function.

| # | window | offset | phys address | value | source | P | C |
|---|---|---|---|---|---|---|---|
| 1 | PS | `0xc000` | `0x3_0070_c000` | `0x0000000f` | `EnableCPUClocksAndPower` `0x8f294bc` | ✔ | ✔ |
| 2 | PS | `0xc000` | `0x3_0070_c000` | poll `&0xff == 0xff`, 5000 | `0x8f294d8` | ✔ | ✔ |
| 3 | PWGATE | `0x268c` | `0x3_0072_668c` | `0x00000000` | `0x8f294fc` | ✔ | ✔ |
| 4 | PWGATE | `0x268c` | `0x3_0072_668c` | poll `&3 == 0`, 5000 | `0x8f29978` | ✔ | ✔ |
| 5 | PS | `0xc000` | `0x3_0070_c000` | `0x1000000f` | `0x8f29990` | ✔ | ✔ |
| 6 | PS | `0xc000` | `0x3_0070_c000` | poll `&0x100000ff == 0x100000ff` | `0x8f29b0c` | ✔ | ✔ |
| 7 | PS | `0xc008` | `0x3_0070_c008` | `0x0000000f` | `EnableSubblocksClocksAndPower` `0x8f27abc` | ✔ | ✔ |
| 8 | PS | `0xc008` | `0x3_0070_c008` | poll `&0xff == 0xff` | `0x8f27e48` | ✔ | ✔ |
| 9 | ANE | `0x1880020…0x188003c` | `0x4_0188_0020…3c` | `0` (8 words) | `InitANEScratchRegisters` `0x8f6121c` | ✔ | ✔ |
| 10 | ANE | `0x1880038` | `0x4_0188_0038` | `1` (SCRATCH6) | `0x8f3dd84` | ✔ | ✔ |
| 11 | ANE | `0x188003c` | `0x4_0188_003c` | `1` (SCRATCH7, **warm-boot request**) | `0x8f3de98` | ✔ | ✔ |
| 12 | ANE | `0x188003c` | `0x4_0188_003c` | `0` | `0x8f3df18` | ✔ | ✔ |
| 13 | ANE | `0x1050000` | `0x4_0105_0000` | `read64`; skip if bit0 | `0x8f3e00c` | **skipped** | ✔ |
| 14 | ANE | `0x1050000` | `0x4_0105_0000` | `(dva & 0xff7efffffffff800) \| 0x0081000000000001` | `0x8f3e144` | **skipped** | ✔ |
| 15 | ANE | `0x1600044` | `0x4_0160_0044` | `0x00000000` | `0x8f3e154` | ✔ | ✔ |
| 16 | ANE | `0x1600044` | `0x4_0160_0044` | `0x00000010` (RUN, bit 4) | `0x8f3e164` | ✔ | ✔ |
| 17 | ANE | `0x1880034` | `0x4_0188_0034` | `this[0xa4]` **only if non-zero** — always 0 on J700 (`0x8f641d4`) | `0x8f3e17c` | n/a | n/a |
| 18 | ANE | `0x188003c` | `0x4_0188_003c` | poll `== 0x08042006`, `IOSleep(1)` ×1000 | `0x8f3e188`…`0x8f3e1e0` | ✔ | ✔ |
| 19 | ANE | `0x1880058` | `0x4_0188_0058` | `1` | `0x8f3e568`…`0x8f3e578` (field `0x128`) | ✔ | ✔ |
| 20 | ANE | `0x1880060` | `0x4_0188_0060` | `0` | `0x8f3e57c`…`0x8f3e58c` (field `0x120`) | ✔ | ✔ |
| 21 | ANE | `0x196c000` | `0x4_0196_c000` | `setBit32(.,0)` | `0x8f3e5e4`…`0x8f3e5f4` (field `0x11c`) | ✔ | ✔ |

Steps 1–8 are quoted from the previous round
(`j700-h17-first-engine-write.md` §3 phases B–F) and re-anchored here to
physical addresses via §2. Steps 9–21 are re-derived in this receipt from the
instruction stream at the stated VAs. Before step 9 `EnableANEClocksAndPower`
also makes two provider votes, `enableDeviceClock(1, this[0x380])` and
`enableDevicePower(1, &out, this[0x380])` (`0x8f2a060`, `0x8f2a0a8`), which on
this SoC resolve to a PMP mailbox command and not to a register write
(`2026-11-04-j700-h17-clkgen-enable-path/README.md` §3.3 and §8.3 item 2).

"Proven" vs "inferred" for the whole table: every *offset/value pair* above is
proven by the instruction stream. What is **inferred** is (i) the mapping from
`{PS,PWGATE}` window objects to `reg[1]`/`reg[2]` of `/arm-io/ane` (derived from
the creation order and the name strings, not from a runtime value), and (ii)
that steps 1–8 are reached in this order on a *pre-loaded* device — they are
gated by `EnableANEClocksAndPower`'s refcount and by `this[0x3a20]`, both of
which the previous round resolved for family `0xe0`.

---

## 6. Q4 — what says the CPU ran

### 6.1 `SCRATCH7 == 0x08042006`

```
0x8f3e180  d9 00 84 52   mov  w25,#0x2006
0x8f3e184  99 00 a1 72   movk w25,#0x804,lsl#16      ; w25 = 0x08042006
0x8f3e188  80 9b 40 f9   ldr  x0,[x28,#0x130]
0x8f3e18c  61 1a 41 b9   ldr  w1,[x19,#0x118]         ; 0x188003c = SCRATCH7
0x8f3e190  93 d7 00 94   bl   0x8f73fdc              ; read32
0x8f3e194  1f 00 19 6b   cmp  w0,w25
0x8f3e198  40 06 00 54   b.eq 0x8f3e260              ; success
…
0x8f3e1c8  20 00 80 52   mov  w0,#1
0x8f3e1cc  b5 87 b1 94   bl   0xbba00a0              ; sleep 1 (thunk: w1 = 0xF4240)
0x8f3e1dc  7f a3 0f 71   cmp  w27,#0x3e8              ; <= 1000 iterations
```

The timeout log is `0xfffffe00073d73f4`:
*"%s: %s: No wake signal from first rANE_SCRATCH7(0x%x) ANECPU"*, and the
SCRATCH7=1 write is logged as `0xfffffe00073d72de`
*"%s: %s: Writing 1 to rANE_SCRATCH7 to effect a WARM Boot"*. `ANE_Init`'s own
timing log calls the whole function *"ANEHWDevice::FW powerOn"*
(`0xfffffe00073d8132`).

So the sequence is a **warm boot request** (`SCRATCH7 := 1`), a release
(`SCRATCH7 := 0`, `CPU_CONTROL := 0`, `:= 0x10`), and a ~1 s wait for the
firmware to publish `0x08042006`.

### 6.2 Is `0x08042006` really the firmware's token? — yes, from the image

The retained firmware contains the constant at three sites, each built as
`movz wN,#0x2006` + `movk wN,#0x0804,lsl#16`
(`python3 scripts/fw.py macho`; `__TEXT` file offset = `0x4000 + VA`):

| firmware VA | file off | bytes | context |
|---|---|---|---|
| `0x8754` | `0xc754` | `c2 00 84 52 82 00 a1 72` | `w2 = 0x08042006`, then a call through an object at `__DATA:0x1c84e8` with `x1 = 0` and `w2` as the value, followed by `dsb ish` (`0x877c`) |
| `0x17644` | `0x1b644` | `c8 00 84 52 88 00 a1 72` | `w8 = 0x08042006`, then `0x1764c: cmp x0,x8` / `0x17650: b.eq` — the firmware **verifies** the same value it published, with `w1 = 5` on the preceding call |
| `0x242c8` | `0x282c8` | `c9 00 84 52 89 00 a1 72` | same shape |

So `0x08042006` is a handshake token the firmware stamps into a scratch
register early in boot and re-checks later; the host watches SCRATCH7 for it.
That is the whole of the "the CPU ran" contract visible offline.

### 6.3 No `CPU_STATUS` readiness poll exists

Proven by §3.2: the only `CPU_STATUS` reader besides the debug dump is
`ANE_deInit`, which runs at *shutdown* and waits for the opposite condition
(bit0/bit1 set). Nothing in `ANE_Init`, `ANE_PowerOn*`,
`ValidatePowerOnState` (`0x8f29bfc`), `setPowerStateGatedPriv` or
`power_on_hardware` polls `0x1600048`. In `ANE_Init` the only MMIO after
`CPU_CONTROL` is the SCRATCH5 (never taken) and the SCRATCH7 poll.

Corollary for the live captures: **`0x68` is not a readiness signal, and the
absence of further register traffic after the `RUN` write is not independent
evidence either.** In this path `ANE_Init` touches nothing but `SCRATCH7` after
`CPU_CONTROL`, so "no mailbox traffic" is exactly what a never-satisfied
`SCRATCH7` wait looks like — which is why the token is the signal to read, not
the mailbox.

---

## 7. Which blocker, and the two measurements that decide it

The question was (a) clock genuinely gated, or (b) firmware never loaded / CPU
never released.

**(b) as stated is not what differs.** The ADT says iBoot placed the image
(`pre-loaded = 1`, `segment-ranges` with physical addresses and DVAs), and the
driver *provably* skips both the copy and the RVBAR write on this device. So on
macOS the firmware is not loaded by the kext either. A Linux port that "fails to
load firmware" is in exactly the same position as macOS; the load is not the
missing step.

**(a) cannot be excluded from `CPU_STATUS`,** because that register lives in the
always-on ASC wrapper and its state change only shows that the RUN bit reached
the wrapper. But the state change does show the control path is alive, and it is
the *same* change M6 shows before it then boots firmware.

What Linux is demonstrably missing, in macOS's own order:

1. the two provider votes (clock + power) and the PS `0xc000`/`0xc008` /
   PWGATE `0x268c` writes (steps 1–8). The clock vote is a **PMP command**
   (prior receipt) — this is the piece that cannot be done by raw MMIO alone.
2. the SCRATCH sequence (steps 9–12): `SCRATCH0..7 := 0`, `SCRATCH6 := 1`,
   `SCRATCH7 := 1` (warm boot), `SCRATCH7 := 0`.
3. then `CPU_CONTROL := 0`, `:= 0x10` — which Linux already does, but *without*
   steps 1–12, i.e. out of order and without the warm-boot request.

Two non-destructive measurements settle (a) vs (b) on the live part, in this
order:

* **Read `SCRATCH7` (`0x4_0188_003c`) before writing anything.** If it already
  reads `0x08042006`, the driver's poll would pass on a stale value and the
  "ready" signal is worthless until the SCRATCH clear is verified (this is the
  failure mode the H15 module guards against: the read-back after
  `SCRATCH7 := 0` must read 0). If it reads 0, the firmware has not run.
* **Read `RVBAR` (`0x4_0105_0000`)** and compare with `0x0000_0100_0000_0001`
  (§4.2). A different value means the mapping/entry the CPU fetches is not the
  one the ADT describes, which would be a (b)-class fault even though the image
  is in memory.
* Then run the full macOS order from §5.4 (with the SCRATCH clear verified) and
  watch `SCRATCH7` for `0x08042006` for ~1 s. A token appearing proves the core
  executes firmware; `SCRATCH7` staying 0 while `CPU_STATUS` bit1 stays clear
  means "released but not fetching" and points at the clock/PMP vote, since the
  memory path is already established by iBoot.

---

## 8. Not derivable offline

1. Whether the ANE CPU clock is on. Nothing in the kernelcache reports it; the
   PMP/APSC side is outside the artifact.
2. The live `RVBAR` and `SCRATCH7` values (only hardware can read them).
3. The meaning of bits 63:48 (`0x0081`) in the driver-composed RVBAR, and of
   bits 2–6 of `CPU_STATUS` (bits 3 and 6 are set on J700 and are not named by
   any driver code).
4. The identity of `IOMapper` code slots `0x890` / `0x8a0` (`com.apple.kernel`
   symbols are not in the retained kernelcache).
5. `this[0xdb]`/`[0xdc]` at *runtime*: derived here from the ADT properties the
   code reads (`pre-loaded`, `ctrr-unlocked`), not observed.
6. Whether `InitializeRTBuddyClient` fails on J700 for the reason I give (no
   `iop-ane-nub`): the ADT is the boot loader's node set, and the function's
   lookup path is only partly decoded; the *conclusion* (direct path) also
   depends on no other provider inventing that nub.
7. Which `_tunableh` register set the HAL applies (unchanged from the previous
   round; not needed for this question).
8. Which engine offset carries the firmware command ring (the A2I/I2A pair) on
   family `0xe0`. `§2.1` shows it is not `0x1608000`; the family-`0xe0` block of
   `initializeANESoCConfig` installs no `0x1840xxx` offsets either, so the ring
   registers must be reached through one of the fields listed in §1 — three of
   those (`0x1370000`/`0x1370004`, `0x188c000`, `0x1890000`) are unresolved
   here. Resolving that needs the `aneFirmwareCommandSend` path
   (`0x8f47d78`), which is out of scope for this question.

---

## 9. Files, and how to reproduce

```
receipts/2026-11-04-j700-h17-firmware-load-path/
├── README.md      this report
└── scripts/
    ├── ane.py     VA↔file mapping, symbol-annotated capstone disassembly
    ├── scan.py    linear-sweep search: `disp|imm|adrp|str` in a kext __text
    ├── regnames.py adrp+add constant-pool string dump for a VA window
    └── fw.py      re-derive reg[]/ranges/segment-ranges from the ADT, the RVBAR
                   arithmetic, and the firmware Mach-O header + 0x08042006 sites
```

```
cd receipts/2026-11-04-j700-h17-firmware-load-path/scripts
python3 fw.py all                     # ADT windows, physical map, segments, Mach-O
python3 fw.py rvbar 0x10000000000     # the RVBAR arithmetic
python3 scan.py disp 0x158            # .. 0x8f4172c is the doorbell (0x1884000)
python3 scan.py disp 0x15c            # CPU_STATUS field: only 0x8f2dd64 + 0x8f42ff4
python3 scan.py disp 0x160            # CPU_CONTROL: only 0x8f3e14c + 0x8f3e15c are the field
python3 scan.py str 0x1050000         # RVBAR: 1 read + 1 write + the debug dump
python3 scan.py str 0x1600            # (empty: nothing builds 0x1600044 literally)
python3 ane.py 0xfffffe0008f3dd70 120 # ANE_Init direct path, SCRATCH..RVBAR..RUN
python3 ane.py 0xfffffe0008f3dffc 100 # the RVBAR skip + compose + write
python3 ane.py 0xfffffe0008f3e180 40  # the SCRATCH7 poll
python3 ane.py 0xfffffe0008f42fe0 40  # the only CPU_STATUS poll (deInit)
python3 ane.py 0xfffffe0008f64f90 70  # family 0xe0 offset table
python3 ane.py 0xfffffe0008f537e0 60  # `pre-loaded` / `ctrr-unlocked`
python3 ane.py 0xfffffe0008f43dec 120 # mapFwCTRRRegion
python3 ane.py 0xfffffe0008f55fa0 30  # the FWIM surface allocation
```

Nothing here is device qualification. A register map is not device behaviour,
and a decoded instruction is not a running CPU.
