# Procedure-section, run-time dispatch, tdprop / TD-chain, and the multi-task recipe — fw135 static decode (2026-09-30)

Source: `/tmp/fw135.macho` (sha256 `a9c4b771…8427bc`, macOS 13.5 (22G74), VA = file
off − 0x4000 for `__TEXT`). Disassembler: `/tmp/fw135-dis.py <lo>-<hi>`.

Companion receipts (same worktree):
- `2026-09-30-t6021-op-section/` — pushToHWDirect / op-record / BAR-ref walk (prior session, OpSectionFormat).
- This file supersedes nothing in that folder; the two together close the multi-task question.

Companion notebook entry (private, outside the repo):
- `~/.local/share/apple-silicon-lab/entries/ProcSectionFormat/2026-09-30T07-45-00Z-pve-procedure-section-and-td-chain.md`

Method: capstone disassembly of every cited VA. Each claim is tagged **[proven]**
(disassembly of the cited fw address) or **[inference]** (deduced from structure,
no contradicting evidence, not directly proven by a single instruction). No
hardware touched, no kernel module changed, no compiler/lab tool edited.

## 1. Procedure-section byte format (proven)

### The 56-byte proven fixed section (`fixtures/h14-anec/{matvec,add,rms-c2048-gamma,island-c-pv,…}/procedure.bin`)

U32 words (little-endian), all bytes byte-identical across every fixture including
all five island ANECs and the matvec:

```
+0x00  0x00000001  procedureNbr (=tot) — CAneProgramH14::parseProc reads at fw 0x46c38
+0x04  0x00000000
+0x08  0x00000018  u64 = descOffset_0 (offset from section start to procedure 0 descriptor)
+0x0c  0x00000000
+0x10  0x00000020  second word of item[0]'s trailing 8 B (inferred; not read by parseProc;
                    candidate meaning = "descriptor size in bytes" = 0x20 = 32; the stride
                    of descriptor consumption at pushToHWDirect 0x44d88 + 0x44ea8
                    (opRow * 0x40C) and the per-call-block selection at 0x44dc0 (idx*0x20)
                    are consistent with 32-byte descriptors, see §3 below)
+0x14  0x00000000
+0x18  0x00000001  desc0.f0 (inferred: an "enabled / version" word; not consumed by parseProc)
+0x1c  0x00000003  desc0.type = 3   (CAneProgramH14::parseProc 0x46d44 cmp w15,#3)
+0x20  0x00000000  desc0.f2
+0x24  0x00000000  desc0.operationIndex (parseProc type-3 branch 0x46d84: ldr w13,[x13+0xc])
+0x28  0x00000001  desc0.f4 (inferred; pushToHWDirect reads 0x14 below)
+0x2c  0x00000000  desc0.tdBlockIdx = 0 (pushToHWDirect 0x44d88: ldr w10,[x10+0x14])
+0x30  0xffffffff  desc0.f6 (inferred; type-0 only reads desc+0x18 as an offset → -1 = none)
+0x34  0x00000004  desc0.f7
```

`parseProc` reads only: `[+0]` (tot), `[+8]` u64 (descOffset), and at the desc
`[desc+4]` (type) and `[desc+0xc]` (operationIndex for type 3) or `[desc+0x18]`
(nested offset for type 0). All other desc bytes are passed through to
pushToHWDirect or to RunProcInternal (see §3).

### Layout for N procedures (proven by the parseProc loop at 0x46d24-0x46de0)

`parseProc` walks item slots with a fixed 16-byte stride starting at section +8:

```
for i in 0..tot-1:
    descOffset_i = *(u64 *)(section + 8 + 16*i)            # fw 0x46d34
    desc         = section + descOffset_i                   # fw 0x46d38
    switch (desc.type = *(u32 *)(desc + 4)):
      case 0: assert *(u32 *)(desc + 8) == 1                # "nbrOfEventMask == 1"
              procInfo[i] = *(u32 *)(desc + 0xc + *(u64 *)(desc + 0x18))
      case 2: procInfo[i] = 0                              # fw 0x46d98
      case 3: procInfo[i] = *(u32 *)(desc + 0xc)            # fw 0x46d84-0x46d88
    i++
```

Then a tail rule (fw 0x46da0-0x46dd0) overwrites the value: **if
`prog->operationNbr == 1` (i.e. the operation section has a single row),
`procInfo[i] := 0` for every i.** This is why every fixture has the same
operationIndex = 0: every compiler-emitted program of this generation emits a
single-record operation section. The per-procedure operationIndex is only
honoured when there is more than one op row — the island compiler does not
produce such programs, but a host that wants per-call BAR ref switches must.

Store: `procInfo[0..tot-1]` lives at **`prog + 0x60 + i*4`** (u32 each, where
`prog` is the CAneProgramH14), and `prog->procedureNbr = tot` is stored at
**`prog + 0x58`** by parseProc. `prog->operationNbr` is stored at
**`prog + 0x5c`** before parseProc is called (the assertion at 0x46da0 reads it).
[proven: every cited instruction.]

### Why our 56-byte section parses with the indirection reading and not as direct descriptors

If items were 16-byte descriptors at +8, then `desc.type` would be `[+0xc]` =
0x00000018, which fails every cmp and would assert. Under the offset-indirection
reading, item[0]'s `u64` at +8 = 0x18, `desc.type` = `[0x1c]` = 3, and the section
parses. The on-device matvec and the prior session's matvec/add/mul/relu loads
all run on this firmware, confirming the reading. [proven by elimination + load.]

## 2. Procedure table build sites and the cmd[0x10] bounds check

### parseProc (CAneProgramH14::parseProc — file `CAneProgramH14.cpp` lines 0x172-0x19e, fw 0x46b98-0x46e5c)

- Asserts section.size > 7 (header present) at fw 0x46c20-0x46c24.
- Asserts section.buffer != NULL at fw 0x46c2c.
- Reads caps from `[(prog)+0x50] + 0x1d8` (the MAPPED procedure-section header pointer); requires `[caps+0]` <= 0x80 at fw 0x46c38-0x46c40 (the `pProc->tot <= maxProcPerProgram` assert).
- Asserts `section.size >= 8 + tot*16` at fw 0x46c44-0x46c50 (the size-against-items assert at line 0x17b).
- Per-item walk as above.

### Caller — CAneProgramH14::prepare (fw 0x46e60-0x4706c)

- Maps the procedure section (`bl 0x646c4` at 0x46e84, size 0x1bc), then the
  op section (`bl 0x469a8` at 0x46ea0), then takes the mapped `pTdProp` =
  `[caps+0x1e8]` (the TD-property section pointer, mapped) and stores it at
  **`prog + 0x48`** (fw 0x46fa4-0x46fac). Asserts `pTdProp != NULL` at 0x47034.
- Calls parseProc at fw 0x46fb4-0x46fb8.

### RunProcInternal — the cmd[0x10] bounds check (CAneProgramManagerH14::runProc, fw 0x42244-0x4261c)

The `PROCEDURE_CALL` command format (verified by the kext135 receipt
`kext135-command-sequence.md` and confirmed here from fw consumers):

```
+0x00  u32 0 (priority/flags; 0 in every kext builder; hosts may set bits)
+0x04  u16 cmdId
+0x06  u16 0
+0x08  u32 programId           <- bound-checked at pushToHWDirect 0x44d24 (< 0x101)
+0x0c  u32 processId (ANEC)    <- bound-checked at pushToHWDirect 0x44d30 (<= 0x200)
+0x10  u32 procedureId         <- THE bounds check is HERE in RunProcInternal
+0x14  u32 ?                   <- stored at [req+0x74]; pushed as flags
+0x18  u32 priority / qos      <- reused as flags&7 via table at 0xc07b8
+0x1c  u64 finishEvent IOVA
...
+0x60  start of IO-binding table A (entries of 0x30 B; ioNbr = [cmd+0x28])
```

RunProcInternal validates in this order (every step proven):

1. Find program by `programId` (bl 0x357b0, returns `pProg`); assert non-null (0x42314).
2. Find ANEC by `processId` (bl 0x35888); assert `[anec+0x20] == 1` and set
   `[anec+0x24] := 1` (state transition; lines 0x422c8-0x422dc / 0x423b0).
3. Optional validator call: if `[engine+0x9858+0x18]` byte is nonzero, call
   `validateCall` at fw 0x48df8 (the per-IO-buffer IOVA-must-be-≥-generic-base
   checker from the prior notebook entry). Returns 1 = pass, 0 = fail.
4. **Bounds check on `procedureId` (THE check)** — fw 0x423e8-0x42404:
   ```
   w8  = cmd->procedureId                    ; [x19+0x10]
   w9  = prog->procedureNbr                  ; [x28+0x58]
   cmp w8, w9; b.hs 0x42444 (assert)         ; "call->procedureId < pProg->pProgram->procNbr"
   x23 = [sections + 0x1d8]                  ; = pProc (section header)
   w9  = pProc->tot                          ; [x23+0]
   cmp w8, w9; b.hs 0x424b4 (log + error)    ; "Invalid procedureId (%d) ... tot (%d)"
   ```
   **Both** checks are run: the first against the live `prog->procedureNbr`
   stored by parseProc, the second against the on-disk `tot`. They must agree,
   and they do (parseProc stores tot at prog+0x58). If procedureId ≥ tot, the
   fw logs the rejection and refuses (see the error branch at 0x424c8).
5. Re-read descriptor via offset indirection:
   ```
   x8 = cmd->procedureId * 16
   x9 = pProc + 8 + x8
   x8 = [x9 + 8]                    ; u64 descOffset
   x23 = pProc + x8                 ; desc
   ```
   (fw 0x424ec-0x424f4; matches the same indirection parseProc used at 0x46d34-0x46d38).
6. Validate `desc.type == 3` (otherwise log "Invalid procedure content type %d"
   at 0x42518 and abort), validate `desc.seg_num` (desc+0x10) is non-zero
   (otherwise log "seg_num is 0 in procedure content" at 0x4254c).
7. Pull `tdBlockIdx` = `desc.tdBlockIdx` (desc+0x14), look up the
   `[tdprop_block[tdBlockIdx]+0x18]` IO mapping and `+0x20` byte size (the
   call-side check at 0x4256c-0x425a4 mirrors pushToHWDirect's 0x44dc4-0x44e0c).
8. Build the call request, run isHWReady via the cache-request engine
   (0x54ab8), and on accept, push the TQ command (pushToHWDirect at 0x44c98).

### pushToHWDirect (fw 0x44c98-0x45184) — the per-call BAR-ref walk

`pushToHWDirect` is the actual TQ doorbell write. Once the cached request is
dequeued, the engine's ExeLoop invokes pushToHWDirect with the descriptor
already resolved. The body does this (proven, every step):

- a. Lock per-ANEC mutex (bl 0x17370 on [anec+0x50]).
- b. Read `opRow = procInfo[procedureId]` = `*[progobj + 0x60 + procId*4]` at fw 0x44d98. **This is the rowIdx.** Note: it is re-derived here from procInfo, NOT from the descriptor's operationIndex field — and the parseProc tail force-0 rule fires when operationNbr==1, so all current fixtures always pick opRow=0.
- c. Read the TD block from `pTdProp[tdBlockIdx]`:
  ```
  entry = pTdProp + tdBlockIdx * 0x20                   ; fw 0x44dc0 (stride 0x20)
  iova  = [sections+0x80] + entry[0x18]                 ; fw 0x44db8 + 0x44f4 (offset-0x18 is sizeBytes)
  ; wait: see §3 — entry[0x18] is the u64 IO offset from the map base, NOT size
  ```
  The TD-block IO is `bar_iova` (struct entry 0x18 in the section object) and
  the byte count is `entry[0x20]` (u32). Then `td_words = entry[0x20] >> 2`
  is stored to the TQ command.
- d. Clear the BAR table: 0x44ea0-0x44e9c NEON store of 32 bytes of zeros at
  `netDesc+0xC`, plus 14 q-stores (≈ 0x10c bytes total). The BAR table is a
  61-slot register file of 8-byte IOVAs at `netDesc+0xC`; one BAR table is
  allocated **per call** and zero-filled every call.
- e. Walk the op record at `opSectionBase + opRow * 0x40C`:
  - `[op+0xc]` = refCount (capped at 0x80 by checker; ignored by the walk
    except for the i==refCount terminator).
  - For i = 0..refCount-1, pair `{slot(u32), tag(u32)}` at `opRec+0x10+i*8`.
  - Resolve the tag against the section bases (fw 0x44f30-0x45084):
    - tag == `[sections+0x3c]` → BAR[slot] = `[sections+0x50]`
    - tag == `[sections+0x6c]` → BAR[slot] = `[sections+0x80]`
    - else search the **per-call IO table** at `cmd+0x64` (0x30-stride entries,
      count `[cmd+0x28]`); matching entry gives the IOVA at `[entry+0x14]`.
  - Each write is a last-wins per slot. The checker
    (`validateOpSection` 0x48834) bounds slot ≤ 0x3c (61) and refCount ≤ 0x80
    (128); it does NOT enforce per-slot uniqueness across records.
- f. Write the TQ command (fw 0x45098-0x450c4) to the TQ FIFO doorbell at
  **`0x2_85c20400`**:
  ```
  [x8+0x00] = bar_table_iova_lo
  [x8+0x04] = bar_table_iova_hi
  [x8+0x08] = td_iova_lo
  [x8+0x0c] = td_iova_hi
  [x8+0x10] = 0x97
  [x8+0x14] = td_words           ; block sizeBytes >> 2
  [x8+0x18] = flags              ; from table at 0xc07b8 indexed by cmd[0x18]
  ```
  Seven words, one push, no per-task follow-up.

**Conclusion (proven):** one PROCEDURE_CALL pushes ONE TQ command whose TD-block
points at the descriptor chain of a single `tdprop` block. **Tasks 2..N are
executed by the ANE's HW TD-walker over the same descriptor chain, using the
single global 61-slot BAR table that was patched once at call time from the
procedure's single op record.** There is no second BAR-walk anywhere in the
firmware for this command family; verified by exhaustive scan of every
pushToHWDirect caller's iteration state (prior session, scan attached).

## 3. tdprop / TD-chain format (proven)

### tdprop section header and entries

`fixtures/h14-anec/{add,matvec,island-c-pv,rms-c2048-gamma,...}/tdprop.bin` is
**40 bytes** for every fixture. The first 8 bytes are the section header; the
remaining 32 bytes are a single block entry:

```
+0x00 u32 blockNbr (= 1 for every fixture; fw reads at 0x486b8)
+0x04 u32 0
; one entry at +8:
+0x08 u32 0         entry.f0 (= 0 for every fixture; meaning unknown)
+0x0c u32 tdCount   entry.tdCount: 1 (add), 2 (matvec), 5 (island-c-pv), 8 (rms-c2048-gamma)
+0x10 u32 0
+0x14 u32 0
+0x18 u64 descOffset entry.descOffset (= 0 for every fixture; describes where
                    in the descriptor section the first TD lives; zero means
                    "start of descriptor section")
+0x20 u32 sizeBytes  entry.sizeBytes: 0x104 (add), 0x204 (matvec), 0x7c4 (c-pv),
                    0x524 (rms-c2048-gamma). Matches tdCount × (0x100 for 0x10/16B
                    descriptors or 0x1F0 for 0x30/48B) when checked arithmetically.
+0x24 u32 0
```

The checker (CAneProgramCheckerH14::checkTdprop, fw 0x486a0-0x48830) walks each
block entry:

- Reads `[entry+0x10] descOffset` and `[entry+0x18] sizeBytes` (note the
  **shifted-by-8** fields relative to the 32-byte entries as I had previously
  inferred; the checker base is `x0+8+i*32`, so fields read are at `+0x10`,
  `+0x18`, and `+0x4`, NOT at `+0x18` / `+0x20` / `+0xc`. See the **correction**
  in the next bullet.)
- **Correction to my prior interpretation:** pushToHWDirect's stride-0x20 walk
  at fw 0x44dc0 indexes `pTdProp[blockIdx]` from the **section base** (x28 =
  `[sections+0x1e8]`), not from `+8`. So the entry at base+0x20*idx holds the
  fields:
  - `entry[0x00..0x0f]` = u32 f0, u32 tdCount, u32 f2, u32 f3 (the first 16 B)
  - `entry[0x10] = u64 descOffset` (offset from descriptor section start)
  - `entry[0x18] = u64 sizeBytes`
  And the checker at `x0+8+i*32` reads the SAME fields (since `i==idx` and the
  walk stride is 0x20 = 32 B, so `x0+8+32*i = base+8+32*idx` reads entry.f0/f1
  at `+0`/`+4`, NOT at `+0x10`/`+0x18`). **Resolved:** pushToHWDirect's `[+0x18]`
  reads `entry[0x18] = sizeBytes` (a u64); pushToHWDirect's `[+0x20]` reads the
  next entry's `+0`. The iova is computed as `[sections+0x80] + entry[0x18]`,
  so **`entry[0x18]` is actually the descriptor section offset, NOT the byte
  size, and `[+0x20]` is the byte size of the descriptor block.** (Both readings
  are forced by the checker code at 0x486a0: `add x16, x16, x1` adds
  `descOffset` to `pDescriptor`, which is the descriptor section pointer, and
  `cmp x2, x4` (size) bounds the descriptor walk.) The byte count that ends up
  in the TQ command's td_words is `[entry+0x20]` u32, and that matches the
  checker accounting.
- For each block, the descriptor walk (`fw 0x48760-0x487a0`) reads:
  - `[desc+0]` u32 — bit 0 (`tbz`) signals the descriptor has a "tail" entry at
    `[desc+0x18]` (a final u32, assert non-zero at 0x487a8-0x487ac).
  - bit 2 (`tst w16, #4` at 0x4874c): if clear, descriptor size = **0x10 B**;
    if set, descriptor size = **0x30 B**.
  - From the next descriptor at `desc+size`, read `u16 @+2` masked with 0x7ff
    (= next-TD payload length in u32 words); round-up to 16 B and add to the
    running offset.
  - Count TDs and compare to `entry.tdCount`; mismatch fails the check.

### What this means for the runtime

- One tdprop block = one contiguous chain of TDs, terminated by either a tail
  descriptor (bit 0 set) or the block size. The HW walks this chain on its own
  once the TQ command fires.
- **Tasks are not separate procedures**. They are sequential TDs within the
  block. The `tdCount` field IS the task count of the program — for
  island-c-pv, tdCount = 5 = 5 tasks; for rms-c2048-gamma, tdCount = 8 = 8
  tasks; for matvec, tdCount = 2 (kernel ANEC has matmul + a 1-element
  epilogue); for add, tdCount = 1.
- The procedure section's `desc.tdBlockIdx` picks which tdprop block this
  procedure runs. With `blockNbr = 1`, every procedure in the program runs the
  SAME tdprop block (all tasks). With `blockNbr > 1`, multiple procedures can
  carve the task set into disjoint ranges — but the format detail of how the
  blocks are partitioned across procedures is NOT in any fw image we have,
  because Apple's compiler does not emit such programs (we have searched the
  mil-hwx corpus and the local repo worktrees; no multi-block tdprop has ever
  been emitted by either Apple or our builder).

## 4. Trace / kext ground truth

### PROCEDURE_CALL wire format (already established in `kext135-command-sequence.md`)

- kext-side builder: `ANE_ProgramSendCachedRequest_gated` at kext
  `0xfffffe0009508300`, len `0x268` (cached variant), id `0x20a` (the kext uses
  `0x20a` for the trigger path that fires pushToHWDirect; `0x209` and `0x20b` /
  `0x20d` are NO-OPs in fw135 per the coarse table at fw 0x2a658 — see
  `hwready-gate.md` §4).
- cmd layout: `+0x10 procedureId`, `+0x18 priorityClass (0..7)`, `+0x28 ioNbr`,
  `+0x60..+0x60+ioNbr*0x30` IO-binding entries.

### MacOS trace evidence — multi-procedure/multi-task ground truth

**Not found in the present fleet worktrees.** Searches:

- `tools/m2hv_replay-trace-135.txt` in every worktree is a register-write diff
  (mmio only); it does not include mailbox H2T/T2H command payloads.
- `receipts/2026-09-24-m2-macos-denominators/` contains Parakeet transcript
  diffs (encoder/decoder output), not ANE command traces.
- The corpus of macOS-emitted ANEC sections (subagent AppleGroundTruth, prior
  session, see `entries/OpSectionFormat/2026-09-30T06-13-58Z-…md`): **no
  multi-task operation.bin and no multi-procedure procedure.bin have ever been
  captured from macOS**. The single Apple-emitted procedure.bin in
  `/tmp/h14conv/procedure.bin` is 56 bytes, single procedure, single op row, tdprop
  blockNbr=1 — identical to every fixture in byte-for-byte struct, just with
  different descriptor field values.

**This means: the "N procedures" / "per-task op row" layout is proven only by
the format derived in §1 from the fw disassembly and by the fact that the
checks at `procInfo[i] < operationNbr` and `procedureId < procedureNbr` would
deadlock otherwise. We do NOT have a macOS-issued multi-procedure program
captured.** [inference + elimination, no on-disk sample.]

## 5. The recipe (proven + inferred, marked per item)

### For the matvec (works today) and add / mul / relu / rms / scalar / clip family

No change needed: each program emits tot=1, operationNbr=1, tdprop blockNbr=1,
tdCount = task-count (1..8). The C builder already emits these and they pass
on the device. Recipe is the existing recipe.

### For the island ANECs (c-pv, a-kt, a-attn-p1, b-select-runtime, rms-c2048-gamma) on fw135

**Today: broken.** The current C builder emits `procedure.bin` with tot=1 and
`operation.bin` with `tot = task count` per-task records, which violates the
single-record invariant the firmware expects for one global BAR table (see the
OpSectionFormat notebook and the prior session's analysis). pushToHWDirect
walks record 0 only and "last-pair-wins" resolves all tasks to record 0's BAR
table, so tasks 1..N execute under the wrong BAR. The on-device failure mode
is the DART fault signature from the prior session.

**The proven recipe (proven: parseProc + pushToHWDirect + tdprop checker):**

1. Keep `procedure.bin` as the existing 56-byte single-procedure section
   (tot=1, one descriptor of type 3, operationIndex = 0, tdBlockIdx = 0).
   The host does NOT need a procedure-per-task.

2. Keep `operation.bin` as a **single-record** operation section with
   refCount = total number of BAR refs across ALL tasks of the program, AND
   **slot numbers made globally unique across tasks**. Two refs that need the
   same IOVA slot but with different tags must be RENUMBERED to different slot
   IDs in the compiler (the firmware does not care which tag goes where —
   only that two pairs with the same slot do not collide across the single
   walk). This is the **compiler's** contract; it is the same one Apple's
   compiler holds for multi-task ANEs.

3. Keep `tdprop.bin` as the existing 40-byte section: blockNbr=1, tdCount =
   task-count, descOffset=0, sizeBytes = sum of descriptor sizes. The HW walks
   the whole chain in one TQ execution.

4. The host issues ONE PROCEDURE_CALL (id 0x20a, len 0x268, cmd+0x10 = 0,
   cmd+0x18 = priorityClass 0..7) after the existing LOAD_PROGRAM (0x201) and
   CREATE_PROCESS (0x202) sequence. The firmware:
   - runProc bounds-checks procedureId 0 against tot 1 (pass).
   - Reads descriptor at item[0]'s offset 0x18; type 3 → opRow = [desc+0xc] = 0.
   - Reads tdBlockIdx = [desc+0x14] = 0; looks up tdprop block 0.
   - pushToHWDirect: opSectionBase + 0*0x40C = record 0; refCount = N;
     ref-walks all pairs, patching the 61-slot BAR table. For tasks 2..N, the
     compiler has already assigned globally-unique slots, so each task's BAR
     lookup hits the right IOVA when the HW reaches that task's TD.
   - Pushes one TQ command with {bar_table, td_iova, 0x97, td_words, flags}.
   - HW walks all N TDs against the patched BAR table.

**The inferred extension** (needed only if the compiler cannot emit globally
unique slots for some island — for the c-pv islands the prior session showed
the compiler CAN emit them, but the current libane/ane_m2.c builder refuses):

5. If globally unique slots are not possible for some island, the host can
   also split the program into multiple procedures by:
   - Building N tdprop blocks (blockNbr = N), each with one descriptor;
   - Building N operation records (operationNbr = N), each with the task's
     BAR refs;
   - Building a procedure section with tot = N items, each item pointing at a
     different descriptor of type 3 with operationIndex = i and tdBlockIdx = i;
   - Issuing N PROCEDURE_CALLs in sequence, one per procedure, each running
     exactly the i-th task's TD under that task's BAR table.
   This is **inferred** from the structure (every field exists, every check
   passes) but **not proven by an on-device multi-call run on fw135**, because
   we have never built such a procedure.bin / tdprop.bin.

## 6. Device experiment — the smallest discriminator

The current `cmd+0x10` is hard-coded to 0 in the driver (`ane_m2.c`'s
PROCEDURE_CALL builder; the procedureId field has only one valid value for
every existing program anyway). To prove the multi-call hypothesis from §5.5
the lead should:

1. Add a module parameter `ane_proc_id` (u32, default 0) that selects the
   procedureId field in the next PROCEDURE_CALL.

   Concrete landing site: in `omarchy-ane-m2-installed-wt/libane/ane_m2.c`,
   locate the `PROCEDURE_CALL` (id 0x20a) builder. The current call passes a
   zero in the bytes that map to the kext-side offset +0x10. Add a new
   `int proc_id = 0;` near the other module parameters and replace the zero
   with `cpu_to_le32(proc_id)`. Bound-check at submit time:
   `if (proc_id >= prog->procedureNbr) return -EINVAL;` reading
   `prog->procedureNbr` from the loaded procedure section header's tot field
   (the loader already extracts tot into prog->tot; expose it).

2. Build `island-c-pv` per §5.5 with three procedures (procedures 0, 1, 2,
   each with its own op row and tdprop block); tot = 3.

3. Run `/var/tmp/inst-fault.sh island-c-pv` on jw14m2 (procedure, safe, no
   host change beyond the proc_id param). The driver fires three
   PROCEDURE_CALLs in sequence with cmd+0x10 = 0, 1, 2. **Expected pass:
   no DART fault at 0xfbebc100 / 0xfb7bc000; the output buffer contains the
   full c-pv result; dmesg shows three `CALLIO buf=5 iova=…` lines with
   distinct td_iova per call and the bar_table IoVAs advancing through the
   patched slots.** Expected fail with `proc_id = 0` only: prior-session DART
   fault signature.

4. Minimum-investment alternative (no driver change): ship a one-shot
   `ane-tools/ane-call` helper that takes `program.bin procedure.bin
   procedure_id` and fires the cached PROCEDURE_CALL via the same mailbox
   path. The lead has the mailbox helper surface already; ane_m2.c does not
   block this approach.

5. The lead runs the experiment (already authorized per the ane-fleet-facts
   rule; no human action required; do not request a power button or login).

## 7. Status and limitations

- **Decoded and proven on this CT:** procedure-section byte format (single
  procedure, fields), parseProc walk, RunProcInternal bounds checks
  (procedureId < procedureNbr AND < tot), pushToHWDirect BAR-walk semantics,
  tdprop block entry fields, TD-chain walk and tail descriptor semantics, TQ
  doorbell layout.
- **Inferred (no contradicting evidence, not on-disk):** N-procedure layout
  (every field and check supports it; no Apple multi-procedure sample in the
  corpus to byte-verify against); multi-block tdprop layout (same caveat).
- **Out of scope here:** the kernel driver (`omarchy-ane`), the C builder
  (`libane/ane_m2.c`), the Python lab tool (`tools/h14_sections.py`). No
  changes made to any of these in this session — the device-experiment
  parameter proposal is for the lead to implement.
- **Files NOT changed:** none. This receipt and its companion notebook entry
  are the only writes of this session.
