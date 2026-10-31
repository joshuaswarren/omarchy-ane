# J700 H17: the T8140 ANE build's register map, and the M6 cross-check

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`, branch
`neo/h17-bringup`. Offline work on this machine's live device tree and
the retained kernelcache of its macOS 26.6.2 build (file SHA-256
`9d7dd347fc96cd0cb27a14b6dbc6f02c6354dc531668cdbdd2e50c641d86bbc5`).
No ANE MMIO, no module load, no reboot. Facts are observed values;
method detail stays out of public text per the 2026-10-29 rule.

## The live device tree: one window

The live `/proc/device-tree/soc/ane@400000000` on this boot carries
`compatible` = `apple,t8140-ane`, `status` = `disabled`, and exactly
one `reg` entry: address `0x0000000400000000`, size `0x02000000`, with
`reg-names` = `engine`. Three `iommus` phandle/specifier pairs, one
`power-domains` phandle, two interrupts. There is no separate asc,
rvbar, scratch or mailbox window: on T8140 everything the ANE touches
lives inside the 32 MiB engine space. (The probe node
`soc/ane-h17-ps-probe`, `omarchy,t8140-ane-ps-probe`, `status` =
`okay`, is ours.) This is a structural difference from M6, whose ADT
grants asc/rvbar/mailbox as separate reg entries
(`ane/h18/ane_h18_soc.c` header comment).

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
(RVBAR), `0x18a0000` (the M6 scratch page) or `0x1608000`; it carries
no `0x196cxxx` access either. None of those four appears as a stored
window base either.

## The exported window table

Besides the inline compositions, the HAL keeps a lazily-initialised
table of window bases and 48 leaf getters that return their slots.
The engine-relative entries (engine `0x400000000` for ANE0, `0x440000000`
for ANE1; every entry exists as an ANE0/ANE1 twin):

| engine-relative window | ANE0 stored value |
| --- | --- |
| `0x186e000` | `0x40186e000` |
| `0x1880000` | `0x401880000` |
| `0x18ac000` | `0x4018ac000` |
| `0x195c000` | `0x40195c000` |
| `0x1960000` | `0x401960000` |
| `0x1974000` | `0x401974000` |
| `0x197c000` | `0x40197c000` |
| `0x1984000` | `0x401984000` |
| `0x1988000` | `0x401988000` |

The table's remaining entries are windows outside the engine space
(`0x3ffff8000`, `0x3003f0000`, `0x3403f0000`, `0x300470000`,
`0x340470000`, `0x322d44000`, `0x362d44000`, `0x381004000`,
`0x3c1004000`, `0x4123bc000`, `0x4163bc000`, `0x4523bc000`,
`0x4563bc000`, `0x480d40000`, `0x480e28000`, `0x4c0d40000`,
`0x4c0e28000` and the engine/DART bases `0x400000000`,
`0x440000000`); their roles are not attributed here. The getters
themselves are never called from the HAL text — they are consumed
outside it — so a per-window role attribution would need the loaded
vtable, which the kernelcache does not carry in resolvable form.

What the table does answer: the build's whole inventory of ANE
windows on T8140 is the union of the inline compositions and the
exported table, and `0x1600000`, `0x1050000`, `0x18a0000` and
`0x1608000` are in neither.

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

The T8140 HAL composes the M6 doorbell `0x18a4000` and IPI
`0x18ac000`, so the T8140 doorbell/IPI block is **M6-shaped**, not the
h16g row's `0x1884000`. But the build is **silent** on the ASC CPU
pair and RVBAR: neither `0x1600000` nor `0x1050000` is composed or
exported anywhere in the T8140 build, and the live DT names no
wrapper window. Two consequences:

1. `0x1600048`/`0x1600044` and `0x1050000` rest on h16g-row and M6
   family parity only — there is no T8140 source for either. The
   hung run's first three engine accesses were consistent with the
   h16g/M6 layout, but the "wrapper base wrong" fallback is back in
   play: the T8140 generation may keep its ASC wrapper elsewhere
   (the exported `0x195c000`/`0x1960000`/`0x1974000`/`0x197c000`/
   `0x1984000`/`0x1988000` windows are candidates), or the T8140 HAL
   may not drive a CPU wrapper at all — its control path is the
   mailbox/doorbell sequence, and nothing in it resembles a
   CPU_CONTROL `+0x44` write. Since the engine window is fully
   mapped (`0x2000000`), reads at `0x1600048`/`0x1050000` cannot
   fault; they return whatever the interconnect gives.
2. The SCRATCH page is pinned by the build to `0x1880000`: the h16g
   rows put SCRATCH0 at `+0x20` of that page, the exported window
   `0x401880000` is that page, and the M6 scratch page `0x18a0000`
   (doorbell page aside — `0x18a4000` is a different page from
   `0x18a0000`) is neither composed nor exported on T8140. The
   `+0x20`-versus-`+0x24` word alignment inside the page stays open —
   the interface rows carry both (`0x1880020/24`, `0x188003c/40`).
   The mailbox candidates `0x1608000` and `0x196c000` have no T8140
   support (`0x196c000` is `0x195c000 + 0x10000`, the `+0x10000`
   alias the M6 header describes for a Postoffice window, not a
   base); the T8140 mailbox is among the exported windows and is
   unattributed.

## Status

At this receipt's commit the quarantine was closed (the module still
refused, `reason=unqualified-register-map`, and no register access
was authorized); it lifted the same day — see the
[containment receipt](../../receipts/2026-10-30-j700-h17-containment/README.md).
Progress against the hang receipt's release criteria:

- Criterion 1 (source-backed map): the build-side inventory is
  complete and each module address now has a verdict. T8140-sourced:
  doorbell `0x18a4000`, poll/ack `0x18a4008`, state `0x18a4010/18`,
  IPI `0x18ac000`, SCRATCH page `0x1880000` (word alignment `+0x20`
  vs `+0x24` open), and the mailbox-setup sequence at
  `0x1960000`/`0x1974000`/`0x1974200`/`0x195c000(+0x284,+0x288)`/
  `0x197c000`. Unsourced on T8140 (h16g-row/M6 parity only):
  CPU_CONTROL `0x1600044`, CPU_STATUS `0x1600048`, RVBAR
  `0x1050000`; both mailbox candidates `0x1608000` and `0x196c000`.
  Consequence for criterion 3: the re-derived module should follow
  the T8140 HAL's control path (pmgr power, then the mailbox/doorbell
  sequence) and drop or gate the h16g/M6 CPU-wrapper and mailbox
  reads rather than inherit them.
- Criterion 2 (power sequence): the `0xc008` raise stands; ordering
  relative to the first ASC access is unchanged.
- Criteria 3 and 4 (re-derived module, hang containment): not
  started.

This receipt adds no device behaviour; it is build evidence.
