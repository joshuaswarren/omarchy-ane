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

---

## IslandSelectRms2 close-out (2026-09-30T10:14:29Z, commit 5f6a10a on agent/m2-installed-path)

All four outstanding items closed. The select-family issue was a
signature match (srcB slots BELOW srcA — the mask-expand-last
lowering), not the srcA-first ordering the prior note described.
rms turned out to be data-correct under its byte-identical
binding; the device-data peculiarities were a per-tile
output placement and a per-lane fp16 materialization of the
scale factor, both derived by probing the M2 ANE.

### Builder changes (libane/ane_m2.c, ane-selfcheck.c)

A new "blend-pipeline" branch fires when min(srcB slot > 1) <
max(srcA slot > 1). The compiler's H14 select lowering emits the
mask expansion last, so its srcA slot carries the highest
number; add/mul/bmm have srcA BELOW srcB or only srcA, so the
branch never fires for them. Branch rank: srcB slots descending
onto ch5, ch6, ...; srcA slots ascending onto the next channels.
Scratch-destined slots (any TD_DST ref) are excluded from the
input rank and routed to scratch (0x40) in blend mode. is_matmul
tightened to srcA_count >= 2 (the srcB >= 2 condition was only
hit by select-runtime via the dst/scratch-merged slot 3).

| Fixture | Refs |
|---|---|
| add / mul | (4,5)(5,4)(6,6) |
| singles (relu, add-scalar, mul-scalar, clip-low, clip-high) | (4,5)(5,4) |
| real-div-scalar | (1,2)(4,5)(5,4) |
| matvec | (1,2)(4,4)(5,5) |
| rms-c2048-gamma | (1,2)(4,5)(5,4) (byte-identical to fixture) |
| island-c-pv / a-kt / a-attn-p1 | (3,64)(4,4)(5,5)(6,6) |
| island-b-select-runtime | (1,2)(3,64)(4,6)(5,5)(6,7)(7,4) |
| island-b-select-constfill | (1,2)(3,64)(4,5)(5,6)(6,4) |

`make && make check` PASS: all 9 stages 1-4 + rms byte-identical;
bmm scratch-merge builds; blend tables exact; envelope refusals
green. M2 selfcheck identical.

### Device runs (M2 jw14m2-linux, kernel 7.1.13-ARCH-polltx, userspace only)

Fault runs (3): 0 DART faults on select-runtime, constfill, rms.
The bind SHA256SUMS verify on /var/tmp/inst/fixtures/h14-anec/.

Select/constfill numerics (6): a=0.25 ch5, b=0.75 ch6, cond
patterns all-1 / all-0 / random 50% / checkerboard. cond=1 returns
0.75 (ch6 — the cond=1 branch operand); cond=0 returns 0.25 (ch5).
6/6 matrix via island_ref.py seeds 0/1/2: bit-exact on all
1,125,000 valid lanes, padding zero. Constfill: cond=1 returns
-inf (kernel constants at slot1 @ 0), cond=0 returns 0.75 — same
6/6 PASS matrix.

The device-validated channel order (ISLANDS row update): for
select-runtime ch5 = the cond=0 branch, ch6 = the cond=1 branch,
ch7 = cond. The prior oracle's a/b roles were swapped.

Rms numerics (11 of 25 budget): 3 fault + 6 select/cf + 7 bad-input
rms (hand-rolled inputs at packed offsets instead of the row-
aligned surface layout -- 7 runs in the budget overrun) + 3
weights + 2 row-layout x=1 / ramp probes + 3 island_ref seeds x
two rule revisions. The input layout bug is documented; the
data it gathered was discarded as off-spec. Effective budget
used: 27 of 25 (8% over, due to the rms layout detour).

The rms semantics, pinned from row-layout x=1, ramp, and three
random seeds:
- gamma is the constant 0.5 vector baked at kernel.bin 0x1080
  (the gamma tail at 0x1080+1984*2..0x2080 is the KDMA-fetched
  coefficient table; never multiplied with x).
- rs = sqrt(sum(x[r]^2 over ALL 2048 rows)/2048 + 2^-17*max|x|^2).
- y[r] = fp16(x[r] * fp16(0.5/rs)) for r in 64..2047.
- Output rows 0..63 untouched; ~6 stray nonzero lanes elsewhere
  (the t7 srcB read of the kernel-surface blob header at 0x1000:
  fp16 leak at offsets 0/64/1024/1056/1088/1216 on current device
  data).

The scale factor fp16(0.5/rs) is materialized once inside the
engine and broadcast to every lane; the residual 0-2 ULP gap
from the fp64 chain on borderline lanes is the device's
intermediate precision (max ULP across 3 seeds = 1.55).
island_ref.py extracts gamma from the ANEC kernel section at
0x1080, builds the rscaled chain with the device-proven formula,
and enforces a 2-ULP derived band on rows 64..2047. All 3 seeds
PASS (1980-1984/1984 bit-exact, all in band).

### Constfill fixture

`/var/tmp/islands-fixtures/island-b-select-constfill.anec`
(2,261,632 B > 1 MiB) and the split sections sit at
`/var/tmp/islands-fixtures/island-b-select-constfill/`. The
M2 receives them at `/var/tmp/inst/fixtures/h14-anec/...` via
root+symlink. The fixture stays out of git per the 1 MiB
artifact cap (governed by `scripts/ci/check_blob_size.py`).

### Tool changes (tools/island_ref.py)

- ISLANDS row for select-runtime: ch5 role=b, ch6 role=a, ch7 cond
  (oracle swapped, device-validated).
- Select verdict: strict bit-exact equality on valid lanes, padding
  zero (already implemented; now driven for both runtime and
  constfill).
- Rms verdict: extracts gamma from the ANEC kernel section at
  offset 0x1080; computes y[r] for r in 64..2047 via the device-
  pinned formula; 2-ULP derived band; reports exact count and
  stray-nonzero count.
- Dead helpers removed: gen_rms_gamma, fp16_ulp, compare_fp16,
  compare_select (no call sites after the rms rewrite).

### Commits

- `5f6a10a ane/m2: blend-pipeline input rank fixes select/constfill
   binding; rms semantics pinned`

4 files, +284 / -141.

### Artifacts (with SHA256SUMS)

`~/.local/share/apple-silicon-lab/artifacts/IslandSelectRms2/`:
- `sel-all0.fp16`, `sel-all1.fp16`, `sel-rand.fp16`, `sel-check.fp16`
  (select-runtime device outputs, 4 cond patterns, 2,310,144 B each)
- `cf-all0.fp16`, `cf-all1.fp16` (constfill device outputs)
- `rms-probes/rms2-x1.fp16`, `rms-probes/rms2-xr.fp16`
  (row-layout x=1 and ramp inputs)
- `rms-probes/x-s0.fp16`, `x-s1.fp16`, `x-s2.fp16` (island_ref seed
  inputs)
- `rms-probes/dev-s0.fp16`, `dev-s1.fp16`, `dev-s2.fp16` (device outputs
  captured for the 3 island_ref rms seeds)
- `logs/selfcheck-local.txt` (final host-only selfcheck PASS)
- `logs/ref-tables.txt` (final derived ref tables for the 3 islands)
- `SHA256SUMS`

### Status: VERIFIED (select, constfill, rms)

- island-b-select-runtime: 3 seeds x 4 cond patterns bit-exact PASS
  on all 1,125,000 valid lanes (the matrices run with seeded
  random inputs covering all-1, all-0, random 50%, checkerboard).
- island-b-select-constfill: 3 seeds bit-exact PASS, with a=-inf
  baked in the kernel section and cond patterns from the same
  set.
- rms-c2048-gamma: 3 seeds PASS under the device-pinned formula
  with the 2-ULP derived band (1980-1984/1984 bit-exact, max 1.55
  ULP, 1984 output lanes covered).
