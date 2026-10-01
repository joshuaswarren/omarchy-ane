# H13 kext (AppleH11ANEInterface 13.5 22G74) — `sCSneCmdProgramLoad` fill-side map

Companion to `2026-10-03-h13-progload-spec.md` (firmware verifiers). This documents the **kext
side**: where the 0x1c0 command is built from a loaded HWX, which HWX object feeds each of the
nine 0x30-byte groups, how counts/sizes are computed, and how buffer pointers (IOVAs in the
firmware DART space) are allocated. Every claim is marked **VERIFIED** (disassembly read at the
cited kext VA) or **INFERENCE**.

- Kext source: member `com.apple.driver.AppleH11ANEInterface` of
  `/var/tmp/t6021-kc/kernelcache.t6020.13.5-22G74.macho`
  (sha256 `9615a486511c7a60b141d7f4291361c5212e908546d6568890029bb90b5431e7`), parsed with
  `artifacts/M2StaticDecode/2026-09-28-kext135-command-sequence/ane135.py`. 4064 symbols, full
  mangled names; all `ZinCompute*` names below are symbol-verified.
- Provenance caveat: the member comes from a **T6020** 13.5 cache. The same 22G74 OS ships this
  kext on T8103; chip selection is IORegistry data (`aneType`), not code bytes. Byte-identity
  with the T8103-shipped kext not proven [assumption].
- All VAs are kext VM addresses as loaded (`__TEXT_EXEC` base `0xfffffe00094b76b0`).
- Tool note: capstone stops at embedded data mid-segment; decode per function window
  (FUNC_STARTS), never whole-segment. Register-doubt sites were re-checked from raw bytes
  (`0x9529d10` madd / `0x9529d00` mov w28 — both confirmed).
- Offline static analysis; no fleet machine touched. Notebook thread:
  `entries/jwm1-parity/20261001T031000Z-jwm1-ane-h13-kext-progload-map.md`; disassembly dumps:
  `artifacts/jwm1-parity/h13-kext-progload-map/`.

## 0. Wire facts (corrects two items in the H169 spec)

- LOAD command id is **0x200** (u16 @ cmd+4), written by `ZinComputeInitSneProgram`
  `0xfffffe0009529584`–`0x9529588` (`strh w9,#0x200,[x8,#4]`). VERIFIED.
- UNLOAD = **0x203** (`ProgramUnload` `0xfffffe00094d1610`), RELEASE = **0x201** len 0xc
  (`ReleaseProgramMemoryBuffer` `0xfffffe00094c0830`–`0x94c0844`). VERIFIED. The
  kext135-command-sequence note "0x201 = LOAD/UNLOAD" and "LOAD +8 = programId" were wrong; the
  programId is **returned by the firmware in the status word** `+0x1b8` and the kext stores it:
  ProgramLoad `0xfffffe00094d1c8c`–`0x94d1c90` (`ldr w8,[x20,#0x248]; str w8,[x20,#0x38]`,
  where params+0x248 = cmd+0x1b8, params+0x38 = the programId field reused by every later
  command). H169 spec §6 item 7 ("semantics of +0x04") is thereby resolved: it is the command
  id; the firmware's `ProgramId = %d` log prints this id (512 for every kext load).
- The 0x1c0 command lives at `H11ANEProgramBufferParamsStruct+0x90`; `ProgramLoad` sends
  `aneCmdSend(this, params+0x90, 0x1c0, …, w7=2)` at `0xfffffe00094d1c3c`–`0x94d1c68`
  (pre-index `strb #1,[x21,#0x88]!` = load-pending flag at params+0x88). VERIFIED.
  `ANE_ProgramCreate_gated` inlines the same send after `memcpy(params+0x90, cmd, 0x1c0)`
  `0xfffffe00094fae48`–`0x94fae88`. VERIFIED.

## 1. Fill flow (`H11ANEIn::ANE_ProgramCreate_gated` `0xfffffe00094f6ff4`)

1. `ANE_ProgramCreatePreprocessing` `0xfffffe00094f5de0` — power-on, kalloc of the
   ZinComputeProgram context (`0x94f5f88`), HWX parse via the embedded `ZinCompute*` library:
   `ZinComputeProgramMake` `0xfffffe000952442c` (+`MakePreCheck`/`MakeRelocations`
   `0x95259b0`/`MakeFvmlibs` `0x9525d44`/`MakeOperations` `0x952605c`/`MakeBindings`
   `0x9526288`/`MakeProcedures` `0x9526648`). The HWX is a Mach-O-family container (magic
   `0xBEEFFACE`, LC_SEGMENT_64 cmd 0x19 + custom cmd 0x4 records); the kext walks it as
   segments → symbols (stride 0x28, symbol tag byte @+0x40, tag ∈ 0x20..0x2b).
2. `ZinComputeInitSneProgram(prog, &wrapper)` **first pass** `0xfffffe00094f7164` — skeleton
   into a kalloc'd 0x1c0 `ZinComputeProgramSne` (= the command; type view `0x7ce5280`).
   Computes all section sizes, writes id 0x200, group defaults.
3. One shared-memory surface for the four generated section blobs:
   size = Σ align_up(generic, op, procedure, tdProp sizes, A) with A = `[this+0x33e8]`
   (page alignment constant, overflow-checked < 2^32) — `0xfffffe00094f7168`–`0x94f71d4`;
   `AllocateSharedMemorySurface(this, size, &handle, 1, 'GORP', 1, 1, 0)` at
   `0xfffffe00094f7268`. VERIFIED.
4. Program-image surface (the HWX segments copy) is DART-mapped:
   `dartMapMemoryDescriptorForProgram` `0xfffffe00094f76e0`, output IOVA stored at
   **params+0x8**; DMACommand at params+0x20; surface handle at params+0x30 (`0x94f768c`).
   Segments flagged in HWX (`[seg+0x3c]` bit0 set, bit1 clear) are additionally pushed to the fw
   by `ZinAneLoadProgramSegment` `0xfffffe00094f77a8`. VERIFIED at cited VAs; exact semantics of
   the fw push = INFERENCE.
5. Mutable/buffer-class symbols (tag class 0x3f1) each get their own surface:
   size = align_up(sym.size, A); `AllocateSharedMemorySurface(..., 'KTUM', …)` `0x94f7a2c`;
   symbol bytes copied in `0x94f7b34`–`0x94f7b4c`; slot recorded at params+0xa240+i*8. VERIFIED.
6. `ZinComputeInitSneProgram` **second pass** `0xfffffe00094f7c10` — real command with mapped
   addresses; then header pass `InitGenericSectionHeader` `0x94f7cf8`,
   `InitProcedureSectionHeader` `0x94f7d0c`, `InitOperationSectionHeader` `0x94f7d20`,
   `InitTextPropertySectionHeader` `0x94f7d34`; content pass `InitGenericSection` `0x94f7d90`,
   `InitOperationSection` `0x94f7e40`, `InitProcedureSection` `0x94f7e8c`,
   `InitTextPropertySection` `0x94f7edc` — all four blobs written into the one surface at the
   computed sub-offsets (kernel VA of blob surface = `[sp+0x130]`, captured `0x94f76c0`).
7. `memcpy(params+0x90, cmd, 0x1c0)` + send (`0x94fae40`–`0x94fae88`), status → programId, then
   `ANE_ProcessCreate_gated` `0x94fb294`.

## 2. Group mapping table

Group offsets are `sCSneCmdProgramLoad` offsets (fw-side names from the H169 spec). "Symbol
type" = byte at HWX symbol+0x40 → `ZinComputeGetBufferType` table `0xfffffe00073a0408` →
type word → index table `0xfffffe00073a0488`.

| # | group (fw name) | base | filled by (kext) | HWX source | count (+4) | size (+0x20) | buffer (+0x18) | status |
|---|---|---|---|---|---|---|---|---|
| 0 | genericSection | `0x08` | `ZinComputeInitGenericSection` `0xfffffe0009529c70` (+Header `0x9529f60`) | every segment/symbol whose type ∈ {0x3ee,0x3ef,0x3f0} (tags 0x20–0x25) — weights/input/output buffers; entry index = running count, entry bufferIndex from table {0x3ee→0, 0x3ef→1, 0x3f0→2, 0x3f1→5} | `totalBufferNbr` = number of qualifying symbols (`w24`, `0x9529dc0`); stored @+0x204 of the blob | blob: `align8(0x208 + n*0x30)` (`0x9529ed4`–`0x9529ee0`); group: same value (must match, else `'buffer->size=%llu, expected_size=%llu'` fail `0x9529ee4`–`0x9529f0c`) | blob kernel ptr = wrapper+0x10, device ptr = wrapper+0x18 (`0x9529f00`) — the 'GORP' surface IOVA + generic sub-offset | VERIFIED (VAs cited) |
| 1 | kernelSection | `0x38` | `ZinComputeInitSneProgram` segment walk `0xfffffe000952962c`–`0x952963c` | symbol type 0x3e9 (tags 0x26/0x27) — the kernel binary blob; buffer = segDevBase + sym.vmaddr − seg.vmaddr (`0x9529670`–`0x9529690`); spare[0] = 3, spare[1] = (segname=="__L3"), spare[2] = 1 (`0x9529638`,`0x95296ac`–`0x95296bc`) | global symbol ordinal: Σ symbol-counts of preceding segments + index within segment (`ZinComputeLookupBufferBySection` `0x952bc0c`–`0x952bc7c`) | `[sym+0x28]` (`0x9529694`–`0x9529698`) | program-image surface IOVA + symbol offset (step 4) | VERIFIED |
| 2 | textSection (TD blob) | `0x68` | same walk, TEXT branch `0xfffffe0009529614`–`0x9529628` | symbol type 0x3e8 (tag 0x28) = `__TEXT/__text` task-descriptor blob; spare[0] = 4 | same ordinal rule | `[sym+0x28]` | program-image surface IOVA + symbol offset | VERIFIED |
| 3 | operationSection | `0x98` | `ZinComputeInitOperationSection` `0xfffffe000952a0f8` (+Header `0x952a3ec`) | procedures × their operations (`MakeOperations` output); tot = `[prog+0x58]`, guarded `≤ 0x80` at `0x9529804`–`0x952980c` | `tot` (constant; group count = tot) | threadFlavor==0: `4 \| (tot*17 << 4)` = 4 + tot*0x110 (bitfield trick `0x95297c8`–`0x95297f0`); flavor4: `procs*0x40c + 4`; then align8 (`0x95297f4`) | 'GORP' surface IOVA + op sub-offset | VERIFIED |
| 4 | procedureSection | `0xc8` | `ZinComputeInitProcedureSection` `0xfffffe000952a754` (+Header `0x952ad40`) | per-procedure blobs (MakeProcedures); per-proc symbol TD count from `[prog+0x60]` array stride 0x40 | `procs` | flavor0: `8 + Σ(0xdc*tds + 0x40)` (`0x95298c8`–`0x9529904`); round-up-to-8 `0x9529910`–`0x9529920` | 'GORP' surface IOVA + proc sub-offset | VERIFIED (size); entry content layout from fw side |
| 5 | kernelPropSection | `0xf8` | **none** — whole-kext store-scan for offsets 0xf8/0xfc/0x110/0x118 found only adrp literal-pool false positives | never populated by the 13.5 kext create path (optional per fw: only checked `if valid`) | 0 | 0 | 0 | VERIFIED by absence (scan over all 1148 functions, immediate-offset stores) |
| 6 | tdPropSection (textProp) | `0x128` | threadFlavor==0: `ZinComputeInitSneProgramTdPropertyBuf` `0xfffffe0009529a98`; flavor4: `ZinComputeInitSneProgramSegmentPropertyBuf` `0xfffffe00095299ec` (both write group 0x128 — the dispatch is the flavor==4 branch `0x952996c`–`0x95299b0`) | TD directory derived from per-procedure `ZinComputeProgramAneOperationTdCount` `0x952bf14` | (fw never reads it for this group) | `align8(4 + Σ_procs tdCount*0x30)` (`0x9529b10`–`0x9529b5c`), overflow-checked | 'GORP' surface IOVA + tdprop sub-offset; spare template {type,4,0,1} from `0x73a03e0` | VERIFIED |
| 7 | opDbg | `0x158` | none found (same scan; offsets 0x158/0x15c/0x178 only hit adrp pools) | never populated in this path (fw only dumps it) | 0 | 0 | 0 | VERIFIED by absence |
| 8 | "proc" | `0x188` | none found | never populated | 0 | 0 | 0 | VERIFIED by absence |

Group header write pattern (all kext-filled groups): clear valid bit0 (`and w,#~1` / template),
then 16-byte spare template `stur q0` — generic template `{0x3ea,4,0,1}` @`0x73a03b0`,
operation `{0x3eb,4,0,1}` @`0x73a03c0`, procedure `{0x3ec,4,0,1}` @`0x73a03d0` (`0x9529708`–
`0x9529714`, `0x95297bc`–`0x95297c4`, `0x9529884`–`0x952989c`). VERIFIED. The spare templates
put the section's *type word* in spare[0] — unverified by fw, but consistent with the
"bufferIndex id space" the BAR cross-check implies ({0,1,2} generic, 3 kernel, 4 text, 5
mutable).

## 3. Buffers, IOVAs, alignment

- Two allocation classes per program, both via `H11ANEIn::AllocateSharedMemorySurface`
  `0xfffffe00094bdb24` (IOMemoryDescriptor + IODMACommand under the hood; DART-mapped by
  `dartMapMemoryDescriptorForProgram` `0xfffffe00094bd288`, which also implements
  `FreeUpDartSpace` reclamation on unload `0xfffffe00094d119c`). VERIFIED (symbols + call sites).
- The section blobs live in ONE surface; the program image (HWX segments incl. text/kernel) in
  a second; each mutable buffer in its own. Group `buffer` fields = surface IOVA (+ sub-offset
  for the four blobs); text/kernel buffers point *into the program image* at
  `segDevBase + sym.vmaddr − seg.vmaddr` (`0x9529688`–`0x9529690`). VERIFIED.
- Alignment: all four blob sizes aligned to 8 bytes (`ZinComputeProgramAlign(x,8)`), then each
  rounded up to the page constant `[this+0x33e8]` for the shared allocation and laid out
  back-to-back at the rounded offsets (`0x94f7168`–`0x94f71d4`); total allocation
  overflow-checked against 2^32 (`0x94f71cc`–`0x94f71d8`). Mutable surfaces sized
  `align_up(sym.size, [this+0x33e8])` (`0x94f79e0`–`0x94f79f0`). VERIFIED. The value of the
  page constant at runtime = INFERENCE (vmaddr evidence below says 0x4000).
- The generic blob's `entry.size` = `[sym+0x28]` and the fw boundary check
  `buffer+size ≤ 0xE0000000` matches the DART window: the real H13 HWX below places surfaces at
  0x30000000/0x30004000/0x30008000 — i.e. **HWX vmaddrs are the firmware-side IOVAs** and the
  kext maps the surfaces at those addresses. The kext itself never checks the 0xE0000000
  bound; only the firmware does. VERIFIED (kext side); the identity "surface IOVA == HWX
  vmaddr" = INFERENCE (strongly suggested by the vmaddr choice and the boundary constant; not
  re-derived from IOMapper internals).

## 4. Cross-check against a real H13 HWX

`receipts/fixtures/mil-oneop/model.hwx` (32 KiB, magic `CE FA EF BE`, arch subtype 4 = H13,
inspected with `mil-hwx-compiler/research/inspect_hwx.py`):

- `__FVMLIB/__const` addr 0x30000000 size 0x400 (input t1), `__FVMLIB/__data` addr 0x30004000
  size 0x400 (output t2), `__TEXT/__text` addr 0x30008000 size 0x1f8 (TD blob, 1 task, 126
  words), `__TEXT/__const` addr 0x30008200 size 0x400 (constants), program descriptor
  `'main'` `text=0x30008000 text_const=0x30008200`, tensor descriptors binding 1/2.
- Predicted PROG_LOAD: generic blob 0x208 + 2*0x30 = 0x268 → align8 0x268; two buffer entries
  {bufferIndex 0 (or 0/5), sizes 0x400/0x400}; textSection = 0x30008000-based IOVA, size 0x1f8,
  count = text symbol's global ordinal; operation size 4 + 1*0x110 = 0x114 (1 procedure, 1
  task); procedure size 8 + 0xdc*tds + 0x40; tdProp size align8(4 + tds*0x30). All satisfy the
  H13 firmware rules (§4 checklist of the H169 spec): `maxAneUsed=1` (kext writes constant 1,
  `0x9529eb8`–`0x9529ebc`), `totalBufferNbr` 1..0x200 ✓, bufferIndex < 6 ✓, `tot ≤ 0x80` ✓,
  sizes match `4+tot*0x110` / `4+tdTotal*0x30` ✓, all `buffer+size ≤ 0xE0000000` ✓
  (max 0x30008400). No conflict found between kext-builder rules and fixture content.
- Not cross-checked (needs a decoded H13 TD dump from the fixture beyond the inspector's
  task-walk): the exact tdProp `nextIdx` chain words the firmware's `verifyDescriptors`
  enforces — our compiler's `h13_td.py` already walks the same task records, so the chain is
  compiler-guaranteed, but I did not re-derive fw TD chain fields from this fixture byte-for-byte
  [INFERENCE for the chain, VERIFIED for sizes/presence].

## 5. Minimal inputs for a Linux PROG_LOAD packer (T8103, 13.5 fw)

1. Parse HWX (0xBEEFFACE container): segments/symbols, `__TEXT/__text` TD blob + task
   descriptor, `__FVMLIB` surfaces, program descriptor, tensor bindings.
2. Build four blobs: generic (`0x208 + n*0x30`, entries {valid=1, ordinal, bufferIndex∈
   {0,1,2,5}, isL3, flags, pad, 0, size, 0xffff}), operation (`4 + tot*0x110`, tot ≥ 1),
   procedure (`8 + Σ(0xdc*tds + 0x40)`, align8), tdProp (`4 + Σ tds*0x30`, align8); kernelProp
   stays absent.
3. Lay the four blobs back-to-back, each rounded up to the page constant (0x4000 per HWX
   vmaddr evidence), in one surface; map program image + blob surface + one surface per mutable
   buffer into the ANE DART at HWX vmaddr-equivalent IOVAs (< 0xE0000000).
4. Command: u32@0 = 1, u16@4 = 0x200, groups per table (spare templates included), send len
   0x1c0, read programId from status +0x1b8 (negative = fail). `verifyProgramSection` boundary
   always runs; `verifyProgram` content checks are gated on a fw manager flag (H169 §1.2) —
   build to satisfy both anyway.
5. Segments with descriptor flag bit0 set additionally go to the fw through the mcache/load
   segment path (`ZinAneLoadProgramSegment`) — replicate only if the no-fw-copy bring-up path
   needs it [INFERENCE].

## 6. Unresolved items (kext side)

1. Value behind `[this+0x33e8]` (page constant) — expected 0x4000 from HWX vmaddr spacing;
   confirm from a live kext instance.
2. Where the HWX bytes enter kernel memory (userspace surface handle in
   `H11ANEProgramCreateArgsStruct+0x10/0x18` → `wireIOSurface`?) — flow verified only down to
   the parsed program; the exact user-client handoff not decoded.
3. `[seg+0x3c]` flag semantics (bit0 = "load to fw via mcache", bit1 = "counted differently in
   ordinals") — inferred from both use sites; not proven.
4. Contents of `ZinComputeInitProcedureSection` entries beyond what the fw verifier reads
   (procedure discriminator blobs) — decoded on the fw side, kext writer body not fully read.
5. The `'KTUM'`/`'GORP'` fourcc tags' meaning (allocation-class bookkeeping only, assumed).
6. Whether the flavor-4 (`ANEThreadFlavor==4`) variants are reachable on T8103 at all (T8103
   runtime flavor appears to be 0; the flavor gates op/proc size formulas and prop-buf choice).
7. HWX container commands beyond LC_SEGMENT_64 / cmd 0x4 (e.g. cmd 0x8 kind 0x434F4741, cmd
   0x31) — parsed by `ZinComputeProgramMake` but not mapped to any PROG_LOAD field here.
8. Byte-identity of this kext member with the T8103-shipped 22G74 kext (provenance caveat §0).
