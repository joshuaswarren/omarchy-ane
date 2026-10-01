# H13 (T8103) ANE firmware — `sCSneCmdProgramLoad` / PROG_LOAD field-level spec

Derived **only** from disassembly of the H13 13.5 (22G74) ANE firmware. Offline static analysis;
no fleet machine touched, no hardware or software changed.

- Target image: `~/.local/share/apple-silicon-lab/artifacts/jwm1-parity/ane-fw-t8103/h13_ane_fw_13.5_22G74.bin`
  sha256 `7f906d11897cb930e5c8bfc155b50c44d803f8a64fcf9ee99152109f3a304f34`
- Reference image: `t602x_selene_13.5_22G74.bin` sha256 `a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`
- Checker source file (from in-image string, `0xb2b7d`): `./sne/aneEngine/program/CAneProgramCheckerH13.cpp`
- All addresses are VAs in the H13 image unless prefixed `sel`. Every field row is marked
  **VERIFIED** (read directly in the cited disassembly) or **INFERENCE** (meaning deduced, mechanics verified).
- Notebook threads: H169 (`entries/jwm1-parity/20261003T193000Z-…h13-vs-selene-fw-offline-h169.md`),
  OpSectionFormat, ProcSectionFormat (selene-side; their addresses do NOT apply to H13).

Functions decoded (H13):

| Function | VA | Insns |
|---|---|---|
| `verifyProgramSection(const sCSneCmdProgramLoad*)` | `0x4a1fc` | 139 |
| `verifyProgram(const sCSneCmdProgramLoad*)` | `0x4b9f8` | 229 |
| `verifyGenericSection(const sCSneCmdProgramGenericSection*, unsigned long)` | `0x4a428` | 107 |
| `verifyKernelPropSection(const sCSneCmdProgramKernelSectionListHeader*, unsigned long)` | `0x4a5d4` | 84 |
| `verifyOperationSection(void*, unsigned long, unsigned int*)` | `0x4a724` | 251 |
| `verifyProcedureSection(const sCSneCmdProgramProcedureSectionListHeader*, unsigned long)` | `0x4ad38` | 84 |
| `verifyDescriptorPropSection(void*, unsigned long, unsigned char*, unsigned long)` | `0x4ae88` | 132 |
| `verifyDescriptors(void*, unsigned char*, void*)` | `0x4b098` | 159 |
| `checkBarEachAneOp(void*, sCSneCmdProgramGenericSection*, sCSneBufferDescriptor*, sCSneBufferDescriptor*)` | `0x4b310` | 254 |
| `verifyBAR(const sCSneCmdProgramLoad*)` | `0x4b708` | 188 |
| Callers: `CAneProgramManager::AddProgram` `0x36288`, `CANEController::CmdProcessor` (calls at `0x270e8`), `CAneServer::InitProgrm` (call at `0x60ed4`), `CAneServer::LoadProgramsInAFP` `0x5fae0`, `CAneServer::LoadProgram` `0x61738` |

## 1. `struct sCSneCmdProgramLoad` — top level

Size **0x1c0 bytes**. VERIFIED as the per-program table stride on the firmware side:
`LoadProgramsInAFP` builds the table at `this+0x1c0` with `mov w10, #0x1c0` stride
(`0x5fd08`–`0x5fd10`), `InitProgrm` walks the same table (`0x60ec8`–`0x60ed0`), and the
command's status field at `+0x1b8` (`0x270ec` write, `0x362d0` trace read) sits immediately
before the stride boundary.

The struct is a command header plus **nine 0x30-byte section groups** at `0x08 + 0x30*n`,
each group:

| group offset | width | field | evidence |
|---|---|---|---|
| +0x00 | u32 | `valid` (bit0 tested) | VERIFIED — `ldrb w8,[x19,#8]; tbnz w8,#0` `0x4ba10`; assert string `program->genericSection.valid` `0xb3853` |
| +0x04 | u32 | `count` | VERIFIED read — `ldr w15,[x19,#4]` (kernel, `0x4b458`), `ldr w14,[x20,#4]` (text, `0x4b46c`); never read for other groups |
| +0x08 | 0x10 | spare (unverified) | VERIFIED touched once — `ldr x9,[x19+0x10]!` / decrement in `AddProgram` failure path `0x364c4`–`0x364d8` (generic group only); purpose **unresolved** |
| +0x18 | u64 | `buffer` (device address) | VERIFIED — e.g. generic `ldr x0,[x0,#0x20]` `0x4ba08` |
| +0x20 | u64 | `size` | VERIFIED — e.g. `ldr x1,[x19,#0x28]` `0x4bb48` |

Group bases and names (VERIFIED via in-image log/assert strings):

| base | name | string evidence |
|---|---|---|
| `0x08` | `genericSection` | `'[No] Generic Section'` `0xb3919`; assert `program->genericSection.valid` `0xb3853` |
| `0x38` | `kernelSection` | `'[X] kernelSection is valid but no buffer!'` `0xb3a1a`; assert `program->kernelSection.size` `0xb38c2` |
| `0x68` | `textSection` (the TD/descriptor blob) | `'[No] TD Section'` `0xb392e`; asserts `program->textSection.valid` `0xb388d`, `program->textSection.size` `0xb38a8` |
| `0x98` | `operationSection` | `'[No] Operation Section'` `0xb3953`; assert `program->operationSection.valid` `0xb3829` |
| `0xc8` | `procedureSection` | `'[No] Procedure Section'` `0xb396a` |
| `0xf8` | `kernelPropSection` | `'[X] kernelPropSection is valid but no buffer!'` `0xb39ec` |
| `0x128` | `tdPropSection` (a.k.a. `textProp`) | `'[No] TD Prop Section'` `0xb393e`; `verifyProgramSection` label `'textPropSection.buffer=…'` `0xb2da3` |
| `0x158` | opDbg (unverified) | CmdProcessor dump `'opDbg buf = 0x%llx, size=0x%llx'` `0xab662` |
| `0x188` | "proc" (unverified) | CmdProcessor dump `'proc buf = 0x%llx, size=0x%llx'` `0xab688` |

Header / trailer fields:

| offset | width | field | evidence |
|---|---|---|---|
| `0x00` | 4 | unknown | not read in any analyzed function — unresolved |
| `0x04` | u16 | programId | VERIFIED read — `ldrh w22,[x21,#4]` `0x27104` (printed next to `'… ProgramId = %d'` `0xab6ae`) |
| `0x06` | u16 | unknown | not read — unresolved |
| `0x1b8` | u32 | host return status | VERIFIED — result of `AddProgram` stored `str w0,[x21,#0x1b8]` `0x270ec`; traced `ldr w3,[x19,#0x1b8]` `0x362d0` |
| `0x1bc` | 4 | pad | — |

### 1.1 Boundary pre-filter — `verifyProgramSection` (`0x4a1fc`)

Runs **before** everything (`AddProgram` `0x362b0`). For each of the seven real section groups
(buffer fields at `+0x20, +0x50, +0x80, +0xb0, +0xe0, +0x110, +0x140`, checked in that order,
`0x4a234`–`0x4a29c`):

- `buffer + size <= 0xE0000000` — VERIFIED. Limit from literal pool: `ldr x8,#0xc1c28` → `0x700000`,
  `ldr x9,#0xc1c30` → `0xdf900000`; `maxAddr = 0xdf900000 + 0x700000` (`0x4a22c`–`0x4a230`).
- On failure: `'genericSection.buffer=0x%llx, genericSection.size=%lld, maxAddr=0x%llx'` …
  `'buffer address out of boundary, load_program failed'` (`0xb2bb7`…`0xb2ded`).
- Only an upper bound is enforced; `0xdf900000` appears only as the pool base, not as a checked
  lower bound.

### 1.2 Mandatory/optional matrix — `verifyProgram` (`0x4b9f8`)

Presence = group `valid` bit0 set AND `buffer != 0`. Order of checks (error line numbers from
`mov w2,#line` before `CLogger::Error`):

| group | required | check addr | error string |
|---|---|---|---|
| genericSection | **always** | `0x4ba08`/`0x4ba10` | `'[No] Generic Section'` (line 0x1d0) |
| textSection | **always** | `0x4ba48`/`0x4ba50` | `'[No] TD Section'` (0x1d4) |
| tdPropSection | **always** | `0x4ba88`/`0x4ba90` | `'[No] TD Prop Section'` (0x1d8) |
| operationSection | **always** | `0x4bac8`/`0x4bad0` | `'[No] Operation Section'` (0x1dc) |
| procedureSection | **always** | `0x4bb08`/`0x4bb10` | `'[No] Procedure Section'` (0x1e0) |
| kernelPropSection | if `valid`@`0xf8` | `0x4bb70`–`0x4bb88` | null buffer: `'[X] kernelPropSection is valid but no buffer!'` (0x1ff); verify fail 0x1fb |
| kernelSection | if `valid`@`0x38` | `0x4bb8c`–`0x4bb98` | `'[X] kernelSection is valid but no buffer!'` (0x206) |

Content verification order: generic (`0x4bb48`) → operation (`0x4bb54`, out-param `NULL`) →
procedure (`0x4bb64`) → kernelProp if valid (`0x4bb84`) → tdProp (`0x4bba4`) →
`verifyDescriptors(tdProp.buffer, text.buffer, op.buffer)` (`0x4bbac`) → `verifyBAR(prog)` (`0x4bbc4`).
`AddProgram` gates the whole `verifyProgram` call on a manager flag byte
(`ldrb w8,[x22,#0x28]; cbz` skip, `0x363b0`–`0x363b4`) — but `verifyProgramSection` always runs.

## 2. Section content specs

### 2.1 Generic section (`sCSneCmdProgramGenericSection`) — `verifyGenericSection` (`0x4a428`)

| offset | width | field | rule | evidence |
|---|---|---|---|---|
| `0x000` | u32 | `maxAneUsed` | must equal **1** | VERIFIED `0x4a438`–`0x4a440`; `'[VERIFICATION] maxAneUsed %d :: H11 maxAneUsed should be %d!'` `0xb2e29` |
| `0x004` | u32 | `nbrOfNe` | `<= 0x10` | VERIFIED `0x4a444`–`0x4a44c`; `'Generic ANE[%d] nbrOfNe %d exceeds H11 max NE %d!'` `0xb2e70` |
| `0x204` | u32 | `totalBufferNbr` | `1 .. 0x200` inclusive | VERIFIED `0x4a4d8`–`0x4a4e4` (`sub w9,w8,#0x201; cmn w9,#0x201; b.hi ok`); `'totalBufferNbr %d :: range should 0 < # < ECSneProgramMaxBuf (%d)!'` `0xb2ebb`, limit `0x200` |
| — | — | section size | `0x208 + totalBufferNbr*0x30 <= size` | VERIFIED `0x4a510`–`0x4a520` |
| `0x208 + 0x30*i` | u8 | buffer `valid` | bit0 must be set | VERIFIED `0x4a554`–`0x4a558`; `'Generic section buffer[%d] is not valid!'` `0xb2f64` |
| `0x210 + 0x30*i` | u32 | `bufferIndex` (a.k.a. "type") | `< 6` | VERIFIED `0x4a54c` (`add x10,x0,#0x210`), `0x4a55c`–`0x4a564` (`ldr w11,[x10]; cmp w11,#6; b.hs`), stride `0x4a56c`; `'Generic section buffer[%d] is wrong type %d!'` `0xb2fa6` |

Correction (w71, re-read from the disassembly; the first draft said entry+4 / 0x20c): the valid byte is at entry+0 (`0x208+0x30i`, `ldurb [x10,#-8]`) and the checked word is at entry+8 (`0x210+0x30i`). Entry bytes `+0x01..+0x07` and `+0x0c..+0x2f` are not read by this verifier
(presumably address/size consumed at bind time) — unresolved. Other offsets in this file were produced by a subagent and not independently re-read; treat any item not cited with an address as INFERENCE until re-verified.

### 2.2 KernelProp section (`sCSneCmdProgramKernelSectionListHeader`) — `verifyKernelPropSection` (`0x4a5d4`)

| offset | width | field | rule | evidence |
|---|---|---|---|---|
| `0x0` | u32 | entry count | `== 0` → **FAIL** (return 0) | VERIFIED `0x4a5e8`–`0x4a5ec`, `0x4a6a0`–`0x4a6b0` |
| `0x8 + 0x18*i` | u64 | (unchecked word) | not read | VERIFIED — unresolved meaning |
| `0x10 + 0x18*i` | u64 | `offset` | `entry[i].offset >= entry[i-1].offset + entry[i-1].len` | VERIFIED `0x4a600`–`0x4a610`; `'KernelProp[%d] offset 0x%llx is overlapped with the previous offset 0x%llx len %lld!'` `0xb2fec` |
| `0x18 + 0x18*i` | u64 | `len` | `offset + len <= section size` | VERIFIED `0x4a614`–`0x4a638`; `'KernelProp[%d] exceeds limit (offset, len) …'` `0xb305a` |

### 2.3 Procedure section (`sCSneCmdProgramProcedureSectionListHeader`) — `verifyProcedureSection` (`0x4ad38`)

| offset | width | field | rule | evidence |
|---|---|---|---|---|
| `0x0` | u32 | entry count | `== 0` → **FAIL** | VERIFIED `0x4ad4c`–`0x4ad50`, `0x4ae04` |
| `0x8 + 0x10*i` | u64 | `offset` | `entry[i].offset >= entry[i-1].offset + entry[i-1].len` (i>0) | VERIFIED `0x4ad64`–`0x4ad74`; `'Procedure[%d] offset 0x%llx is overlapped with the previous offset 0x%llx len %lld!'` `0xb32e7` |
| `0x10 + 0x10*i` | u64 | `len` | `offset + len <= section size` (also for i=0) | VERIFIED `0x4ad78`–`0x4ad9c`; `'Procedure[%d] exceeds limit (offset, len) …'` `0xb3354` |

### 2.4 Operation section — `verifyOperationSection` (`0x4a724`)

Header and entries:

| offset | width | field | rule | evidence |
|---|---|---|---|---|
| `0x0` | u32 | `tot` (operation count) | `tot <= 0x80`; `tot == 0` passes **this** verifier (returns 1) but **fails later** in `verifyDescriptors` (see 2.7) | VERIFIED `0x4a734`, `0x4a7a4`–`0x4a7a8` (`'Operation number %d exceeds MAX Operation number (%d)!'` `0xb310e`), `0x4a7e4` |
| — | — | section size | `4 + tot*0x110 <= size` | VERIFIED `0x4a738`–`0x4a748` |
| `0x4 + 0x110*i + 0x00` | u32 | `opType` | `0..3`; only **0** ("AneOp") is walked further | VERIFIED `0x4a828`–`0x4a834`; `'Operation[%d] wrong opType %d!'` `0xb315e` |
| `…+0x04` | u16 | TD range `start` | see TD-range rule below | VERIFIED `0x4a800` |
| `…+0x08` | u16 | TD range `end` (inclusive) | `end - start + 1 <= 0x10000` | VERIFIED `0x4a7fc`–`0x4a810`; `"Number of TDs (%d) in one operation exceeds TQ's limit (%d)"` `0xb3291` (limit `0xffff` arg) |
| `…+0x0a` | u16 | `nbrOfNe` | `<= 0x10` (type-0 entries only) | VERIFIED `0x4a83c`–`0x4a844`; `'Operation[%d] nbrOfNe %d exceeds H11 max NE %d!'` `0xb3196` |
| `…+0x0c` | u32 | `nbrOfLocalbarSetup` | `<= 0x20` | VERIFIED `0x4a84c`–`0x4a858`; `'Operation[%d] nbrOfLocalbarSetup %d exceeds EAnsProgramBarMaxIndex %d!'` `0xb31df` |
| `…+0x10 + 8k` | u32 | `bar[k].barIndex` | `<= 0x1f`, k < `nbrOfLocalbarSetup` | VERIFIED `0x4a860`–`0x4ab50` (unrolled); `'Operation[%d] bar[%d] index %d exceeds H11 bar slots %d!'` `0xb323f` (limit `0x20`) |
| `…+0x14 + 8k` | u32 | `bar[k].bufferIndex` | cross-checked in `verifyBAR` (see 2.8); not range-checked here | VERIFIED read in `checkBarEachAneOp` `0x4b460` |

The pair array exactly fills the 0x110 stride (`0x10 + 32*8 = 0x110`). A third parameter
(`unsigned int*`) receives `1` on success when non-NULL (`0x4ab5c`–`0x4ab68`); `verifyProgram`
passes `NULL` (`0x4bb58`).

### 2.5 Text (TD blob) and TD-prop sections

`textSection` (group `0x68`) holds the raw task-descriptor blob; `tdPropSection` (group `0x128`)
is a directory of TDs inside it. TD base address = `textSection.buffer + tdPropEntry.offset`
(VERIFIED `verifyDescriptors`: `add x5, x1, x4` `0x4b148`; the same TD's header word is read at
`textSection.buffer + 0x18 + tdPropEntry.offset` in `verifyDescriptorPropSection` (`0x4af6c`,
`0x4afe8`) — i.e. the TD header spans ≥ 0x1c bytes with a type word at `+0x18`).

**TD header (as read by the verifiers):**

| TD offset | width | field | evidence |
|---|---|---|---|
| `+0x00` | u16 | `TdPropTID` (must equal the op-relative prop index) | VERIFIED `0x4b14c`, `0x4b150`–`0x4b158` |
| `+0x06` | u8 | `Hdr1.f.NextSize` (next TD-prop length in u32 words) | VERIFIED `0x4b128` (`ldrb w15,[x5,#6]`), used `0x4b164` |
| `+0x18` | u32 | type word; **bit24 (`TDE`)** selects TD header size | VERIFIED `0x4afec`–`0x4aff0` (`tst w9,#0x1000000`), `0x4affc` (`ubfx w8,w9,#0x18,#1`) |
| `+0x1c` | u32 | `Hdr7.f.NextPointer` (next TD-prop offset) | VERIFIED `0x4b12c` (`ldr w13,[x5,#0x1c]`), used `0x4b178` |

**`verifyDescriptorPropSection(tdProp, tdPropSize, textBuffer, textSize)` (`0x4ae88`):**

| offset | rule | evidence |
|---|---|---|
| `0x0` u32 `tdTotal` | `tdTotal == 0` → **PASS** (returns 1); else `4 + tdTotal*0x30 <= tdPropSize` | VERIFIED `0x4aeb4`–`0x4aec8`, `0x4af60`–`0x4b024`; `'TdProp section (%lu) is smaller than actual (%lu)!'` `0xb33d3` |
| entry `0x8+0x30i`, u32@`+0` `offset` | byte offset into text blob; `entry[i].offset >= entry[i-1].offset + entry[i-1].len` (i>0) | VERIFIED `0x4afac`–`0x4afbc`; `'TD[%d] offset 0x%x is overlapped with the previous offset 0x%x len %d!'` `0xb341f` |
| entry u32@`+4` `len` | `offset + len <= textSize`; also `len >= 0x28` if TD bit24 set, else `len >= 0x2c` | VERIFIED `0x4afc0`–`0x4afcc` (`'TD[%d] exceeds limit (offset, len) …'` `0xb347f`); `0x4afe8`–`0x4aff8` (`'TD[%d] len %d is smaller than ane_TD_HEADER_t (TDE %d)!'` `0xb34d8`) |
| entry u32@`+0x2c` | next index in the per-op TD chain (consumed by `verifyDescriptors`) | VERIFIED read `0x4b144` (`ldp w20,w4,[x3,#-4]`) |
| entry `+0x08..+0x28` | not read by any verifier | unresolved |

### 2.6 TD chain rules — `verifyDescriptors(tdProp, textBuffer, opSection)` (`0x4b098`)

Asserts its three args non-null (`'pTdPropSection'` `0xb33b8`, `'pDescriptor'` `0xb33c7`,
`'pOpSection'` `0xb3529`; lines 0x113–0x115).

1. `op.tot == 0` → **FAIL** (`0x4b0b4`–`0x4b0b8`, return 0 at `0x4b260`). So although
   `verifyOperationSection` accepts an empty operation section, `verifyProgram` overall does not.
2. For every type-0 op entry, with `start = u16@entry+4`, `end = u16@entry+8`,
   `tdTotal = u32@tdProp+0`:
   - **Range rule:** FAIL `'TD index wrong : OP[%d] start:end (%d:%d) TD total %d'` (`0xb3534`,
     line 0x11f) iff `start < tdTotal <= end` (`0x4b104`–`0x4b110`, ccmp chain). I.e. the
     `[start:end]` window may lie fully inside the tdProp table (`end < tdTotal`) or fully past
     its end (`start >= tdTotal`), but must not straddle `tdTotal`.
3. The walk then iterates `k = 0 .. (end-start)` over tdProp entries `prop[start+k]` (VERIFIED
   `0x4b120` `umaddl x3, w16, w14, x11` → `tdProp+8+0x30*(start+k)`; loop exit `cmn w2,w17`
   `0x4b138` with `w2 = start-end-1`, i.e. `end-start+1` iterations — one **past** `end`,
   inclusive end). Per k:
   - chain word `w20 = [prop[start+k] - 4]` = `tdProp+4` when `start+k == 0`, else
     `prop[start+k-1].u32@+0x2c`;
   - `u16` at `textBuffer + prop.offset` (`TdPropTID`) must equal `w20` **and** equal `k`
     (VERIFIED `0x4b148`–`0x4b158`, `b.ne` → `'TD[%d] (headerTID:TdPropTID:index)=(%d:%d:%d)'`
     `0xb3583`, line 0x129). So TDs are threaded: `prop[i-1].u32@0x2c == i-start` and the TD's
     first u16 equals its op-relative index; for `start == 0` the header word `u32@tdProp+4`
     must be 0.
   - for `k >= 1` additionally: `prop[start+k].len == 4 * TD(start+k-1).Hdr1.NextSize + 4`
     (VERIFIED `0x4b164`–`0x4b174`, `ldr w6,[x3,#4]`; `'TD[%d] len(%d) != prev Hdr1.f.NextSize(%d)'`
     `0xb35ca`, line 0x130) and `prop[start+k].offset == TD(start+k-1).Hdr7.NextPointer`
     (VERIFIED `0x4b178`–`0x4b17c`; `'TD[%d] offset(0x%x) != prev Hdr7.f.NextPointer(0x%x)'`
     `0xb360e`, line 0x136). These two checks are skipped when `end-start+1` iterations end
     first (the `cmn` exit at `0x4b13c` bypasses them for the last pair).
   - quirk: when `start >= tdTotal` (range fully past the table) the walk still runs and reads
     entries beyond `tdTotal` — bytes inside the section but outside the counted table. The
     firmware enforces the same chain rules against that data.

### 2.7 BAR cross-check — `verifyBAR` (`0x4b708`) + `checkBarEachAneOp` (`0x4b310`)

`verifyBAR` asserts (each `CLogger::Assert` + deliberate hang): program non-null (`0xb2baf`,
line 0x1a5); `program->operationSection.valid` @`0x98` (`0xb3829`, 0x1a7);
`operationSection.buffer` @`0xb0` (`0xb3849`, 0x1a9); `program->genericSection.valid` @`0x08`
(`0xb3853`, 0x1ab); `genericSection.buffer` @`0x20` (`0xb3663`, 0x1ad);
`generic->totalBufferNbr > 0` (`0xb3871`, 0x1ae); `program->textSection.valid` @`0x68`
(`0xb388d`, 0x1b0); `program->textSection.size` @`0x88` non-zero (`0xb38a8`, 0x1b1);
`program->kernelSection.size` @`0x58` non-zero when kernel valid (`0xb38c2`, 0x1b5).

Loop (`0x4b91c`–`0x4b958`): for every operation entry with `opType == 0`
(`ldur w9,[x22,#-4]; cbnz` skip), call
`checkBarEachAneOp(entry+4, genericSection, prog+0x68 (textSection), kernelSection-present ? prog+0x38 : NULL)`.
Failure → `'Operation[%d] BAR setup is wrong!'` `0xb38de` (line 0x1c4).

`checkBarEachAneOp` per type-0 entry:

| check | rule | evidence |
|---|---|---|
| BAR setup count | `nbrOfLocalbarSetup` @`entry+0xc` `<= 0x20`; `== 0` → pass | VERIFIED `0x4b348`–`0x4b350` (`'Operation BAR setup number %d is exceed MAX_BAR_SLOTS %d!'` `0xb366e`), `0x4b420` |
| `barIndex` | `bar[k]` @`entry+0x10+8k` `< 0x20` | VERIFIED `0x4b444`–`0x4b450`; `'BAR[%d] index %d should <= %d!'` `0xb36c1` |
| `bufferIndex` vs kernel | if kernelSection present: `kernelSection.count` (u32@`prog+0x3c`) `!= bufferIndex` @`entry+0x14+8k` | VERIFIED `0x4b458`–`0x4b47c`; `'BAR[%d] bufferIndex %d is matched with buffers more than one!'` `0xb36f9` (line 0x16b) |
| `bufferIndex` vs text | `textSection.count` (u32@`prog+0x6c`) `!= bufferIndex` when a matching generic buffer exists | VERIFIED `0x4b46c`–`0x4b484` (collision flag), `0x4b4e0`–`0x4b4ec` (same string `0xb36f9`, line 0x178) |
| `bufferIndex` resolution | some **valid** generic buffer entry must have `bufferIndex@+4 == bufferIndex` | VERIFIED `0x4b4bc`–`0x4b4f4`; assert `generic->buffers[j].valid` `0xb3750` (line 0x173); `'BAR[%d] bufferIndex %d is NOT matched with any buffer!'` `0xb376a` (line 0x181) |

Net constraint per `{barIndex, bufferIndex}` pair: `barIndex < 0x20`; `bufferIndex` must match
exactly one valid generic-section buffer and must differ from both `textSection.count` and
`kernelSection.count` (those two counts are the buffer ids of the TD blob and the kernel section
in the same id space — dump strings `'TD : bufferIndex %d'` `0xb37fd`, `'Kernel : bufferIndex %d'`
`0xb3811` at `0x4b63c`–`0x4b688`).

## 3. Where the struct and buffers come from (host path)

- `CANEController::CmdProcessor` receives the CSNE_CMD payload (`x21`) and calls
  `AddProgram(progMgr, cmd)` at `0x270e8`; the u32 return value is written back to the command
  at `+0x1b8` (`0x270ec`) — the host reads its status there. CmdProcessor dumps the section
  buffer/size pairs for generic/kernel/Descriptor/operation/procedure/kernelProp/tdProp/opDbg/proc
  (`0x27000`–`0x270d4`) — the log names match the group table above.
- `AddProgram` (`0x36288`): `verifyProgramSection` always (`0x362b0`, fail → `-1`); optional
  `CAneProgram::SaveProgram` gated on manager bits 2/5 of byte @`mgr+0x44` (`0x3631c`–`0x3632c`);
  index-pool allocation limited to 0x100 programs (`0x36344`–`0x36348`);
  `verifyProgram` only when manager byte @`mgr+0x28` set (`0x363b0`–`0x363c4`).
- `CAneServer::LoadProgram` (`0x61738`) is a lookup: program index `< 0x1b` (27) — max programs
  per engine — against server tables at `+0x4ea8/+0x4eac/+0x4eb0` (stride 0xc) and validity word
  at `+0x378 + idx*0x1c0`.
- `CAneServer::LoadProgramsInAFP` (`0x5fae0`) is the *preloaded-firmware* path: parses the
  in-firmware `'ANEH'`/`'ANEP'` records (`0x5fb58`, `0x5fc58`), memcpy's a 0x1c0-byte
  `sCSneCmdProgramLoad` per program into `this+0x1c0+slot*0x1c0` (`0x5fd08`–`0x5fd20`), then
  patches six section buffer/size pairs from ANEH sub-records — store offsets
  `+0x1e0/+0x210/+0x240/+0x270/+0x2a0/+0x300` relative to the slot base = struct
  `+0x20 (generic) / +0x50 (kernel) / +0x80 (text) / +0xb0 (operation) / +0xe0 (procedure) /
  +0x140 (tdProp)` (`0x5fe28`–`0x5fed8`). kernelProp (`+0x110`) is never populated here,
  consistent with its optional status. `InitProgrm` then `AddProgram`s each table entry
  (`0x60ec8`–`0x60ed4`).

## 4. Host must supply (T8103 PROG_LOAD builder checklist)

1. `buffer + size <= 0xE0000000` for all seven real sections (device-addressable window;
   `verifyProgramSection`, `0x4a234`–`0x4a29c`).
2. `valid` bit0 + non-zero `buffer` for the five mandatory groups (generic, text, tdProp,
   operation, procedure); kernelSection and kernelPropSection optional but, if `valid`, need
   non-zero `buffer` and `size`.
3. Generic: `maxAneUsed == 1`; `nbrOfNe <= 0x10`; `1 <= totalBufferNbr <= 0x200`;
   every buffer entry `valid` bit0 set with `bufferIndex < 6`; section sized
   `0x208 + n*0x30`.
4. Operation: `tot <= 0x80` and `tot >= 1` (empty section dies in `verifyDescriptors`); each
   entry `opType <= 3`; type-0 entries: `nbrOfNe <= 0x10`, `nbrOfLocalbarSetup <= 0x20`,
   `barIndex < 0x20`, TD range `end-start+1 <= 0x10000`; section sized `4 + tot*0x110`.
5. Every BAR pair's `bufferIndex` must equal exactly one valid generic buffer's
   `bufferIndex`, and must not equal `textSection.count` or `kernelSection.count`.
6. Procedure: `count >= 1`; 16-byte entries `{u64 offset, u64 len}` tiling the section
   (`entry[0].offset+entry[0].len <= size`, monotonic, `offset+len <= size`).
7. KernelProp (if used): `count >= 1`; 0x18-byte entries `{u64 ?, u64 offset, u64 len}`, same
   tiling rules.
8. TD blob + tdProp directory: tdProp entries `{u32 offset, u32 len, …, u32 nextIdx@0x2c}`
   (0x30 stride) tiling the blob; TD headers ≥ 0x28/0x2c bytes (bit24 of word@TD+0x18 selects);
   chain invariants per type-0 op: `TD[i].u16@0 == i-start == prop[i-1].u32@0x2c`
   (`u32@tdProp+4 == 0` for `start == 0`), `prop[i].offset == TD[i-1].u32@0x1c`,
   `prop[i].len == 4*TD[i-1].byte@6 + 4`; `[start:end]` must not straddle `tdTotal`.
9. `totalBufferNbr > 0` and `textSection.size != 0` (verifyBAR asserts) — implied by (3)/(2)
   but enforced again with a hang-on-failure assert.
10. Read program status from command `+0x1b8` after the CSNE_CMD.

## 5. Selene (T602x 13.5) differences — informative only

- Same flags/counts/sizes offsets (`0x08…0x148`), but selene's `verifyProgram` takes section
  pointers from a trailing table: generic `+0x1c0`, text `+0x1c8`, operation `+0x1d0`,
  procedure `+0x1d8`, kernelProp `+0x1e0`, tdProp `+0x1e8` (sel `0x48d4c`–`0x48dc0`). H13
  enforces/uses the in-group `buffer` fields instead — do **not** reuse the M2 builder layout.
- Selene `verifyProgram` is 67 insns vs H13's 229; it runs generic/op/procedure/kernelProp/
  tdProp + a lighter tdprop checker (sel `0x486a0`) and **skips `verifyBAR`** entirely
  (tail branch `sel 0x48dd8`–`0x48de4`). All the BAR-vs-buffer-index discipline of §2.7 is an
  H13-only burden. (Matches H169: "H13 enforces the full sCSneCmdProgramLoad section structure
  that selene skips".)
- Selene's tdprop checker reads descriptor-size bits differently (bit 2 of descriptor word 0 →
  0x10/0x30-byte descriptors, per ProcSectionFormat notebook) vs H13's bit24-of-word@0x18 →
  0x28/0x2c TD headers. Formats are not portable between chips.

## 6. Unresolved items

1. Purpose of the 0x10-byte spare area in each group (`+0x10..+0x1f`); only the generic group's
   `+0x10` u64 is ever touched (decremented in `AddProgram`'s failure path, `0x364c4`–`0x364d8`) —
   looks like a refcount.
2. Header bytes `0x00..0x03`, `0x06..0x07`, and pad `0x1bc..0x1bf` — never read in the analyzed
   functions.
3. Generic buffer entry bytes `+0x08..+0x2f` (presumably buffer address/size consumed at bind
   time, not by these verifiers).
4. KernelProp entry `u64@+0x00` — unchecked.
5. `opType` values 1..3 semantics (all verifiers skip their bodies).
6. The `+0x158` (opDbg) and `+0x188` (proc) groups: dumped by CmdProcessor, never verified;
   content format unknown; `valid`/`count` words never read.
7. Exact semantics of command `+0x04` u16 beyond the "ProgramId" log label.
8. Status encodings at `+0x1b8` beyond "−1 = failure, 0 = success" (`AddProgram` returns
   `w20 = -1` on all failure paths, `0x36470`).
9. Quirk: TD ranges fully past `tdTotal` are walked against out-of-table bytes (§2.6.3) —
   intentional or latent bug, unknown.
10. Whether the trailing pointer table seen in selene (`+0x1c0..+0x1e8`) also exists in the
    H13 header definition (unused by H13 code) — the H13 struct is 0x1c0 bytes, so it cannot.
