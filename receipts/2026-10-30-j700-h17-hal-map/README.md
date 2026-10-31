# J700 H17: the T8140 ANE build's register map, and the M6 cross-check

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`, branch
`neo/h17-bringup` at `e0b4f8a`. Offline work on the retained kernelcache
of this machine's macOS 26.6.2 build (file SHA-256
`9d7dd347fc96cd0cb27a14b6dbc6f02c6354dc531668cdbdd2e50c641d86bbc5`).
No ANE MMIO, no module load, no reboot. Facts are observed values;
method detail stays out of public text per the 2026-10-29 rule.

## The interface constant table has no T8140 row

The interface kext's constant table carries per-family rows shaped
`{doorbell, CPU_STATUS, w1, w2, w3, w4, …}`:

| family | doorbell | CPU_STATUS | w1 | w2 | w3 | w4 |
| --- | --- | --- | --- | --- | --- | --- |
| h14g | `0x1844000` | `0x1400048` | `0x100` | `0x8` | `4` | `0xf` |
| h16g | `0x1884000` | `0x1600048` | `0x10` | `0x8` | `4` | `0x13` |

Both are present verbatim; the h16g row is followed by `0x208/0x218`
and the h14g row by `0xd0/1/3/0xa`. The rows that carry the SCRATCH
pairs (`0x1880020`/`0x1880024`, `0x188003c`/`0x1880040`) also carry
power-block values `0xc000`, `0xc010`, `0xc030` and the block value
`0x196c000`.

**No data row anywhere in the build carries the doorbell `0x18a4000`**
or the IPI `0x18ac000`. There is no T8140/h17 row: the h17-specific
values are supplied by the HAL, as the stage-4 receipt said.

## The T8140 ANE HAL's own map

The HAL composes these engine-relative offsets per access (engine base
`0x400000000`, matching the live DT node `ane@400000000`, window
`0x2000000`):

| offset | use seen in the HAL |
| --- | --- |
| `0x186e000` | stored as a window base |
| `0x1880000` | stored as a window base `0x401880000`; no per-access use in the HAL text |
| `0x18a4000` | doorbell write |
| `0x18a4008` | polled, then acknowledged |
| `0x18a4010`, `0x18a4018` | state reads, `& 3` |
| `0x18ac000` | IPI pending |
| `0x195c000` | window base; `+0x284` and `+0x288` written `1` |
| `0x1960000` | window base; word `+0` written `1` |
| `0x1974000` | window base |
| `0x1974200` | written `0` then `1` (pulse) |
| `0x197c000` | conditional; also logged as an address |
| `0x1984000`, `0x1988000` | window bases |

Observed doorbell function: read `0x18a4010 & 3` and `0x18a4018`,
write the XOR-with-`3` value to `0x18a4000`, poll `0x18a4008 & 3` up
to 5001 times with 10-unit sleeps between, then write the same value
to `0x18a4008`. This is the M6-shaped doorbell block, not the h16g
row's `0x1884000`.

Observed command/mailbox setup sequence: write `1` to `0x1960000`;
touch `0x1974000`; write `0` then `1` to `0x1974200`; touch
`0x195c000`; write `1` to `0x195c284` and `1` to `0x195c288`; then,
conditionally, `0x197c000`.

The HAL never composes `0x160xxxx` (ASC wrapper), `0x1050000`
(RVBAR) or `0x1608000`; it carries no `0x196cxxx` access either.

## Cross-check against the H18/M6 (T8152) table

`ane/h18/ane_h18_soc.c` (T8152 ANE0) gives, relative to its engine
base `0x418000000`:

| M6 window | engine-relative |
| --- | --- |
| ASC `0x419600000` | `+0x1600000` |
| RVBAR `0x419050000` | `+0x1050000` |
| SCRATCH page `0x4198a0000` | `+0x18a0000` (words `+0x24…+0x40`) |
| doorbell `0x18a4000` | `+0x18a4000` |
| mbox `0x316000000` | separate window, not engine-relative |

`ane/h18/ane_h18.h` names the same offsets: `ANE_H18_DOORBELL_OFF
0x18a4000`, `ANE_H18_IPI_PENDING_OFF 0x18ac000`, `ANE_H18_SCRATCH_OFF
0x24` (eight words), `ANE_H18_ASC_CPU_CONTROL 0x44`,
`ANE_H18_ASC_CPU_STATUS 0x48`, `ANE_H18_RVBAR_OFF 0x0`.

The T8140 HAL composes the M6 doorbell `0x18a4000`, so the T8140
engine block is **M6-shaped**. Two consequences:

1. `0x1600048`/`0x1600044` (ASC `+0x48`/`+0x44`) and `0x1050000`
   (RVBAR) are the **same layout on M6 and h16g**; the hung run's
   first three engine accesses were M6-consistent. The "wrapper base
   wrong" fallback is therefore weak, and the missing `0xc008` child
   state remains the prime suspect for the hang.
2. The h16g-shaped SCRATCH list (`0x1880020…0x1880044`) and mailbox
   candidates (`0x1608000`, `0x196c110/114`) have no T8140 support.
   By family parity the T8140 SCRATCH would sit on the doorbell page,
   `0x18a0024…0x18a0040` (M6 layout) — but the HAL stores
   `0x401880000` as a window too, so `0x1880020` cannot yet be ruled
   out. `0x196c000` is `0x195c000 + 0x10000`: the `+0x10000` alias
   the M6 header describes for a Postoffice window, not a base.

## Status

Quarantine stays closed: the module still refuses
(`reason=unqualified-register-map`) and no register access is
authorized. Progress against the hang receipt's release criteria:

- Criterion 1 (source-backed map): partially met. ASC CPU pair and
  RVBAR are corroborated by two sources; the doorbell and IPI are
  pinned by the HAL; the h16g scratch/mailbox inheritance is
  disqualified. Still open: the T8140 SCRATCH page and the T8140
  postoffice/mailbox base (M6 uses a separate window; the T8140 one
  is not in the HAL text).
- Criterion 2 (power sequence): the `0xc008` raise stands; ordering
  relative to the first ASC access is unchanged.
- Criteria 3 and 4 (re-derived module, hang containment): not
  started.

This receipt adds no device behaviour; it is build evidence.
