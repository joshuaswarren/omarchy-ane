# T6021: the total BO cap is a parameter; all 38 Qwen programs on one boot (2026-10-01)

## Problem

`ane_t6021` counted the BO bytes held at one time against a fixed 2 GiB cap
(`ANE_T6021_BO_TOTAL_MAX`), and `DRM_IOCTL_ANE_BO_INIT` returned `-ENOSPC`
above it. A program section BO stays held until reboot, because the cached
firmware program keeps reading it. The 38 real Qwen programs have
2,754,388,032 B of ANEC (22,464 B to 223,006,144 B each), so one boot could
not load all of them.

## Change (commits `62f04c6`, `6dd8f12`)

In `ane/t6021/ane_t6021_rtclient_main.c`:

- `bo_total_max_mb` (`:256`, uint, 0444, default 12288) replaces
  `ANE_T6021_BO_TOTAL_MAX`. `BO_INIT` returns `-ENOSPC` when the counted bytes
  would pass `bo_total_max_mb << 20` (`:1145`). The per-BO limit stays 1 GiB.
- `bo_total_bytes` (`:261`-`:279`) is a read-only parameter that shows the
  counted bytes now. A write returns `-EPERM`.
- The probe line names the cap: `loaded ane_t6021 ... BO cap 12288 MiB`
  (`:2010`).
- The ioctl ABI is unchanged. `ane/t6021/README-install.md` lists both
  parameters.

A second limit is independent of the cap. The module sets a 32-bit DMA mask
(`:1708`), so all BOs share 4 GiB of IOVA, minus the firmware maps. Near
4 GiB of BOs, `BO_INIT` fails with `-ENOMEM` whatever the cap. Thus the
12 GiB default cannot be reached today. A change of the DMA mask is a
separate decision.

`ane/t6021/gate/gate.sh` now checks every island op (`island-*`,
`rms-c2048-gamma`) with `tools/island_ref.py --island OP --seed 0..2` and the
gate's ANEC. Before, the gate made its own inputs and failed
`island-b-select-runtime` and `rms-c2048-gamma` while `island_ref.py` passed
the same ANEC bytes. Cause, from the ANEC tensor headers:

- The gate filled the first N*C*H*W elements densely. select-runtime rows are
  768 B with W=375, so the valid columns are 0-749; the gate also wrote
  columns 750-767 of every row.
- rms needs one half per 64-B row (C=2048, plane 64, row 64). The gate packed
  2048 halves into 4 KiB.
- For rms the gate checked against a random gamma file. The device uses the
  gamma in `kernel.bin` at 0x1080.

`island_ref.py` keeps the row strides and reads the real gamma, so the gate
now uses it. The dense generator remains for matvec and the elementwise ops.

## Device test (boot `0e2c3743`, module `a584a967`, no module options)

Stock `7.1.13-3-1-ARCH`, DTB from the packaged overlay. The module was built
on the device from `19648c8`. The merged `ane/` tree differs from it only in
two comments that named the old 2 GiB cap (the rebase onto main touched only
tools and tests). Module SHA-256
`a584a96744a8d51232ee1ede5996c8fe8f8cf11556b2751283446cc539f7ae7f`.

| Check | Result |
| --- | --- |
| Probe line, parameters | `BO cap 12288 MiB`; `bo_total_max_mb` 12288; `bo_total_bytes` 0 after probe |
| AIC2 884 and 1833 (mailbox recv, send) | 0 and 0 |
| add, mul, matvec 2048x5120 gates | PASS |
| `island_ref.py`, 6 islands x 3 seeds | 18/18 PASS |
| `gate.sh` with each of the 6 island ops | 6/6 GATE PASS |
| Qwen program 20, one call, golden inputs | rel L2 0.001174 vs the M1 golden, max abs 0.00073 |
| Lifecycle: 4 workers x 38 loads of the 2048x5120 matvec | 152/152 PASS |
| 38 Qwen programs, one call each, zero inputs | 38/38 rc 0, no `ENOSPC`, no `ENOMEM` |
| `bo_total_bytes` before and after the 38 programs | 143,589,376 B and 2,818,605,056 B |
| dmesg, whole boot | 0 DART faults, 0 `EXCH ... failed`, 0 quarantine, 0 completion-wait lines, 0 allocation failures |

Each program ran as `ane-run --anec prog_NNN/program-0.anec --ports
prog_NNN/ports.json --repeat 1 --time` under the device lock, in order. The
outputs were not checked. Wall time includes the load.

| Program | ANEC bytes | Wall ms | Exec ms | `bo_total_bytes` after |
| --- | ---: | ---: | ---: | ---: |
| prog_000 | 34,098,240 | 66 | 3.017 | 178,520,064 |
| prog_001 | 22,464 | 5 | 1.733 | 179,601,408 |
| prog_002 | 117,994,432 | 201 | 6.309 | 297,598,976 |
| prog_003 | 22,464 | 4 | 1.630 | 297,631,744 |
| prog_004 | 117,994,432 | 197 | 6.336 | 415,694,848 |
| prog_005 | 22,464 | 5 | 1.627 | 415,793,152 |
| prog_006 | 223,006,144 | 318 | 10.675 | 639,352,832 |
| prog_007 | 22,464 | 3 | 1.617 | 639,352,832 |
| prog_008 | 117,994,432 | 206 | 6.356 | 757,350,400 |
| prog_009 | 22,464 | 4 | 1.621 | 757,415,936 |
| prog_010 | 117,994,432 | 180 | 6.329 | 875,479,040 |
| prog_011 | 22,464 | 5 | 1.614 | 875,577,344 |
| prog_012 | 223,006,144 | 316 | 10.679 | 1,098,809,344 |
| prog_013 | 22,464 | 4 | 1.729 | 1,098,809,344 |
| prog_014 | 117,994,432 | 170 | 6.316 | 1,216,806,912 |
| prog_015 | 22,464 | 4 | 1.725 | 1,216,872,448 |
| prog_016 | 117,994,432 | 226 | 6.411 | 1,334,935,552 |
| prog_017 | 22,464 | 3 | 1.718 | 1,335,033,856 |
| prog_018 | 223,006,144 | 316 | 10.668 | 1,558,265,856 |
| prog_019 | 22,464 | 3 | 1.687 | 1,558,265,856 |
| prog_020 | 83,900,608 | 111 | 4.881 | 1,558,265,856 |
| prog_021 | 34,098,240 | 56 | 3.043 | 1,592,377,344 |
| prog_022 | 22,464 | 4 | 1.722 | 1,592,442,880 |
| prog_023 | 117,994,432 | 179 | 6.405 | 1,710,505,984 |
| prog_024 | 22,464 | 4 | 1.724 | 1,710,604,288 |
| prog_025 | 223,006,144 | 321 | 10.711 | 1,933,885,440 |
| prog_026 | 22,464 | 5 | 1.620 | 1,933,885,440 |
| prog_027 | 117,994,432 | 193 | 6.425 | 2,051,883,008 |
| prog_028 | 22,464 | 4 | 1.671 | 2,051,948,544 |
| prog_029 | 117,994,432 | 188 | 6.407 | 2,170,011,648 |
| prog_030 | 22,464 | 4 | 1.604 | 2,170,109,952 |
| prog_031 | 223,006,144 | 381 | 10.728 | 2,393,391,104 |
| prog_032 | 22,464 | 3 | 1.668 | 2,393,391,104 |
| prog_033 | 117,994,432 | 162 | 6.345 | 2,511,388,672 |
| prog_034 | 22,464 | 4 | 1.668 | 2,511,454,208 |
| prog_035 | 117,994,432 | 201 | 6.399 | 2,629,517,312 |
| prog_036 | 22,464 | 6 | 1.688 | 2,629,615,616 |
| prog_037 | 188,917,120 | 292 | 9.146 | 2,818,605,056 |

The 38 programs added 2,675,015,680 B to the counter, less than their ANEC
total, for two reasons. The 18 programs of 22,464 B have byte-identical
ANECs, so the module loads them once (the SHA-256 program digest, `:863`), and
each later one adds only io BOs of new sizes (0-98,304 B). prog_020 was loaded
earlier in the same run (the golden check), so its sections were already
counted.

At the end of the run the counter was 2.63 GiB, so about 1.37 GiB of the
4 GiB IOVA window was left, less the firmware maps. Another model of the same
size (2.49 GiB of new sections) cannot fit on the same boot.

## Limits

- One boot. The 38-program run proves load and execution, not the outputs;
  only prog_020 was checked against a golden.
- The firmware program table holds 256 entries, and the module stops at 250
  distinct programs (`ANE_T6021_MAX_PROGRAMS`, `:715`). This boot used 21
  distinct Qwen programs plus the gate and island programs.
- The new prog_020 port table (from PR #10) cannot tell `t0` from `t2` by
  shape and strides, and `qwen_prog_run.py` warns. The golden result was the
  same as with the older table, so the binding order was right on this run.
- Every M2 boot is a USB chainload from the M1 host. The disk boot is
  unproven.

## Receipts

Private notebook: entry `entries/BoCap/20261001T001500Z-…-bo-cap.md`,
`artifacts/BoCap/` with `SHA256SUMS` (the verify, 38-program and timing
scripts, gate logs, island logs, the 38-program table and logs, dmesg, and
the SHA-256 of every large output left on the device).
