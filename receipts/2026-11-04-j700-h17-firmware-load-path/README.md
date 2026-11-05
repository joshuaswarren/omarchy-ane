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
├── analysis/      raw sweep/trace outputs backing §§3, 6, 10, 11, 12, 13
└── scripts/
    ├── ane.py     VA↔file mapping, symbol-annotated capstone disassembly
    ├── kimage.py  whole-kernelcache index: 366 filesets, global symtab, ptr/bl/adrp/find scans
    ├── disva.py   disassemble N instructions from a VA (`va <va> <n>`)
    ├── scan.py    linear-sweep search: `disp|imm|adrp|str` in a kext __text
    ├── cases.py   simulate the initializeANESoCConfig dispatch tree → §11 table
    ├── regnames.py adrp+add constant-pool string dump for a VA window
    └── fw.py      re-derive reg[]/ranges/segment-ranges from the ADT, the RVBAR
                   arithmetic, and the firmware Mach-O header + 0x08042006 sites
```

```
cd receipts/2026-11-04-j700-h17-firmware-load-path/scripts
python3 fw.py all                     # ADT windows, physical map, segments, Mach-O
python3 fw.py rvbar 0x10000000000     # the RVBAR arithmetic
python3 cases.py                      # §11 cross-family table from the full dump
python3 kimage.py list ane            # §12 filesets + global symbolize (kc-index.json cache)
python3 kimage.py bl 0xfffffe0008f45fb8   # the two FWSharedEventDoorbellRing ring sites
python3 kimage.py find doorBellRing   # §12.1: only __FUNCTION__ + LINKEDIT strings
python3 kimage.py sym 0xfffffe0008ef85a8  # §13.2 _IOProcessorChannelSend; bl-scan it for the send topology
python3 disva.py va 0xfffffe0008f58060 40 # §13.3 IRQ status read + W1C ACK
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

## 10. Message/command register protocol (offline, 2026-11-04 evening)

Sweeps over the whole interface range (`0xfffffe0008eeb820`–`0xfffffe0008ff8dd8`)
for loads/stores of the family-`0xe0` SoC-config fields, then a use-site
disassembly of every hit. Raw outputs retained in `analysis/` (field-consumer
sweep, SoC-config dispatcher, doorbell/IRQ/command sites, full
`SendRequestToFirmware_gated` and `aneFirmwareCommandSend` traces).

### 10.0 Method correction — PAC virtual calls are not field reads

The dominant hit pattern is

```
ldr  x16, [x0]            ; signed vtable pointer (autda/xpacd guard follows)
add  x8,  x16, #off
ldr  x9,  [x16, #off]     ; method at vtable slot off
blraa x9, x17             ; virtual call
```

so `ldr x9,[x16,#0x148/0x150/...]` hits are **vtable-slot calls**, not device
field reads. True field/register consumers are only the sites of the form
`ldr w1,[this,#off]` followed by a register-helper call (§3 helper cluster).

### 10.1 The mailbox trio, pinned to named consumers

| Reg offset | Field | Consumer (VA) | Use |
|---|---|---|---|
| `0x1884000` | `this+0x158` | `ANEHWDevice::doorBellRing` (`0xfffffe0008f416b4`) | host→firmware **raise**: write `1 << bit`, `bit < 32` enforced (`cmp w8,#0x20`) |
| `0x188c000` | `this+0x150` | `aneInterruptHandlerPriv` (`0xfffffe0008f5810c`), `handleOutstandingCommand`, `waitForFirmwareToQuiesce`, `HandleFirmwareTimeout_gated`, ISP surface-map paths | firmware→host IRQ **status**, read via helper `0x8f73fdc(base, reg)` |
| `0x1890000` | `this+0x154` | `aneInterruptHandlerPriv` (`0xfffffe0008f581c4`) | IRQ **ACK**: write `0xffff` via helper `0x8f74130(base, reg, val)`, then `dmb ish` — 16 W1C lines |
| `0x1370000/4` | `this+0x148..14c` | `ReadFirmwareTimestamp` (`0x8f43d78`), `GetANETimeToSystemTimeSync` | free-running firmware **timestamp** counter pair |

The MMIO handle passed to both helpers is device field `[this,#0x320]`
(`ldr x0,[x26,#0x320]` at every IRQ call site). `doorBellRing` reaches the
same handle through `[arg+0xe320]`, i.e. its argument object and the interrupt
handler's device sit `0xe000` apart in the same allocation — how exactly is
not resolved offline and does not change any register value above.

`doorBellRing` is referenced once as data: a u64 equal to its VA at
`0xfffffe000c7c4378`. That table (`…c7c42c8`…, entries sorted by mangled name)
is the `OSMetaClass` **method list** of `ANEHWDevice`: `ANE_PowerOn`,
`ProgramLoad`, `isANEActive`, `ANE_PowerOff`, `IsAneTestApp`, `IsRecoveryOS`,
`ProcessAbort`, `ProcessReMap`, `ProgramAbort`, **`doorBellRingEPv`**,
`processCount`, `updateReport`, `ANE_GetStatus`, `ANE_SaveState`,
`ProgramUnload`. No direct `bl` to it exists anywhere in the kernelcache and
no chained or raw slot in any vtable points at it (a full-image scan for a
chained-rebase slot with target `0x01f316b4` returns zero hits) — the method is
**exported through the meta-class table**, and as §12.1 shows it has **no
caller anywhere in this kernelcache**: a compatibility export, not H17's
request path.

Its argument convention, from its own body (`0x8f416b4`): `x0 = arg`,
`x9 = [arg]` (the `ANEHWDevice*`), `w8 = [arg,#8]` (the bit, `cmp w8,#0x20`
rejects ≥ 32), MMIO handle `[arg+0xe000+0x320]`, register offset
`[x9,#0x158]`, tail-`b` to the `write32` helper with `w2 = 1 << w8`. The
`[this+0x320]` the IRQ helper reads is the **same field via the shifted
pointer**: the IRQ handler receives `device+0xe000`, `doorBellRing` receives
the device and adds `0xe000` itself. The earlier note that the call site at
`0x8f107dc` in `SendRequestToFirmware_gated` was the doorbell rung is
**withdrawn**: that site is a vtable slot `+0x158` dispatch whose target
(`0xbbb9cf4`, outside the ANE iface range) is a base-class method; the
`+0x158` coincidence is arithmetic, not causal.

### 10.2 Command-send call chain (`ANEHWDevice`)

| Symbol | VA |
|---|---|
| `aneCmdSend(ANEFirmwareCommand&)` | `0xfffffe0008f49064` |
| raw `aneCmdSend(void*,u32,void*,u32,u32,bool,u64,u32,bool,bool,bool,u64)` | `0xfffffe0008f3d300` |
| `aneFirmwareCommandSend(ANEFirmwareCommand&, function<void(void*,int)>)` | `0xfffffe0008f47d78` |
| `aneCmdSendAsync(ANEFirmwareCommand&, function<void(void*,int)>)` | `0xfffffe0008f48f58` |
| `cancelFirmwareCommand(void*)` | `0xfffffe0008f436c8` |
| `commandSleep(void*, u32)` | `0xfffffe0008f59b1c` |

`aneCmdSend(ANEFirmwareCommand&)` builds a completion thunk and tail-calls
`aneFirmwareCommandSend`; that function is queue/state-machine logic (OSAction
helpers, error constants `0xe00002bd` and `0xe208`) with **no direct MMIO access
in its first 400 instructions**. A whole-kernelcache scan (§12) then found
**no caller of `doorBellRing` anywhere in the image** — no `bl`, no raw data
pointer, no `callPlatformFunction("doorBellRing")` site — and the request path
does **not** ring `0x1884000` at all: `SendRequestToFirmware_gated` and
`ANE_ProgramPrepareAndSubmitRequest_gated` ring an **IOSurfaceSharedEvent
shmem word** through `FWSharedEventDoorbellRing` (§12.2). The observable H17
submit handshake is therefore: build request (with its shared-event array) →
queue → ring the shmem doorbell → firmware answers via an IRQ line, status
read at `0x188c000`, W1C-ACK written to `0x1890000`.

This is a decoded instruction stream, not device behaviour: nothing in this
section has been exercised on hardware, and task submission stays off-limits
until the ring/bit semantics are pinned beyond what a static trace shows.

## 11. The cross-family SoC-config table (offline)

`ANEHWDevice::initializeANESoCConfig` (`0xfffffe0008f64ae0`) is a dispatcher on
the platform ID at `this+0x1cc`: a binary tree of `cmp w8,#K` nodes rooted at
`0x8f64e3c` (`cmp`s at `0x8f64e40`…`0x8f657a4`), 21 leaves. Simulating the tree
for every candidate key (`scripts/cases.py`, output
`analysis/h17-socconfig-cases.txt`) gives **21 keys, one distinct case each,
no collisions**. Each case stores: family id → `[x21]` (out-param; `key` where
the body stores the dispatch value directly), name string → `[x21,#0x10]`,
variant id → `this+0x1c8`, a 16-byte q-block → `this+0x148..0x154`
(`stur q0,[x23,#0x44]`, `x23 = this+0x104`), an 8-byte d-pair →
`this+0x158/0x15c` (`str d0,[x19,#0x158]`), and the CPU-control register
offset → `this+0x160`.

### 11.1 All 21 cases

| key (`this+0x1cc`) | name | fam (`[x21]`) | var (`this+0x1c8`) | q-block (`+0x148..0x154`) | d-pair (`+0x158/0x15c`) | ctl (`+0x160`) |
|---|---|---|---|---|---|---|
| 0x1 | h11 | 0x11 | 1 | runtime | runtime | `0x1400044` |
| 0x4 | h11 | 0x12 | 1 | runtime | runtime | `0x1400044` |
| 0x8 | h12 | 0x30 | 3 | `0x1170000/4`, `0x184c000`, `0x1850000` | `0x1844000` / `0x1400048` | `0x1400044` |
| **0x20** | **h13** | **0x40** | **4** | `0x1170000/4`, `0x184c000`, `0x1850000` | `0x1844000` / `0x1400048` | `0x1400044` |
| 0x40 | h13g | key (0x40) | 4 | same as 0x20 | same | `0x1400044` |
| 0x60 | h13g | key (0x60) | 4 | runtime | runtime | runtime |
| 0x70 | h14 | key (0x70) | 5 | same as 0x20 | same | `0x1400044` |
| 0x80 | h14g | 0x70 | 5 | same as 0x20 | same | `0x1400044` |
| 0x90 | h15 | key (0x90) | 6 | same as 0x20 | same | `0x1400044` |
| 0xa0 | h14g | 0x80 | 5 | runtime | runtime | `0x1400044` |
| 0xb0 | h15g | 0x90 | 6 | same as 0x20 | same | `0x1400044` |
| 0xc0 | h15g | 0x90 | 6 | same as 0x20 | same | `0x1400044` |
| 0xd0 | h16 | 0xc0 | 7 | same as 0x20 | same | `0x1400044` |
| 0xe0 | h15g | 0xb0 | 6 | same as 0x20 | same | `0x1400044` |
| 0xf0 | m11 | 0xa0 | 8 | same as 0x20 | same | `0x1400044` |
| 0x100 | h16g | 0xc0 | 7 | `0x1370000/4`, `0x188c000`, `0x1890000` | `0x1884000` / `0x1600048` | `0x1600044` |
| 0x110 | h16g | 0xd0 | 7 | same as 0x200 | same | `0x1600044` |
| **0x200** | **h17** | **0xe0** | **9** | `0x1370000/4`, `0x188c000`, `0x1890000` | `0x1884000` / `0x1600048` | `0x1600044` |
| 0x210 | h17 | 0xe0 | 9 | `0x1840068/6c`, `0x1908000`, `0x1840080` | `0x1840078` / `0x184007c` | `0x1600044` |
| 0x220 | h17 | 0x100 | 9 | runtime | runtime | `0x1600044` |
| 0x300 | m9 | 0xf0 | 0xa | same as 0x20 | same | `0x1600044` |

Name strings are `__TEXT,__cstring` at `0xfffffe00073b4fab` (`h13`),
`…fbd` (`h13g`), `…fc2` (`h14`), `…fc6` (`h14g`), `…fcb` (`h15`), `…f95`
(`h11`, keys 0x1/0x4), `0xfffffe00073b5038` (`h17`, all three cases). Pool
sources: `0x200` (H13-class q-block), `0x260` (H17-class q-block), `0x320`/
`0x468` (the 0x210 case), `0x450`/`0x480` (d-pairs).

### 11.2 The mailbox-page cluster (`this+0xf4..0x138`)

The same cases fill a second register-offset cluster on the mailbox pages,
adjacent to the §10.1 trio:

| field | h13 (key 0x20) | h17 (key 0x200) |
|---|---|---|
| `+0xf4..0x104` | `0xc030`, `0x1840048/4c/50` (pool+0x420) | `1`, `0xc010`, `0x1880020/24` (pool+0x3b0, `stur q1,[x19,#0xf4]`) |
| `+0x104..0x114` | `0x1840054/58/5c/60` (pool+0x430, `stur q0,[x23,#4]`) | `0x1880028/2c/30/34` (pool+0x2d0) then `0x1880038, 0x188003c, 0x196c000, 0x1880060` (pool+0x2f0, `stp q0,q1,[x23]`) |
| `+0x118` (SCRATCH7, §6.1) | `0x1840084 − 0x20 = 0x1840064` | `0x188003c` |
| `+0x124` | — | `0x1880064` |
| `+0x128/0x12c` | — | `0x1880058`, `0x188005c` (pool+0x4a8) |
| `+0x138` | — | `0x1c201060` |

### 11.3 The three handshake fields vs. what this host did

| field | h13 | h17 | kext consumer | this host (§3, §6; three boots) |
|---|---|---|---|---|
| `this+0x118` SCRATCH7 | `0x1840064` | `0x188003c` | `ANE_Init` `+0x564` reads it and compares the literal `0x08042006` (`mov w25,#0x2006; movk w25,#0x804,lsl#16` at `0x8f3e180`); also `DumpANERegisters+0x17c` | the wake poll that returned `0x08042006` |
| `this+0x160` CPU_CONTROL | `0x1400044` | `0x1600044` | `ANE_Init` `write32(…,0)` then `write32(…,0x10)` (`0x8f3e14c`…`0x8f3e164`) | the register our release sequence writes |
| `this+0x15c` CPU_STATUS | `0x1400048` | `0x1600048` | `ANE_deInit` poll (§3.3), `DumpANERegisters+0x1bc` | the register polled for the wake word |

Two independent sources — Apple's kext constant pool and this host's observed
wake behaviour — agree on CPU_CONTROL `0x1600044`, CPU_STATUS `0x1600048`, and
token `0x08042006` for H17: the kext's case for this platform is key `0x200`
(family `0xe0`, variant 9). Note the kext's wake-word *read* targets SCRATCH7
(`0x188003c`), never CPU_STATUS — CPU_STATUS is polled only in `ANE_deInit`
(bits 0/1, 20 × 10 ms), matching the module's own poll order (§6.1).

The `+0x160` scalar at `0x8f64fcc` is built `mov w8,#0x44; movk w8,#0x140,lsl#16;
orr w8,w8,#0x200000` → **`0x1600044`** — the H17 case sets bit 21, which moves
the CPU-control window from the H13 page `0x1400000` to `0x1600000`; the d-pair
lo half is the §10.1 doorbell and the hi half is CPU_STATUS.

### 11.4 Runtime-source cases

Keys `0x1, 0x4, 0x60, 0xa0, 0x220` do not load the q-block/d-pair from the
constant pool: they make a PAC-signed virtual call (`autda` + `blraa`, PAC
modifier `0xcda1<<48` / `0x37a5<<48`) through `[this+0x2b0]`'s vtable slot
`+0x2d8` with the C-string `"ane-subtype"` (`0xfffffe00073b5073`) and take the
values from the returned object. Offline, the existence of that lookup is
pinned, not its results.

Raw evidence kept: `analysis/h17-socconfig-cases.txt` (generated by
`scripts/cases.py` from the full function dump),
`analysis/h17-sweep-0x118.txt`, `analysis/h17-sweep-0x15c.txt`,
`analysis/h17-sweep-0x160.txt` (displacement sweeps of the full
`__TEXT_EXEC`, `scripts/scan.py disp <off>`). As elsewhere in this receipt:
decoded instruction stream, not device behaviour.

## 12. The doorbell architecture, resolved image-wide (offline, 2026-11-04 night)

`scripts/kimage.py` parses the retained kernelcache as a whole: MH_FILESET with
**366 kext filesets**, each with its own `LC_SYMTAB`, and the file is one linear
mapping (`VA = 0xfffffe0007004000 + file_offset` holds across every top-level
segment, so `ane.py`'s mapping is image-wide). Modes: `list [sub]` (fileset
extents), `sym <va>` (global symbolize), `ptr/bl/adrp/find` (whole-image scans).
The ANE family in this cache is exactly two kexts —
`com.apple.driver.AppleH16ANEInterface` and `com.apple.driver.AppleT8140ANEHAL`
— there is no separate manager/`ANECore` kext to find.

### 12.1 `doorBellRing(void*)` has no caller in the whole kernelcache

Whole-`__TEXT_EXEC` `bl` scan: **0 hits**. Raw-u64 scan for the function's VA:
**exactly one slot**, the `OSMetaClass` method list (`0xc7c4378`). `adrp+add`
scan for a bare `"doorBellRing"` selector string: **0 sites** — the string only
exists as `__FUNCTION__` and mangled-name text. So the §10.2 note that a
consumer-side kext rings `0x1884000` is **superseded**: the MMIO channel
doorbell is a *compatibility export*, and on H17 the request path rings
something else.

### 12.2 What a request actually rings

`ANEHWDevice::FWSharedEventDoorbellRing(ANERequest*)` (`0x8f45fb8`) — called
from `SendRequestToFirmware_gated+0x2100` and
`ANE_ProgramPrepareAndSubmitRequest_gated+0x3e68` (only two `bl` sites
image-wide):

- gated on bit 10 (`tbz #0xa`) of the static u32 at `0xfffffe000c3f8d48`;
- `request+0x18fc` = u32 shared-event **count**; `request+0x190c` = event
  **array**, stride `0x30`, each entry's `+0x14` an `IOSurfaceSharedEvent*`
  (`getShmemIndex()` is called on it);
- logs each event: *"FW shared event doorbell uuid: 0x%llx cachehandle: 0x%llx,
  transactionId: 0x%llx"* and *"wait event idx: %lld type: %d value: %lld"*;
- the ring itself is the **common tail** (also taken when the count is zero,
  once past the gates): `w9 = 0x40; str w9, [[dev+0x3b20]]` — a **raw store of
  bit 6 (0x40) into the firmware-visible shmem word**, not an MMIO write32;
- `[dev+0x3b20]` is initialised in `start+0x39c4`:
  `[dev+0x3b18] = vtable([dev+0x3ab8])[+0x138](…)` (the shared-event shmem
  base) `+ ((count−1) & mask)`.

`ANEDriverEventSource::signalDoorbell()` (`0x8ef09ec`) is unrelated to the
firmware: it tail-`b`s `IOEventSource::signalWorkAvailable` when its own
`+0x28` bit 0 is set — a work-loop wakeup.

### 12.3 The MMIO doorbell is RTKit-style *named channels*

`ANE_Init` calls `channelNameToIndex(char*, uint*)` (`0x8f41748`) eight times;
each resolves a firmware-published channel **name** to a **bit index** against
the table at `[[dev+0x3de0]]` (count `u32 [dev+0xe1f0]`, stride `0x100`,
`strnlen(name,0x20)`+`bcmp`) and caches the index:

| dev field | channel name |
|---|---|
| `+0xe1f8` | `SHAREDMALLOC` |
| `+0xe1fc` | `TERMINAL` |
| `+0xe200` | `BUF_H2T` |
| `+0xe204` | `BUF_T2H` |
| `+0xe208` | `IO` |
| `+0xe20c` | `IO_T2H` |
| `+0xe210` | `DEBUG` |
| `+0xe214` | `DATA_CHAIN_H2T` |

`doorBellRing(void*)` (`0x8f416b4`) is the generic ringer for those bits:
`x0 = {ANEHWDevice*, u32 bit}`, rejects `bit ≥ 32`, tail-`write32(
[dev+0xe320], [dev+0x158] = 0x1884000, 1<<bit)`. The IRQ side matches the
pair-direction naming (`*_H2T`/`*_T2H`): `aneInterruptHandlerPriv` reads the
status word at `0x188c000`, writes `0xffff` (16 W1C lines) to `0x1890000`, then
switches on the u32 `[dev+0xe278]` (`cmp #3`) — the 4-way state selector — and
logs *"no clocks. returning"* / *"polling all channels"* around it.

### 12.4 `ANERequest` field offsets as observed

From `SendRequestToFirmware_gated` (`x0 = this`, `x1 = request`; retained
function dump): `+0x18` u64, `+0x20` u32, `+0x24` u8 (flag set/cleared), `+0x25`
u8, `+0x28` u64 (object, its `vtable+0x158` invoked — the §10.2 site), `+0x30`
u64 (cleared), `+0x38` u64, `+0x40`/`+0x44` u32, `+0x1890` u64, `+0x1898` u32,
`+0x18a0` u64, `+0x18e0` u64 (client key, passed to `findClient`), `+0x18f0`
u64, `+0x1900` u32, `+0x18fc` u32 event count, `+0x190c` event array (stride
`0x30`), `+0x3120` u64, `+0x3128` u32, `+0x3130` u64 (queue link, cleared on
completion).

What is still not pinned: the *contents* of the firmware-published channel
table at `[[dev+0x3de0]]` (runtime data — it comes from the firmware's channel
registration, and the 8 names above are the driver's *expected* names), the
meaning of the `[dev+0x3b20]` mask arithmetic (`(count−1) & mask` selects which
channel's word), and the shared-event entry fields beyond `+0x14`. None of
this changes §12.2's conclusion that a command rings a **shmem word, bit 6**,
while `0x1884000` is the per-channel doorbell with driver-resolved bit indices.

Raw evidence: `analysis/h17-ring-fns.txt` (full disassembly of `doorBellRing`,
`channelNameToIndex`, `FWSharedEventDoorbellRing`, `signalDoorbell`),
`analysis/h17-doorbell-arch.txt` (scan outputs, strings, offsets). Tooling:
`scripts/kimage.py`. Again: decoded instruction stream, not device behaviour —
the command rung stays closed until a fresh boot, and nothing here was
exercised on hardware.

## 13. The messenger protocol and the full request path (offline, 2026-11-04 night, part 2)

This section closes the two gaps §12 left open: *how a request or command is
actually delivered to firmware*, and *how the completion comes back*. All of it
is static decode of the retained kernelcache.

### 13.1 Userspace submission ABI: 17 selectors

`H11ANEInUserClient::externalMethod` (`0xfffffe0008fc7370`) is a log-and-forward
shim: it builds a copy of the `IOExternalMethodArguments` on its stack and
tail-calls `ANEClientInterface::externalMethod` (`0xfffffe0008fc62b8`), passing
`this` (the ANEClientInterface), the copied args, and the `ANESharedEvents*`
argument. The real dispatch resolves the client device
(`ANEClientInterface vptr+0xa8`), caches it at `[iface+0xd8]`, and invokes the
**runtime** driver singleton's `vptr+0x278` with the copied-args block and the
selector (`0xfffffe0008fc64a4`; the singleton global `0xc3f4aa0` holds a
runtime-allocated pointer — static value `0x100`, so its vtable identity is not
statically resolvable). A second `vptr+0x278` call (`0xfffffe0008fc64ac`,
`x0 = client device`, `x1 = selector`) is the per-selector log/name lookup.

The selector table passed to the kernel's method dispatcher lives at
`0xfffffe0007f5d428`, 17 rows of `IOExternalMethodDispatch` with chained-auth
fixup slots; decoded targets:

| sel | function |
|---|---|
| 0 | `ANE_DeviceOpen` |
| 1 | `ANE_DeviceClose` |
| 2 | **`ANE_ProgramSendRequest`** |
| 3 | `ANE_ProgramCreate` |
| 4 | `ANE_ProgramPrepare` |
| 5 | `ANE_ProgramUnprepare` |
| 6 | `ANE_ProgramDestroy` |
| 7 | `ANE_GetStatus` |
| 8 | `ANE_ProgramCreateInstance` |
| 9 | `ANE_ProgramChainingPrepare` |
| 10–16 | further static `ANE_*` free-function wrappers (same `{ANEClientDevice*, IOExternalMethodArguments*}` tail-call idiom) |

The selector-2 wrapper (`0xfffffe0008fc3a44`) reads the device out of
`[iface+0x10]`, authenticates its vtable (`autda`, PAC modifier `0xcda1<<48`),
loads slot `+0x930`, and tail-`braa`s it with the args block — i.e. the public
entry is a virtual method on the ANEClientInterface. `ANEClientInterface`'s
static vtable decodes slot +0x930 to `clientClose` because **the dispatch
targets the runtime-allocated object's vtable**, not the kext's static one;
same caveat as the `+0x278` slot above. `ANEClientDevice::getClient`
(`0xfffffe0008fc27d0`) is `return this + 0x18` (the embedded client).

### 13.2 Delivery: an RTKit message queue per channel, not MMIO

The command/request primitive is `_IOProcessorChannelSend(cb, id, a2, a3)`
(`0xfffffe0008ef85a8`, called via `IOProcessorChannelSendRetry(u8 channel,
const void *buf, u64, u64, bool)` at `0xfffffe0008f43a8c`; a logging wrapper
`IOProcessorChannelSendWithLog` exists at `0xfffffe0008f4948c`).

Control-block layout (0x30 bytes) — **re-audited 2026-11-04 against the
disassembly** (`analysis/h17-messenger-irq-chain.txt`); the first version of
this table swapped the two indices, called the payload words u32, and
mis-claimed `+0x00/+0x08` as hook pointers:

| off | field | evidence |
|---|---|---|
| `+0x00`/`+0x08` | role unresolved — **not dereferenced by either primitive** | no load/store/`blr` against them in send or receive |
| `+0x10` | s32 phase word | send stores `slot[0] = x1 | (sxtw(+0x10) ^ 1)` (`0x8ef8620..62c`); receive compares `slot[0]&1` vs `+0x10&1` (`0x8ef89f0..a04`) |
| `+0x14` | u32 entries (wrap at `entries−1`) | `csinc` wraps at `0x8ef8664..670` and `0x8ef8a50..660` |
| `+0x18` | u64 message buffer base | `ldr x8,[cb+0x18]`; slot = base + index·64 |
| `+0x20` | u32 **read (consumer)** index | receive's slot address (`0x8ef89b4`); send initialises it to the producer value when `−1` (`0x8ef8650..658`) |
| `+0x24` | u32 **write (producer)** index; `−1` → send returns `−1` | send's slot address (`0x8ef85ec..5f4`, `b.eq` → `mov w20,#-1` at `0x8ef8690`); post-advance store `0x8ef867c` |
| `+0x28` | u32 dequeue counter | receive increments it (`0x8ef8a68..670`) |
| `+0x2c` | u32 enqueue counter | send increments it (`0x8ef8680..688`) |

`send(cb, x1 = message id/flags, x2, x3 = two u64 payload words)`: reject
if the producer index reads `−1`; `slot = base + (producer)*64` — **64-byte
message slots**; `dsb st`; store `x2` at `slot+0x08` and `x3` at `slot+0x10`
— **both full 64-bit `str`** (the first version of this section said u32);
`slot[0] = x1 | (phase^1)` (RTKit phase/written bit in bit 0, id in bits 2+,
which is what receive re-extracts as `slot[0] & ~4`); advance the producer
with wrap-at-`entries−1`, increment the enqueue counter; return `0`, or
`−1`.

The post-advance producer store is **not** a plain index
(`csinv`/`csinc` at `0xfffffe0008ef866c..67c`): when consumer ≠ new-producer
it stores `~prod'` (bitwise-inverted); when consumer == new-producer it
stores `0`. The consumer store (`0xfffffe0008ef8a64`) is the plain
wrapped index. A later send therefore uses `+0x24` as a slot address only
after the other endpoint has acknowledged a plain value back into it — an
H→T handshake word whose exact full/empty arithmetic is not resolvable
from host-side code alone and stays **unresolved**.

`receive(cb, u64 *out_id, u64 *out_a, u64 *out_b)`
(`_IOProcessorChannelUnidirectionalReceive`, `0xfffffe0008ef896c`):
`slot = base + (consumer)*64`; **empty iff `slot[0]&1 != (cb+0x10)&1`**
(`dsb ishld` before the read); otherwise copy `slot[0]&~4` → `*out_id`,
`slot[+0x08]` → `*out_a`, `slot[+0x10]` → `*out_b` (three u64 loads),
re-store `slot[0] = sxtw(phase)^1` (slot release), advance the consumer
with wrap-at-`entries−1`, increment `+0x28`; return `0`/`−1`.

The `0xfffffe000c3f8d68/70/78` pointers are **global null-guarded
instrumentation hooks, not control-block fields**: `+d68` is called
`(slot, 0x40)` (PAC modifier `0xf33d<<48`) after every slot store in both
primitives — consistent with cache clean; `+d70` is called `(slot, 0x40)`
before the receive slot read — consistent with cache invalidate; `+d78`
(entry of both primitives) and `+d80` (exit, via `ldp x9, x0, [0xc3f8d80]`)
take the argument at `0xc3f8d88` with modifier `0x2abe<<48` — a trace/log
hook. Their identities are unconfirmed.

So H17 host→firmware work is delivered as **RTKit message-queue enqueues
into shared memory**, with the §12.2 shmem-word store (bit `0x40`) as the
doorbell that wakes the firmware.

Send topology (whole-image `bl` scans): `aneFirmwareCommandSend` →
`IOProcessorChannelSendRetry` (`0x8f486cc`, the IO command channel) and
→ `handleOutstandingCommand` (`0x8f48a70`, timeout requeue);
`processCommandResponse` → `IOProcessorChannelSendRetry` (`0x8f43990`, replies);
`processTargetToHostIOCommand` and `processSharedMallocRequest` →
`IOProcessorChannelSendWithLog` (T2H/MALLOC replies); the raw
`_IOProcessorChannelSend` has one caller, `ANE_processISPSurfaceMapRequest`.

### 13.3 Completion: one IRQ line, W1C, then per-channel dequeue

`aneInterruptHandlerPriv` (`0xfffffe0008f5807c`, `x26 = dev+0xe000`, `x20 =
dev+0xc3f4d48`):

1. gate: bit 1 of `[0xc3f4d49]` (runtime flag) must be set;
2. `read32([dev+0xe320], [dev+0x150] = 0x188c000)` — IRQ status word;
3. `write32([dev+0xe320], [dev+0x154] = 0x1890000, 0xffff)` then `dmb ish` —
   **W1C all 16 lines** (`0xffff`);
4. per-message loop, dispatch on the IRQ line number vs the §12.3 cached
   channel indices at `[dev+0xe1f8..0xe214]`:

| line equals | arm |
|---|---|
| `[dev+0xe200]` BUF_H2T or `[dev+0xe204]` BUF_T2H | host→ / target→host **buffer** channel: dequeue one message via `_IOProcessorChannelUnidirectionalReceive` (`0x8f582c8`) from the channel's control-block table (`[[dev+0xd48+0x278]]`) |
| `[dev+0xe208]` IO | `IOProcessorChannelReceiveWithLog` (`0x8f47cd4`, `0x8f582e4`) → `processTargetToHostIOCommand` (`0x8f03a70`) |
| `[dev+0xe1f8]` SHAREDMALLOC or `[dev+0xe210]` DEBUG | `IOMalloc(0x32)` + `processSharedMallocRequest` (`0x8f4e3xx`) |
| `[dev+0xe20c]` IO_T2H / `[dev+0xe214]` DATA_CHAIN_H2T | `processTargetToHostIOCommand`-family arms (`0x8f585ac..0x8f585b8`) |

5. completions reach clients through `handleOutstandingCommand(OSObject*,bool)`
   (`0xfffffe0008f43110`, callers: `ANE_deInit`, `cancelFirmwareCommand`'s
   `_block_invoke`, `processCommandResponse`, `aneFirmwareCommandSend`) and the
   timer path: `timeOutRequestTimerHandler_gated` → `handleRequestCompletion`
   (`0xfffffe0008efc364`, sole `bl` at `0x8f25da8`; extent 0xb10 bytes).

### 13.4 What this means for a first hardware command rung

The static picture says a submission is: (a) build the 64-byte RTKit message
with id and two u32 words, (b) enqueue into the channel's message buffer at the
cached write index, (c) store `0x40` into the shmem doorbell word §12.2, (d)
wait for the IRQ line matching the channel's cached index (status `0x188c000`,
ACK `0xffff` to `0x1890000`). Still *not* pinned statically, and all of it
device-side: the runtime channel-index values (firmware-published at boot), the
message id space, and the producer handshake word at control-block `+0x24`
(§13.2: stored inverted, acknowledged plain by the firmware — full/empty
arithmetic unresolved) together with the still-unresolved `+0x00/+0x08` fields.
A post-wake read of `0x188c000` and of the channel-name table would resolve
the first two without submitting anything. *Executed 2026-11-04: the
post-wake `0x188c000` read ran on hardware and returned `0x00000000` —
the wake handshake raises no IRQ line (receipts/2026-11-04-j700-h17-wake-irq-status);
the channel-name-table read and the doorbell re-read remain open. §14,
traced the same day, statically pins the publish/ack handshake around that
table — the count rides SCRATCH0, the request word is `0xF7FBDFF9`
(SCRATCH7), `0x08042006` is the firmware's reply, and SCRATCH3 is the
host-written registration word the firmware clears. The runtime channel
indices and the shmem doorbell stay device-side.)*

Raw evidence: `analysis/h17-messenger-irq-chain.txt` (the send/receive
primitives, the IRQ handler arms, the real `externalMethod`, and the
selector-2 wrapper). Again: decoded instruction stream, not device behaviour.

### 13.5 The direct-path client: the 9 selectors a submit actually uses

The same chained-fixup decode applied to `H11ANEInDirectPathClient`'s table
(`0xfffffe0007f5d6d0`, `w4 = 9` at `0xfffffe0008fc7574`) pins the restricted
direct-path ABI:

| sel | function |
|---|---|
| 0 | `ANE_DeviceOpen` |
| 1 | `ANE_DeviceClose` |
| 2 | **`ANE_ProgramSendRequest`** |
| 3 | `ANE_ProgramOutputSetEnqueue` |
| 4 | `ANE_ProgramInputsReady` |
| 5 | `ANE_MemoryMapRequest` |
| 6 | `ANE_MemoryUnMapRequest` |
| 7 | `ANE_SessionHintRequest` |
| 8 | `ANE_ProgramChainingSetActiveProcedure` |

The selectors are static free functions `ANE_*(ANEClientDevice*, void*,
IOExternalMethodArguments*)`. The selector-2 wrapper (`0xfffffe0008fc3b3c`)
reads the user-space ANE-program request struct's scalar fields at
`+0x18` (u64 user pointer) and `+0x20` (u32 length), wraps the user pointer in
a `ScopedMMap<ANEProgramCreateArgsOutput>` (`0xfffffe0008fc3340`,
`IOMemoryDescriptor::withAddressRange`), and forwards the kernel-mapped struct
to the `ANECoreInterface`/`ANEHWDevice` method chain (e.g.
`ANE_ProgramOutputSetEnqueue` → `ANECoreInterface::ANE_ProgramOutputSetEnqueue`
at `0xfffffe0008fae980`, `ANE_ProgramInputsReady` → `0xfffffe0008fae960`).
`ANEClientDevice::getClient` (`this+0x18`) supplies the embedded client in
every wrapper. For libane this is the complete userspace submission surface:
open → map buffers → inputs-ready/output-enqueue → send-request.


## 14. The channel-registration handshake (offline trace, 2026-11-04)

**Correction: see §15 before using this section as a hardware plan.** The
initial trace omitted descriptor construction and the first wake poll.
Its empty-table-buffer proposal is not a safe implementation plan. The
absence of direct `bl` calls also does not establish that a callback is
unused. Hardware observations in the separate rung receipts remain valid.

Whole-function trace of `ANEHWDevice::ANE_Init` (`0xfffffe0008f3dc28`,
AppleH16ANEInterface `__TEXT_EXEC 0xfffffe0008eeb820..0x8ff60dd8`), the
function Apple's driver runs after CPU release. It answers the §13.4 open
items "how does the firmware publish the channel table" and "what does the
host do around the wake word". Register reads go through
`ANERegisterControl::read32` (`0x8f73fdc`) and writes through
`ANERegisterControl::write32` (`0x8f74130`), both called with the accessor
at `[dev+0xe320]` and a register offset read from the §11 family table —
`[dev+0xfc..0x118]` = SCRATCH0..7 at `0x1880020..0x188003c`. x28 is pinned
at `dev+0xe1f0` (the channel-count anchor) at `0x8f3dc64`. Decoded
instruction stream — not device behaviour; every VA is
`0xfffffe0007004000 + file_offset` in the retained kernelcache.

### 14.1 Boot order up to the SCRATCH7 handshake

| step | VA | what |
|---|---|---|
| 1 | `0x8f3dd74` | `InitANEScratchRegisters` (`0x8f6121c`) |
| 2 | `0x8f3dd78` | `write32(SCRATCH6, 1)` — the same SCRATCH6=1 this driver's `h17_order` path performs |
| 3 | `0x8f3de44` | `CleanupRTBuddyEndpoints` (`0x8f52228`); failure logs "Failed to Initialize RTBuddy endpoints" (`0x73d724f`) and returns `0xE00002BC` (`0x8f3de48-0x8f3de50`) |
| 4 | `0x8f3e384-0x8f3e3a4` | `count := read32(SCRATCH0)` → `[dev+0xe1f0]` (`str w0,[x28]` at `0x8f3e390`); `[dev+0xe1f4] := read32(SCRATCH1) + 1` |
| 5 | `0x8f3e344-0x8f3e354` | pre-fill the 8 channel slots at `dev+0x3ef0+0x10i` with `{0xFFFFFFFF, 0xFFFFFFFF}` |
| 6 | `0x8f3eeb8-0x8f3f02c` | build the 0x100-byte RTBuddy message from the `[dev+0x4b0]` template (marker `0x40` at +0x68 = msgCount 64; trace-gated word dump) |
| 7 | `0x8f3f040-0x8f3f084` | `dsb st`; SCRATCH0 := lo32, SCRATCH1 := hi32 of a 64-bit ANE address derived from the `[dev+0x498]` command-buffer descriptor (base at `+0x18`, size at `+0x38` — the `cmd_buffer_base: 0x%llx, size: 0x%llu` string at `0x73d75ff` names those fields; the exact `x20` operand of `base + x20 - size` was not pinned further) |
| 8 | `0x8f3f088-0x8f3f098` | `write32(SCRATCH7, 0xF7FBDFF9)` — the host's **table request** word |
| 9 | `0x8f3f0dc-0x8f3f170` | poll `read32(SCRATCH7)` until it equals `0x08042006` (`w25` init at `0x8f3e180-0x8f3e184`), 1 ms `IOSleep` per iteration, 1000 max; success logs "Got second int from ANECPU (channel description table ready)" (`0x73d7706`); timeout increments the retry counter `[dev+0x8c]`, and at ≥ 2 logs "No second int from ANECPU" (`0x73d7662`) and returns `0xE00002DA` (the `-4` variant `0xE00002D6` at `0x8f3f7e8` exits the other timeout arm) |

Two static facts that change the wake recipe as recorded in the receipts:

- **The channel count is firmware-published in SCRATCH0.** `ANE_Init` reads
  it before writing the table address, and the driver's only other write to
  `[dev+0xe1f0]` is the zero-store in `initializeANEProperties`
  (`0x8f6422c`). The interim scan note that the count "is only zero-written,
  so the channel-table path is dormant in this cache" is **refuted**: the
  writer exists (`str w0,[x28]`, `0x8f3e390`) and was missed only because
  the literal-operand scan could not see stores through a register base.
- **`0x08042006` is a firmware reply at SCRATCH7.** The driver clears
  SCRATCH7 and polls for that reply; it does not write the reply there
  (`c52cbdb`, `ane/h16/ane_h16_main.c`, boot path). The kext has an initial
  wake poll at `0x8f3e188..0x8f3e25c` and a second poll after the
  `0xF7FBDFF9` request. The two phases must not be conflated (§15).

### 14.2 Table copy and per-channel objects

| step | VA | what |
|---|---|---|
| 10 | `0x8f3f1ec-0x8f3f240` | `IOFree([dev+0x3de0], count<<8)`; `kernel_debug(0x61b0058, 3)`; `IOMallocData(count<<8)` → `[dev+0x3de0]`, size `[dev+0x3de8]` |
| 11 | `0x8f3f244-0x8f3f27c` | re-read SCRATCH1/SCRATCH0 → ANE table address `hi<<32|lo` → `aneAddressToHostAddress` (`0x8f41314`) → `memmove([dev+0x3de0], host, count<<8)` (`_memmove` `0xb394bf0`) |
| 12 | `0x8f3f284` | `count == 0` → **skip the walk**, jump straight to the SCRATCH3 handshake; the handshake and name lookups still run with an empty table |
| 13 | `0x8f3f2a4-0x8f3f3c8` | per 0x100-byte entry: slot `{dev, source}` at `dev+0x3ef0+0x10i`; ctor `0x8ef8314` (`IOMalloc(0xc0)`) → object at `dev+0x3df0+8i`; per-type stats counters on the stack; log "Found channel %d in the channel table. Type=%d (%s), Source=%d, size: %d" (`0x73d774c`) |

Channel-table entry (stride `0x100`): `+0x00` name (compared with
`strnlen(name, 0x20)` + `strncmp`, `0x8f41790-0x8f417b0`), `+0x40` u32 type
(`== 0` → flag 1), `+0x44` u32 source, `+0x48` u32 size, `+0x50` u64 buffer
ANE address. The 0xc0-byte object (`0x8ef8364-0x8ef83cc`): `+0x00` =
`PACIA(doorBellRing, 0x2abe)` — the callback pointer is PAC-signed at the
  call site (`0x8f3f35c-0x8f3f368`). No direct `bl` was found, but this
  callback registration refutes §12.1's compatibility-only inference;
  indirect invocation must be traced separately. `+0x08` slot; `+0x10` flag;
`+0x14` size; `+0x18` host buffer; when flag && size ≥ 1 the ctor seeds
each 0x40-byte control block in the host buffer with `+0x00 = (u64)flag,
+0x08 = +0x10 = 0` and calls the null-guarded global `[0xc3f8d68]` hook as
`hook(block, 0x40)` — the same cache-clean hook recorded in §13.2.

### 14.3 Registration ack and the required channels

| step | VA | what |
|---|---|---|
| 14 | `0x8f3f3cc-0x8f3f4ac` | `write32(SCRATCH3, 0x08042006)`; poll `read32(SCRATCH3)` to 0, 1 ms sleep, 1000 max; timeout logs "No wake signal from rANE_SCRATCH3(0x%x) ANECPU" (`0x73d7802`); success `kernel_debug(0x61b0058, 5)` |
| 15 | `0x8f3f4d0-0x8f3f844` | `channelNameToIndex` (`0x8f41748`) for **"SHAREDMALLOC"** → `[dev+0xe1f8]`, **"TERMINAL"** → `[dev+0xe1fc]`, **"BUF_T2H"** → `[dev+0xe204]`; a miss returns `0xE00002C2`, logs "No %s ipc channel found" (`0x73d783a`/`0x73d785b`/`0x73d787c`) and ANE_Init exits `0xE00002DA` — all three names must exist in the published table |

So the scratch mailbox is a three-phase handshake: host publishes its table
buffer's ANE address (SCRATCH0/1) → host requests with SCRATCH7 `0xF7FBDFF9`
→ firmware acks SCRATCH7 with `0x08042006` → host copies the firmware's
table → host writes SCRATCH3 `0x08042006` and the firmware **clears** it as
the registration ack. SCRATCH7 is firmware→host, SCRATCH3 is host→firmware,
both with the same reply word. The `[dev+0xe20c]`/`[dev+0xe214]`
(IO_T2H / DATA_CHAIN_H2T) fields of the §13.3 dispatch switch are filled by
later lookups on the same table.

### 14.4 What this pins for the first hardware rung

- **Superseded by §15:** the handshake involves four scratch-word writes
  (SCRATCH0, SCRATCH1, SCRATCH7, SCRATCH3), plus constructing and mapping an
  initialization descriptor and its referenced memory. A scratch snapshot
  alone proves neither table availability nor channel reachability.
- The pending post-wake SCRATCH0..7 snapshot rung (§13.4) now has exact
  expectations: `SCRATCH7 == 0x08042006` (observed 2026-11-04), SCRATCH0/1
  either 0 or a stale address, SCRATCH3 untouched (never written by us),
  SCRATCH6 == 1 (we write it). *Executed 2026-11-04 on boot 3f9aa97c
  (receipts/2026-11-04-j700-h17-wake-scratch-snapshot): the firmware
  publishes SCRATCH0 = `0x8`, SCRATCH1 = `0xc440`, SCRATCH2 = `0x100`,
  SCRATCH3 = `0x00f00000` (without host involvement), SCRATCH5 =
  `0x100090` post-wake; SCRATCH4 0; SCRATCH6/7 as we left them. The
  "either 0 or a stale address" expectation was too conservative — the
  firmware writes real values into the scratch window.*
- What stays unpinned: the shmem doorbell word (`[dev+0x3b20] := 0x40`,
  §12.2 — IOSurface shared-event path only), the control-block producer
  arithmetic (§13.2), and the runtime channel indices — all firmware- or
  runtime-side; a device read of the published table is the only way to see
  them.

Raw evidence: `analysis/h17-chanreg-trace.txt` (the full dump sequence with
per-window VAs). Nothing in §14 has executed on a device.

## 15. Initialization descriptor prerequisites — correction before writes

Offline follow-up to `4a19594`/`6610ec7`, after the owner rebooted to boot
`0855c23b-dce5-47b0-83c5-a19baef2beef`. No module was loaded and no ANE
register was accessed during this follow-up. Source is the same retained
kernelcache (SHA-256 recorded in `analysis/h17-chanreg-prereqs.txt`). Run:

```
python3 scripts/chanreg_prereqs.py > analysis/h17-chanreg-prereqs.txt
```

### 15.1 Two distinct SCRATCH7 replies

CPU_CONTROL writes 0/0x10 occur at `0x8f3e148..0x8f3e164`. The kext then
polls SCRATCH7 for `0x08042006` (`0x8f3e180..0x8f3e25c`), **before**
reading the channel count at `0x8f3e384..0x8f3e390`. Our Linux rung at
`c52cbdb` performs that first wake, not the later descriptor handshake.
The kext's second request (`0xF7FBDFF9` at `0x8f3f090..0x8f3f098`) comes
only after the descriptor below is constructed. No extra request is needed
to explain the first firmware wake observed on Linux.

### 15.2 SCRATCH0/1 publish a populated initialization descriptor

`DeviceMemoryManager::Allocate(0x174)` at `0x8f3ecb8..0x8f3ecc0` returns
an offset (w0); -1 is refused. At `0x8f3ecdc..0x8f3ece4`, x20 becomes
`[[dev+0x498]+0x38] + sign_extend(w0)`. Later SCRATCH0/1 receive
`[[dev+0x498]+0x18] + x20 - [[dev+0x498]+0x38]` — equivalently the
descriptor's ANE base plus that offset (`0x8f3f044..0x8f3f084`). This is
**not** an empty channel-table destination. The allocator rounds its
request up to 64 bytes (`0x8ef90d0..0x8ef90d4`), so 0x174 requests consume
0x180 bytes; the observed final 16-byte zero store ends at +0x180.

Observed descriptor stores (meanings intentionally limited to data flow):

| Offset | Value/source | Store VA |
|---|---|---|
| +0x00 | conditional value from `[dev+0x288]` or `[[dev+0x490]+0x18]` | `0x8f3ed48..0x8f3ed54` |
| +0x08 | `[[dev+0x4a0]+0x18]` | `0x8f3ed58..0x8f3ed60` |
| +0x10 / +0x18 | zero-extended `[dev+0x3c0]` / 32-bit `0x10000000 - value` | `0x8f3ed64..0x8f3ed70` |
| +0x20 / +0x28 | optional allocated surface's +0x18 / selected allocation size | `0x8f3ed74..0x8f3ed8c` |
| +0x30 | u32 `[dev+0x4a8]` | `0x8f3ee24..0x8f3ee28` |
| +0x38..+0x4f | zeros | `0x8f3ee2c..0x8f3ee30` |
| +0x50 | zero-extended `[dev+0xe1f4]` (earlier SCRATCH1 + 1) | `0x8f3ee34..0x8f3ee38` |
| +0x58 / +0x60 | `[[dev+0x498]+0x18]` / `[[dev+0x498]+0]` | `0x8f3ee3c..0x8f3ee48` |
| +0x68 | u32 `0x40`; semantic name not established | `0x8f3eec4..0x8f3eec8` |
| +0x6c..+0x17f | zeros, then +0x6c..+0x16b overwritten from `[dev+0x4b0]` | `0x8f3eeb8..0x8f3ef2c` |

Other prerequisites include the kext's count bound (`count < 0x21` at
`0x8f3e75c..0x8f3e764`) and an IPC surface allocation through
`AllocateSharedMemorySurface` (`0x8f3e79c..0x8f3e7e4`). The slot-prefill
loop actually covers **16** 16-byte slots starting at `dev+0x3df0`
(`x8=-0x100; [dev+x8+0x3ef0]; x8+=0x10`), not eight slots starting
at +0x3ef0 as §14 claimed. The observed Linux count 8 remains an observed
value, not validation of that erroneous slot model.

### 15.3 Gate for a future write rung

Do not implement the earlier empty-buffer proposal. First resolve the
descriptor's required surfaces, the template at `[dev+0x4b0]`, address
translation, size limits, and cache/DMA ownership for each referenced
range. Then model successful and refused paths offline, independently
review the implementation, and stage a separate opt-in hardware rung.
Reading an address from SCRATCH0/1 after the second reply also does not
authorize arbitrary physical-memory reads: the complete table range must
translate into memory owned and retained by the module. Neither a
SCRATCH7 reply nor a copied table qualifies command or task execution.

## 16. Resolved descriptor producers and offline construction contract

Follow-up to `34704d4`. Source: the retained kernelcache SHA-256
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
`scripts/chanreg_prereqs.py` now emits the producer windows as well as
the consumers in `analysis/h17-chanreg-prereqs.txt`. This resolves the
source-level descriptor prerequisites of §15.3 into a construction model,
not an assertion that a Linux-created descriptor has run on hardware.

### 16.1 Referenced allocations and exact sizes

| Object | Producer | Contract |
|---|---|---|
| firmware / FWIM (`dev+0x490` for non-preload; `dev+0x288` mapped base for preload) | `start`, `0x8f55fa0..0x8f55fd4`; preload path §5.2 | `0x380000` bytes; descriptor +0 points at mapped firmware DVA, **not** its physical address |
| device memory / DM (`dev+0x498`) | `start`, `0x8f561a0..0x8f56238` | allocation request `0x40000`, tag `0x444d4d20`; CPU buffer is bzero'd and `DeviceMemoryManager` initialized over that size; descriptor is an offset allocation within this arena |
| IPC (`dev+0x4a0`) | `ANE_Init`, `0x8f3e79c..0x8f3e7e4` | requested extent `max(mapper_page_size, u32(SCRATCH1+1))`, tag `0x49504320`; descriptor +8 is its DVA |
| optional HEAP | `ANE_Init`, `0x8f3e9d0..0x8f3e9dc`, `0x8f3ebb0..0x8f3ec80` | if SCRATCH3 is zero, +0x20/+0x28 are zero; otherwise request `max(mapper_page_size, SCRATCH3)`, tag `0x48454150`; +0x20 is returned DVA and +0x28 is requested size |

The surface wrapper saves its requested size at metadata +0
(`0x8f4b6d0`, stack +0x80), DVA at +0x18, CPU pointer at +0x38
(`0x8f4be44`, stack +0xb8), then publishes the metadata at
`0x8f4be80..0x8f4bedc`. **+0x38 is a CPU pointer, not a size.**
That corrects the ambiguous label in §14.1 step 7; §15's address arithmetic
is unchanged. Descriptor +0x60 gets DM's requested size (+0 metadata).

For the recorded snapshot `SCRATCH1=0xc440`, the IPC requested extent is
**0xc441**, not 0x10000. A Linux 16-KiB-granule mapping must back 0x10000
bytes, but that padding is not part of the logical IPC extent. Descriptor
+0x50 is the unrounded `SCRATCH1+1`, even when it is smaller than a page.
With `SCRATCH3=0xf00000`, the optional HEAP request is 15 MiB. These
computed sizes do not establish that any allocation already exists on Linux.

### 16.2 The options template is reconstructible

`initializeANEProperties`, `0x8f641e4..0x8f64220`:

1. Zero `[dev+0x2ac]`.
2. Call `kalloc_data_external(0x100, 4)`; store pointer at `dev+0x4b0`.
3. Store u32 `[dev+0x2ac]` at template +0 and zero 8 bytes at +4.
4. OR bit 2 (`4`) into template u32 +0xc0.

The allocation's flag 4 is `Z_ZERO`. The retained wrapper
`0xb425f1c..0xb425f40` forwards the low three flag bits. Published XNU
corroboration: commit `f6217f891ac0bb64f3d375211650a4c1ff8ca1ea`,
[`osfmk/kern/zalloc.h`](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/kern/zalloc.h)
defines `Z_ZERO=0x0004`. This source gives the flag's name; the retained
cache gives the actual call and store sequence.

`start`, `0x8f53158..0x8f53194`, adds two conditional bits at template
+0xc0: OR `8` if `[dev+0xed]&1 && !([dev+0xec]&1)`; OR `0x10` if
`[dev+0x202]&1 && !([dev+0x201]&1)`. The offline model exposes this
selection explicitly (allowed values 4/12/20/28), rather than claiming the
Linux snapshot measured either feature. It models the traced default
configuration and these modifiers, not arbitrary macOS property overrides.
The full 256-byte template is copied to descriptor +0x6c; the flags thus
land at **descriptor +0x12c**. All remaining template bytes are zero on
this traced default path. The marker at descriptor +0x68 remains `0x40`;
no semantic name beyond the store value is needed to construct it.

The u32 at descriptor +0x30 comes from `[dev+0x4a8]`, initialized zero
at `0x8f64184`. `ANE_LoadFirmware_gated` can replace it with its second
unsigned argument (incoming w3 saved as w23 at `0x8f461d4`, stored at
`0x8f462d8`). The existing preload rung never calls that upload method;
the model defaults this field to zero but retains an explicit u32 input.

### 16.3 Ownership and address translation contract

The DM host→ANE translator (`0x8f412f8..0x8f41310`) is exactly
`surface.DVA + (host_pointer - surface.CPU_pointer)`. SCRATCH0/1 receive
that address's low/high halves. There is no DMA-handle-as-physical shortcut.
The reply-side translator has firmware/IPC/preload branches; for IPC it
returns `surface.CPU_pointer + (reply_DVA - surface.DVA)` at
`0x8f413b4..0x8f413d4` and `0x8f41534..0x8f41538`.

For Linux, the next implementation must use the already-established
DMA-API domain (see `34704d4:ane/h16/ane_h16_main.c`,
`ane_h16_stage_domain` / `ane_h16_map_stage`): allocate coherent DM/IPC/
HEAP memory, retain their CPU pointers and DMA handles, and validate every
complete mapped extent. An allocated DMA handle is a DVA, never a PA.
If an additional alias is needed, it must be reserved before allocation,
mapped using `iommu_iova_to_phys` on the owned handle, and use the existing
checked mapping/rollback discipline. Do not guess an unreserved address.
Coherent buffers plus a DMA write barrier before publishing the scratch
pointer avoid importing macOS cache hooks; a device-visible handshake reply
must precede the DMA read barrier and table copy. `dsb st` in the kext
is ordering evidence, not proof of Linux DMA coherency.

All referenced buffers and mapping records must be retained after a request
or timeout until reboot. The model refuses unasserted ownership/retention,
overlap, misalignment, out-of-aperture mappings, and wrapped input sizes.
Those are **offline assertions**, not substitutes for kernel allocation
and IOMMU checks.

The reply table is `channel_count*0x100` bytes. Our first-run policy is
stricter than the kext's start-pointer check: **the entire range must fit
inside the owned logical IPC extent**. No read from arbitrary physical
memory, firmware TEXT, another allocation, mapping padding, or a one-past-end
pointer is accepted. The model accepts counts 1..32 (source bound is <33;
zero would fail the subsequent required-channel lookups anyway).

### 16.4 Executable model and remaining hardware boundary

`ane/h17/chanreg_model.py` builds the 0x180-byte allocation, including
all §15 stores, default template and conditional flags, from explicit
owned-region inputs. `ane/h17/test_chanreg_model.py` checks every byte
against an independent store list, exact/requested-vs-mapped sizes,
optional HEAP, all flag combinations, count limits, final valid descriptor
slot, overflow, unowned/unretained/overlapping regions, and full reply ranges.

```
make -C ane/h17 chanreg-model-test
```

The 12 test methods pass; addresses in tests are synthetic and explicitly
marked so. Test output is retained in `analysis/h17-chanreg-model-tests.txt`.
The target also runs under `ane/h17`'s `sim-all`; the full successful run
is retained in `analysis/h17-chanreg-sim-all.txt`.

**The descriptor's source-level construction contract is resolved.** The
model does not implement or enable a kernel rung. A first table-discovery
rung should publish the constructed descriptor, request with SCRATCH7,
copy a validated table, and **stop before SCRATCH3 registration**. The
kext initializes per-channel slots through the constructor (§14.2) before
writing that registration word; skipping those buffer writes and sending
the final ack is not an equivalent sequence. That is a later rung after
validating every channel-buffer range. No doorbell, task, IRQ ack, or new
hardware access is added by this change.
