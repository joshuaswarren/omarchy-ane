# 2026-11-22: the smallest program shape to try on the M6, and why (lane w7S, Q3)

Queue item: matvec programs assert in `CAneProgramManager.cpp` or never finish; bigger
elementwise programs abort at `ExecLoopHx.cpp:1847`. From the firmware logs and the H18
template code, propose the smallest program shape that passes, as a patch plus test.

This is a proposal, not a measured result. Nothing here has run on the M6.

## Known state on the M6 (T8152)

- `add` `[1,1,128,128]` fp16 elementwise: proven. 100/100 PROCEDURE_CALLs on the proven
  build, and 1000/1000 loaded-once calls on 2026-11-21 (receipt
  `2026-11-21-h18-m6-reuse`, median 548 us/call, digests equal).
- `matvec` `y[1,256] = x[1,256] . W[256,256]` fp16: fails. Two observed modes —
  `CAneProgramManager.cpp` assert, or PROCEDURE_CALL acknowledged with no finish event,
  unload unanswered, domain power cycle leaves the core un-stopped.
- bigger elementwise programs: abort at `ExecLoopHx.cpp:1847`.
- Firmware: RTKit 3514.0.15 (`adc release`, `__j8xx__`, VM mode dynamic). The captured
  fw log (receipt `2026-11-18-ane-h18-fixediova`, 56 records) ends at START PROCESSED;
  the assert text lives in the firmware's own ring, not the kernel log.

## Structural decode of the fixture set (packed by tools/ane-prog-pack)

ANEC header fields: +0x0c td-block count, +0x10 descriptor size, +0x18 kernel(const)
size, +0x20 input count, +0x24 output count, +0x38/+0x3c task counts. tdprop word 3
repeats the td-block count; word 8 repeats the descriptor size.

| fixture | pack B (no inputs) | kernel B | td blocks | inputs | surfaces | sec2 |
|---|---|---|---|---|---|---|
| **clip-high** | **2,432** | **128** | 1 | 1 | 2x 0x8000 | ~0x80 |
| real-div-scalar | 4,480 | 2,048 | 1 | 1 | 2x 0x8000 | 0x800 |
| rms-c2048-gamma | 11,712 | 8,320 | **8** | 1 | **2x 0x20000** | 0x2080 |
| relu | 18,656 | 16,384 | 1 | 1 | 2x 0x8000 | 0x4000 |
| add (PROVEN) | 18,800 | 16,384 | 1 | 2 | 3x 0x8000 | 0x4000 |
| mul | 18,800 | 16,384 | 1 | 2 | 3x 0x8000 | 0x4000 |
| island-c-pv | 20,608 | 16,384 | — | 1 | in 0x234000, out 0xbc000, **scratch 0x260000** | 0x4000 |
| matvec | 133,664 | **131,072** | **2** | 1 | 2x 0x4000 | **0x20000** |

All elementwise-class programs share the add surface layout (ANEC header channels:
n=1 c=512 h=1 w=1 planeStride 0x40 — 512 planes of 32 halves, element at half 32j).

The T6021 decode of the same manager (`receipts/2026-09-30-t6021-procedure-section`)
shows what CAneProgramManager*::runProc validates at call time: procedureId < tot,
desc.type == 3, desc.seg_num != 0, then desc.tdBlockIdx -> tdprop_block's IO mapping
(+0x18) and byte size (+0x20). Every fixture's procedure section is byte-identical
(tot=1, one desc, type 3, seg_num 1, tdBlockIdx 0), so the per-program difference the
manager sees is downstream: the task stream's td blocks and the surfaces they name.

## Hypotheses

- **matvec fails in the manager or the exec loop over its second td block and its
  128K section 2.** The weights live in section 2 and the task stream addresses them
  at a fixed IOVA; wAX's `prog_iova_kern`/`prog_text_const` map exactly that surface —
  the assert-or-hang split is consistent with "weights mapping present vs absent".
  It is not a size problem alone: add's sec2 is 16K and passes, matvec's buffers are
  *smaller* (0x4000) than add's.
- **"bigger elementwise" = surface size or td-block count in the exec loop.**
  rms-c2048-gamma is the smallest member of that class: td=8 and 0x20000 surfaces
  against add's td=1 and 0x8000. island-c-pv adds a 2.4MB scratch buffer.
  `ExecLoopHx.cpp:1847` aborts, not a clean refusal — a fixed bound hit at run time,
  not a validation reject. Candidates: tile count per task, scratch aperture, or the
  per-call DMA span.

## The proposal: rung order

1. **mul** (control): identical pack shape to add — same 6 sections, same 3 buffers of
   0x8000, same td=1 — only the op record differs. If it fails, the add pass is
   fixture-specific, not shape-specific, and nothing below is worth a run.
   `ane/h18/mul-check.py` + `sim/test_mul_check.py` already exist on
   `w91/ane-h18-mul-check` (unmerged, 2026-11-12).
2. **clip-high — the smallest program in the set, and this patch's subject.** 4,480 B
   ANEC (kernel: the fp16 bound pair -inf, 0.5), 1 input, 1 output, td=1, same proven
   0x8000 surfaces. First constants-bearing probe: if it passes, the envelope widens
   to one-input programs and small kernel constants, and the matvec failure narrows
   to its second td block / weight-surface mapping. If it asserts in the manager,
   even a minimal program fails and the add pass is shape-specific (3-buffer IO or
   its exact section sizes).
3. **relu** (1 input, 16K kernel, no folded constants): splits clip's two deltas
   (arity vs constants) if clip fails.
4. **rms-c2048-gamma** (the smallest "bigger elementwise"): td=8, surfaces 4x. The
   direct probe of the ExecLoopHx bound — pass or abort bisects surface size vs td
   count against add.
5. **matvec with `prog_iova_kern` + `prog_text_const`** (wAX's existing params):
   the weights mapped at the IOVA the task stream names.

Patch delivered here: `ane/h18/clip-check.py` (gen-input + check, min(x,0.5) exact in
fp16 — no error band, the output must be bit-for-bit the input or the 0.5 bound) and
`ane/h18/sim/test_clip_check.py` (25 controls: clip-low's max() output, an identity
copy, shifted/reversed planes, padding, short dumps and missing finish all FAIL).

## Run recipe (host side, through w7G — pinned by sha256 like every program file)

```
python3 ane/h18/clip-check.py gen-input -o clip-in.f16
tools/ane-prog-pack --anec fixtures/h14-anec/clip-high/program-0.anec \
    --in 5=clip-in.f16 -o ane_h18_prog-clip.bin
# stage per ane/h18/m6-ane-stage.sh with hs_prog=1 prog_calls=N prog_out_words=4096
python3 ane/h18/clip-check.py check <dmesg>
```

## Measured locally

- `sim/test_clip_check.py`: 25/25 controls PASS.
- `ane-prog-pack` on the fixture with the generated input: 6 sections, buf 5 in /
  buf 4 out at 0x8000 each, 512 nonzero halves at the strided plane positions,
  input bytes embedded verbatim.
