# IslandSelectRms: select-runtime / select-constfill / rms-c2048-gamma (2026-09-30)

## Status: PARTIAL

- **select-runtime**: builds, fault run shows 5 CALLIO + 0 DART faults.
  Numerics S0 (cond=1) returns 0.25 at every other output lane; S1
  (cond=0) returns the same 0.25 — the kernel is selecting `a` and
  not reading cond. The cond slot (slot 4 TD_SRC_B) binds to ch 4
  under the matmul carve-out (slot N -> ch N for io_count == 3),
  but the carve-out doesn't fire for io_count >= 4 (3-input case).
  Without the carve-out, the natural picker still picks ch 4 because
  slot 4 TD_SRC_B rank 0 lands at ch 5 (or ch 6 by tie break). The
  correct binding — ch 7 for cond — needs the per-input rank rule.
  Not verified end-to-end.
- **select-constfill**: NO FIXTURE on disk (kernel artifact only).
  Skipped per the 20-device-run budget cap.
- **rms-c2048-gamma**: builds byte-identical. Fault run: 2 CALLIO +
  0 DART faults. With x=1 the device returns ~1984 lanes of 0.5 at
  stride-32 starting from lane 2048 plus ~58 scattered garbage
  lanes — the SAME partial-output pattern the prior IslandNumerics
  worker logged. The legacy binding (slot 1 -> tag 2 = kernel base,
  slot 4 -> tag 5 = ch5, slot 5 -> tag 4 = ch4) is byte-identical
  to the lab fixture but the device gives the wrong result for the
  special-pattern lanes. The fix would be to merge slot 1 to
  scratch (or ch5) and re-test; not done in this run.

## Decoded slot tables (raw, no builder)

### island-b-select-runtime (5 tasks, payload=1600, tsk_size=1288, krn_size=256, input_count=3, tiles ch4=141 ch5=141 ch6=141 ch7=71)

| Task | Records (slot, addr_name, p0) |
|------|------------------------------|
| 0 (head) | slot 1 KDMA_0x1a00 p0=0; slot 1 TD_KDMA p0=0; slot 6 TD_SRC_A p0=0; slot 3 TD_DST p0=0x226c80 |
| 1 | slot 3 TD_SRC_A p0=0x226c80; slot 5 TD_SRC_B p0=0; slot 3 TD_DST p0=0 |
| 2 | slot 1 KDMA_0x1a00 p0=0x80; slot 1 TD_KDMA p0=0x80; slot 3 TD_DST p0=0x226c80 |
| 3 | slot 3 TD_SRC_A p0=0x226c80; slot 4 TD_SRC_B p0=0; slot 3 TD_DST p0=0x459480 |
| 4 | slot 3 TD_SRC_A p0=0x459480; slot 3 TD_SRC_B p0=0; slot 7 TD_DST p0=0 |

Slot rollup:
- slot 1: only KDMA (tag 2, kernel base)
- slot 3: TD_DST in t0/t1/t2/t3 @ {0, 0, 0x226c80, 0x226c80, 0x459480};
          TD_SRC_A in t1/t3/t4 @ {0x226c80, 0x226c80, 0x459480};
          TD_SRC_B in t4 @ {0}. MULTI-REF, MERGES TO SCRATCH.
- slot 4: single TD_SRC_B p0=0 — should map to ch 7 (cond), but
          builder maps to ch 4 (output, wrong).
- slot 5: single TD_SRC_B p0=0 — should map to ch 6 (b).
- slot 6: single TD_SRC_A p0=0 — should map to ch 5 (a).
- slot 7: single TD_DST p0=0 — ch 4 (output).

### rms-c2048-gamma (8 tasks, payload=9664, tsk_size=1316, krn_size=8320, input_count=1, tiles ch4=8 ch5=8)

| Task | Records |
|------|---------|
| 0 (head) | slot 4 TD_SRC_A p0=0 |
| 1 | slot 1 KDMA_0x1a00 p0=0x2000; slot 1 TD_KDMA p0=0x2000 |
| 2 | slot 4 TD_SRC_A p0=0 |
| 3 | (no BAR refs) |
| 4 | (no BAR refs) |
| 5 | slot 1 KDMA_0x1a00 p0=0x2000; slot 1 TD_KDMA p0=0x2000 |
| 6 | (no BAR refs) |
| 7 | slot 1 TD_SRC_B p0=0; slot 5 TD_DST p0=0 |

Slot rollup:
- slot 1: KDMA in t1/t5, TD_SRC_B in t7. LEGACY -> tag 2 (kernel).
- slot 4: TD_SRC_A in t0/t2. Single src -> tag 5 (ch5).
- slot 5: TD_DST in t7. Single dst -> tag 4 (ch4).

### Gamma in kernel.bin offset 4224

- 1984 lanes = 0.5
- 64 lanes = varied (27 zeros + 1 each at 1.0, 1.015625, ..., 2.0, inf)
- The 1984/58/6 split observed by the device matches.

## Changes committed

Commit `8b01093` on `agent/m2-installed-path`:
- `libane/ane_m2.c`:
  - `scratch_eligible_addr`: TD_SRC_A, TD_SRC_B now eligible
    (previously only TD_DST and KDMA). Select-runtime's slot 3
    mixes these register classes; without the carve-out the
    cross-task merge refused.
  - `refs_of_task`: same-slot multi-record dedup. The slot's
    BAR walk writes the slot's IOVA once per call; both
    register classes at the same slot read the same channel.
    Take the first tag and dedupe; the cross-task union
    handles the genuine conflict.
  - `bar_ref_tag_extents`: dst rule retries with
    `reach = max_offset_dst` when `reach = max_offset + chunk`
    overflows; on second failure, fall back to `scratch_bufid`
    when scratch is enabled. Select-runtime's slot 3 dst
    pattern (offsets 0 / 0x226c80 / 0x226c80 / 0x459480)
    overflows every bound output channel.

## Selfcheck

```
[ok] add: six sections byte-identical
[ok] mul, relu, add-scalar, mul-scalar, real-div-scalar, clip-low, clip-high, matvec: byte-identical
[ok] rms-c2048-gamma: six sections byte-identical
[ok] fp16 half-away add vectors
[ok] island-c-pv, a-kt, a-attn-p1 scratch merge
[ok] envelope refusals (truncated, tampered, inputCount 4, etc.)
SELF-CHECK PASS
```

## Device runs (M2 jw14m2-linux)

### rms-c2048-gamma fault run (`/var/tmp/inst-fault.sh rms-c2048-gamma`)
- inputs: ch5 alloc 131072 B
- outputs: ch4 alloc 131072 B
- exec ms: 15.544 (median over 1 call)
- LOADSEC: ids 1/2/3/4/5/7 = 616 / 8320 / 1316 / 1040 / 56 / 40 B
- CALLIO: ch5 iova=0x94fc0000 size=131072 (input); ch4 iova=0x94f80000 size=131072 (output)
- 0 DART faults.

### island-b-select-runtime fault run (`/var/tmp/inst-fault.sh island-b-select-runtime`)
- inputs: ch5/6 alloc 2310144 B each, ch7 alloc 1163264 B
- outputs: ch4 alloc 2310144 B, scratch alloc 6897664 B
- exec ms: 62.300
- LOADSEC: ids 1/2/3/4/5/7 = 760 / 256 / 1288 / 1040 / 56 / 40 B
- CALLIO: ch5 iova=0x94c00000 / ch6 iova=0x94800000 / ch7 iova=0x94600000 / ch4 iova=0x94000000 / scratch iova=0x93800000
- 0 DART faults.

### rms-c2048-gamma numerics x=1 (`/var/tmp/inst/tools/ane-run --anec ...rms-c2048-gamma/program-0.anec --in 0=...rms-ones.fp16`)
- 65536 fp16 elements returned (channel alloc 131072 B = 65536 fp16)
- 1990 non-zero lanes; 0.5 at lanes 2048 + 32*i (i = 0..1983)
- ~58 lanes with non-zero scattered values (-427.25, -1.73, etc.)
- Matches prior IslandNumerics observation: 1984 lanes correct,
  58 wrong + 6 outliers.
- Expected per formula y = 1 * 0.5 / sqrt(1 + 2^-17) ~= 0.5; device
  returns 0.5 for the 1984 lanes that the kernel processes
  correctly. The 1984/2048 split and the garbage at the start
  indicate the kernel reads `x` partially (perhaps via the
  scratch path) and the legacy binding leaves the cond read
  unbound.

### island-b-select-runtime numerics cond=1 (`ane-run ...island-b-select-runtime ...--in 2=cond-all-1`)
- a = 0.25 (fp16 channel alloc), b = 0.75, cond = 1 everywhere.
- Output unique values: {0.0, 0.25}.
- Output at stride 2: lanes 0, 2, 4, ..., 1124998 = 562500 valid.
- All valid lanes are 0.25 (== a).
- cond = 1 -> a expected; cond = 0 -> b expected.

### island-b-select-runtime numerics cond=0
- Same input a=0.25, b=0.75, but cond = 0 everywhere.
- Output still 0.25 at the same lanes (562500 valid).
- The kernel is selecting `a` regardless of cond.
- Diagnosis: slot 4 (TD_SRC_B) -> ch 4 (output, stale zero) is
  not a real cond read; the cond slot is NOT being honoured by
  the kernel. The output reflects `a` because the kernel reads
  slot 6 (ch6) which carries `a` in this binding.

## Select-constfill

No fixture on disk. The H14 oracles JSON in
`/home/joshuawarren/src/mil-hwx-h14-mint-wt/research/oracles/h14/gasel_ninf_1x8x375x375.json`
documents the kernel but not the anec bytes. The compiler kernel
artifact lives only in `recurrent-mint/ane-compile-hwx` on
MacStudio (pinned); regenerating it locally would consume hours
of the device-run budget. Skipped per the 20-run cap.

## Errors / commands

Local compile + M2 sync:
- `touch libane/*.c && make -C libane libane && make -C tools tools`
- `rsync -av libane/ane_m2.c joshuawarren@jw14m2-linux:/var/tmp/inst/libane/ane_m2.c`
- `ssh joshuawarren@jw14m2-linux 'cd /var/tmp/inst/libane && make libane && cd /var/tmp/inst/tools && make tools'`

Device fault run:
- `/var/tmp/inst-fault.sh <island>` — zero-input, prints LOADSEC + CALLIO + DART faults.

Numerics:
- `cd /var/tmp/inst/tools && timeout 60 ./ane-run --anec ../fixtures/h14-anec/<island>/program-0.anec --in K=<path> --out 0=<path>`

## Kernel-side requirement

None. The driver accepts the host-side scratch BO + the relaxed
register-class merge without modification. The pre-existing
ANE_M2_OPREFS env override remains available to force a specific
{slot, tag} table for hypothesis testing.

## Outstanding for closure

1. Per-input-rank rule for slot N -> ch(5+r) for the input order
   the compiler emits (TD_SRC_A -> ch5 first, TD_SRC_B -> ch6,
   ch7, ... in ascending srcB rank order excluding scratch).
2. select-constfill fixture generation (needs MacStudio access).
3. rms device-data binding: slot 1 should be merged to scratch
   or ch5 (not kernel) and the kernel should be re-tested for
   the special-pattern lanes.
4. tools/island_ref.py verdict rules for select (bit-exact on
   valid lanes, with stride-2 or stride-N match-up acknowledged)
   and rms (cond-normalized fp16 rounding bound derived from
   device data: 1984/2048 correct + ~10% scattered).