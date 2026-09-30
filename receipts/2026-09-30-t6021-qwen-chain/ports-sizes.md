# Qwen port tables: sizes, slots and bufferIds for all 38 programs

Date: 2026-09-30. Branch: agent/m2-ports-sizes, code commit `5c4d29f`. Host-only work: no device
was used. Private record: entry `entries/PortSizes/20260930T232740Z-ct-port-sizes.md`
and `artifacts/PortSizes/2026-09-30-port-sizes/` (old and new tables, logs,
SHA256SUMS).

## Result

`tools/hwx_ports.py` now derives every port from the HWX slot table. The 38
regenerated tables have no exception, and `ane-run --ports ... --dry-run`
exits 0 for all 38. Before this change, 37 of the 38 tables failed the size
gate. The 38 tables hold 416 io ports; 96 of them are larger than 16 KiB.
For `prog_020`, the dry run prints the binding that ran on the M2:
`opref 5:1:2,4:5,5:4,6:6,7:7` and `io 4:5:16384,6:16384,7:16384,4:16384`.

## How the HWX binds a BAR slot

The H14 program descriptor (load command 4, kind 4) holds one IOVA per BAR
slot, at offset `0x10 + 0x10 * slot`:

| Slot | Resource | Bound as |
|------|----------|----------|
| 0 | `__TEXT,__text` (the task stream) | not referenced by any task |
| 1 | `__TEXT,__const` (the kernel constants) | section tag 2 |
| 2 | empty in all 38 programs | not referenced |
| 3 | `__DATA,__bss`, 16,384 B (18 programs) or 4,096 B (6 programs) | scratch io record, bufferId 0x40 |
| 4.. | the io surfaces, in the order of the IOVA array | one io record per port |

The LC 0x40 record at each IOVA names the tensor stored there. The 416
tensor descriptors hold no IOVA-like value, so this name is the only link
from a tensor to a slot. The `__FVMLIB` section at each IOVA gives the
allocation. In all 38 programs the resource spans do not overlap, and
`surface_bytes` is not larger than the allocation.

Before this change, the port path bound slot 3 to nothing. In 24 programs the
task stream writes slot 3 through TileDMA and reads it back through
KernelDMA. The operation record is the only source of a slot's IOVA, so the
firmware would have resolved slot 3 without a host buffer (inference; this
was never run). The port build now refuses any task-stream BAR ref at a slot
that the table does not bind.

## A port larger than 16 KiB

A port is one IOVA entry, so it is one BAR slot and one io BO of the whole
allocation. Example: `prog_000` `t38` is slot 11 at IOVA 0x3001c000 with a
393,216 B allocation. The next surface, `t5`, starts at 0x3007c000, which is
0x3001c000 + 0x60000. The 16 KiB steps in the IOVA map are how Apple packs
the surfaces, not several slots. libane and the driver can express this
already: an io record carries `{bufferId, size}`, `BO_INIT` accepts up to
1 GiB, and the operation record binds `{slot, bufferId}`.

## Which ANEC channel holds each port

The HWX-to-ANEC converter gives the first output channel 4, the inputs
channels 5 and up in descriptor order, and the other outputs the next
channels. It sets `tiles[channel]` to the allocation in 16 KiB units. The
generator uses the same plan and checks `tiles[bufferId] * 16384` against
the allocation rounded up to 16 KiB for every port. All 416 ports agree.

The old generator used channel `4 + index` inside each direction. That rule
put `prog_000` `t38` on channel 10 (16,384 B), while the ANEC allocates
393,216 B on channel 12. This caused the 37 failures. The per-channel
`iova` field in the converter manifest is a size and order match, not the
plan (for `prog_020` it puts channel 4 at the IOVA of `t0`), so the
generator does not use it.

## What the task DMA touches

The generator walks every dense BAR-ref record (bit 29) and records the slot,
the register class and the offset. In all 38 programs:

- every referenced slot is bound (kernel, scratch or port);
- every offset at an io slot and at the scratch slot is 0;
- every slot-1 offset is inside the constant region (largest 222,979,392 B
  of 222,980,416 B);
- no record writes a TileDMA base (0x1110, 0x1128, 0x1508 or their high
  words) without a BAR ref;
- no input is written by TileDMA, and every output is written by TileDMA.

The DMA extent is not computed. The rule from the task registers to the
bytes one DMA touches is not decoded: a C x H x W x stride model of the
written registers gives 4,194,304 B for `prog_022` task 8 at slot 6, whose
tensor is 524,288 B. The table therefore uses Apple's allocation as the
span. The allocations do not overlap in Apple's IOVA map, and every BO is at
least the allocation.

## Changes

- `tools/hwx_ports.py`: derives ports from the slot table, the LC 0x40
  names and the converter channel plan; adds the scratch entry; audits every
  BAR ref; lists ambiguous port groups with a reason and a device test.
- `libane`: `ane_m2_program_build_ports` accepts one or more outputs and one
  scratch (`dir` 2, bufferId 0x40, slot 3); refuses duplicate bufferIds or
  slots, a BAR ref at an unbound slot, and a BAR-ref offset outside the bound
  buffer. Both builders now share one section emitter. The task limit is 128
  (the largest program has 120 tasks; the old limit of 64 refused 6
  programs).
- `tools/ane-run.c`: accepts `"direction": "scratch"`; each named output
  maps to its position among the outputs; up to 64 `--in`/`--out` names
  (6 programs have 10 or 11 inputs).
- `tools/qwen_prog_run.py`: packs and unpacks at any non-overlapping
  descriptor strides (for example `t5` [1,1,6144,3] at 64 B per row, and
  [1,16,1,1] at 64 B per plane). The `prog_020` inputs pack to the same
  bytes as the surfaces that ran on the M2 (`c2f1483b`, `d1fa1370`,
  `9e996bea`). The HWX readers moved into the generator. A device run of a
  program with more than 2 tasks is refused until the driver waits for the
  program's own TD count; `--dry` and `--pack-only` still work.

## Per-program tables

Ports larger than 16 KiB are listed as `name@slot/channel=bytes`; all other
ports are 16,384 B. An ambiguity group lists ports with the same direction,
shape and strides.

| Program | In | Out | Scratch | Ports > 16 KiB | Ambiguity groups | Dry run |
|---|---|---|---|---|---|---|
| prog_000 | 2 | 7 | no | t38@s11/ch12=393216 t5@s12/ch6=393216 | out:t16/t20/t22/t37; out:t26/t35 | exit 0 |
| prog_001 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_002 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_003 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_004 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_005 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_006 | 11 | 10 | yes | t109@s7/ch17=393216 t28@s11/ch9=65536 t30@s12/ch18=65536 t44@s16/ch13=65536 t46@s17/ch19=65536 t76@s20/ch15=393216 | in:t0/t2; out:t106/t97; out:t108/t87/t91/t93; in:t22/t27; in:t28/t44; out:t30/t46; in:t32/t39 | exit 0 |
| prog_007 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_008 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_009 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_010 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_011 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_012 | 11 | 10 | yes | t109@s7/ch17=393216 t28@s11/ch9=65536 t30@s12/ch18=65536 t44@s16/ch13=65536 t46@s17/ch19=65536 t76@s20/ch15=393216 | in:t0/t2; out:t106/t97; out:t108/t87/t91/t93; in:t22/t27; in:t28/t44; out:t30/t46; in:t32/t39 | exit 0 |
| prog_013 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_014 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_015 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_016 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_017 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_018 | 11 | 10 | yes | t109@s7/ch17=393216 t28@s11/ch9=65536 t30@s12/ch18=65536 t44@s16/ch13=65536 t46@s17/ch19=65536 t76@s20/ch15=393216 | in:t0/t2; out:t106/t97; out:t108/t87/t91/t93; in:t22/t27; in:t28/t44; out:t30/t46; in:t32/t39 | exit 0 |
| prog_019 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_020 | 3 | 1 | no | none | in:t0/t2 (resolved by Prog20Map) | exit 0 |
| prog_021 | 2 | 7 | no | t38@s11/ch12=393216 t5@s12/ch6=393216 | out:t16/t20/t22/t37; out:t26/t35 | exit 0 |
| prog_022 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_023 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_024 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_025 | 11 | 10 | yes | t109@s7/ch17=393216 t28@s11/ch9=65536 t30@s12/ch18=65536 t44@s16/ch13=65536 t46@s17/ch19=65536 t76@s20/ch15=393216 | in:t0/t2; out:t106/t97; out:t108/t87/t91/t93; in:t22/t27; in:t28/t44; out:t30/t46; in:t32/t39 | exit 0 |
| prog_026 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_027 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_028 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_029 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_030 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_031 | 11 | 10 | yes | t109@s7/ch17=393216 t28@s11/ch9=65536 t30@s12/ch18=65536 t44@s16/ch13=65536 t46@s17/ch19=65536 t76@s20/ch15=393216 | in:t0/t2; out:t106/t97; out:t108/t87/t91/t93; in:t22/t27; in:t28/t44; out:t30/t46; in:t32/t39 | exit 0 |
| prog_032 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_033 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_034 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_035 | 4 | 8 | no | t20@s7/ch7=393216 t53@s14/ch15=393216 | in:t0/t2; out:t31/t35/t37/t52; out:t41/t50 | exit 0 |
| prog_036 | 6 | 2 | yes | t13@s6/ch4=524288 t2@s9/ch8=524288 | in:t0/t1; in:t14/t4/t7 | exit 0 |
| prog_037 | 10 | 3 | yes | t28@s8/ch9=65536 t30@s9/ch4=65536 t44@s13/ch13=65536 t46@s14/ch15=65536 | in:t0/t2; in:t22/t27; in:t28/t44; out:t30/t46; in:t32/t39 | exit 0 |

## Ambiguous ports and device tests

In an ambiguity group, the ports have the same direction, shape and strides.
Neither the tensor descriptors nor the task DMA tell their slots apart; only
the LC 0x40 name at each IOVA assigns a name to a slot. Each table gives the
reason and a device test for each group:

- Output group: run once with the M1 golden inputs, then compare every port
  in the group with every golden in the group. The name binding predicts
  rel L2 <= 0.02 on the name-matched pairs only. The M1 goldens in the
  `prog_000` groups are pairwise distinct (rel L2 0.9999 to 1.0605 for
  t16/t20/t22/t37, 0.7338 for t26/t35), so one run separates them.
- Input group: run A binds the golden inputs by name; run B swaps the files
  of two ports in the group. A predicts rel L2 <= 0.02 on every output. B
  predicts more, unless the graph is symmetric in the two inputs. Check the
  swap with the fp16 MIL evaluation on the host first. The `prog_001`
  golden inputs are pairwise distinct (0.7338 for t0/t1, 1.0002 to 1.0477
  for t14/t4/t7).
- `prog_020` `t0`/`t2` is resolved: the run that left slot 4 on the zeroed
  output buffer matched the fp16 MIL graph with `t0 := 0` (rel L2 0.0122),
  so slot 4 is `t0` ([prog20-port-binding.md](prog20-port-binding.md)).

## Before a device run of programs 0 and 1

1. The driver call wait. `prog_000` has 32 tasks and `prog_001` has 10. The
   program-20 run stopped multi-task runs on module 7b592674 until the call
   waits for the program's own TD count ([README.md](README.md)).
2. `prog_001` is the first port-table program with a scratch. Slot 3 binds
   bufferId 0x40 (16,384 B). The derived build used the same binding for the
   three bmm islands, which passed on the M2
   ([island-bmm](../2026-09-30-t6021-island-bmm/README.md)).
3. Host-proven commands (dry run, no device):
   - `qwen_prog_run.py --prog prog_000 --in t1=… --in t5=…` with goldens for
     t16, t20, t22, t26, t35, t37 and t38 (capture e0418).
   - `qwen_prog_run.py --prog prog_001 --in t0=… --in t1=… --in t14=… --in t2=… --in t4=… --in t7=…`
     with goldens for t13 and t17 (capture e0419).
   Both pack every input (16,384 to 524,288 B) and print the locked
   `ane-run` command.

## Verification

- `make -C libane libane && make -C tools tools`: no warnings under
  `-Wall -Werror -Wextra`.
- `tools/ane-selfcheck fixtures/h14-anec`: SELF-CHECK PASS, including the
  new refusal of a table that leaves a task-stream slot unbound.
- `python3 -m pytest -q tests/`: 8 passed. `test_hwx_ports.py` regenerates
  all 38 tables, checks the size and coverage invariants, runs
  `ane-run --dry-run` on each, and checks the `prog_020` binding.
- `/var/tmp/qwen-real-anec-h14/PORTS_SHA256SUMS` lists the 38 new tables.
