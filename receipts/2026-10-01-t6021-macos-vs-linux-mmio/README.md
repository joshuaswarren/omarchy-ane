# T6021: the macOS 13.5 hv trace against the Linux ane_t6021 MMIO footprint (2026-10-01)

Status: step 4 E1 and E2 ran later on 2026-10-01, see
[2026-10-01-t6021-dart-tunables](../2026-10-01-t6021-dart-tunables/README.md)
(BRD/BWR at reset values, LLT already applied, PERF counters off; applied in
the macOS order, 0x220/0x224 and the SID words do not change the speed, and
0x20c breaks translation on a live DART). Step 4 E4 (rank 4, the ten P-1
words skipped) and the rank 2 analysis ran in
[2026-10-01-t6021-p1-groups](../2026-10-01-t6021-p1-groups/README.md): no
speed change; the 0x300-0x310 words are ADT tunables of all three DARTs.

The macOS hv trace shows one DMA-path setting that Linux never makes: the
DART tunables on the two bulk DARTs of the ANE, DARTBRD and DARTBWR. By
their names these carry the bulk reads and writes of the ANE: weights and
activations [INFERENCE from the ADT instance names]. macOS writes six
control words and 16 per-stream words on each of them. The Linux
`apple-dart` driver writes none of them, so they stay at their reset values.
Three independent sources give the same values: the 13.5 trace, the j414c
iBoot of macOS 26.6.2, and the live macOS 27.0 device tree. Translation
itself is the same on both systems: 16 KiB pages and 4-level tables, with
no bypass and no large pages for the ANE data stream. So each TLB entry
covers the same 16 KiB on both systems, and of the DART state the trace
shows, only these tunables differ for the ANE data stream. If the DART
limits the ANE under Linux, the tunables are where the difference must be.

This is an offline analysis. Nothing ran on a device.

## The trace and its limits

- Source: `tools/m2hv_replay-trace-135.txt` (151 writes, event ordinals of
  `trace-135.log`), the counts in
  [2026-09-24-m2hv-diff-tool.md](../2026-09-23-m2-hv-trace/2026-09-24-m2hv-diff-tool.md),
  and the window list in `tools/m2hv_diff.py` (trace config v3).
- The guest is a kernel-only macOS 13.5 with no root filesystem. The 13.5
  ANE kext starts the ASC only when a user client opens the ANE. So the
  trace has no firmware start, no mailbox or doorbell write, no
  `CH_PROPERTY_WRITE` and no job. Every ANE write in it comes before a
  firmware start that never happens. The task asked for "up to the first
  job"; no job exists in any hv trace.
- The trace config traced writes only (`read=False`). The only reads in the
  log are 60 PMGR-hook reads. The raw `trace-135.log` is not in this
  repository, so this receipt has no per-read table.
- Traced windows: engine-low (4 KiB at 0x284000000), RVBAR, ASC wrapper,
  the three ANE DARTs (16 KiB each), MBI scratch, pmgr 0x28e080000 (64 KiB)
  and two VENC power-state windows. Not traced: the DAPF at 0x285804000, the
  DPE block at 0x2858ec000, TM, the IPI doorbell, the PMP, DCS/AMCC, fabric
  QoS, CLPC, SMC and AIC. The trace cannot say anything about those blocks.
- The ASC's own MMIO accesses (firmware-side) are not in an hv trace of the
  AP.
- The hv log has no timestamps. Order is the event ordinal.

## Step 1: what macOS writes (`macos-trace-blocks.tsv`, `macos-trace-writes.tsv`)

| block | writes | events | phase |
|---|---|---|---|
| pmgr SET window 0x28e08c000 | 11 | 15-2930 | before bring-up, power-down |
| pmgr ps: afi, afnc0_ioa/ls/lw0 (fabric parents) | 12 | 18-106 | before bring-up |
| VENC_SYS, VENC_DMA ps | 9 | 1063-1201 | before bring-up |
| pmgr ps: ane_sys, ane_cpu | 7 | 2813-2932 | power-up, ane_cpu on, power-down |
| engine-low (AXI2AF bridge) | 26 | 2816-2841 | after ane_sys on |
| dart-ane0 (LLT) | 12 | 2843-2918 | DART init |
| dart-ane1 (BRD) | 34 | 2851-2920 | DART init |
| dart-ane2 (BWR) | 34 | 2881-2922 | DART init |
| pmgr ps: ANE islands (set4..td) | 6 | 2923-2928 | power-down (T=0) |
| ASC wrapper, RVBAR, scratch | 0 | - | - |
| other pmgr and east ps words (ISP, i2c6, ...) | 2,927 | up to 3170 | after the ANE power-down |

The ADT `instance` property names the three DARTs DARTLLT, DARTBRD and
DARTBWR, and the DAPF DAPFLLT. The names suggest low-latency, bulk-read and
bulk-write paths [INFERENCE].

## Step 2: what Linux touches (`linux-footprint.tsv`, `linux-baseline-counts.tsv`)

Source: omarchy-ane `603c79b` and omarchy-linux `57f8f6deaa3a`, default
module parameters.

- pmgr ps words (kernel `pmgr-pwrstate` and genpd): the fabric parents, ane_sys,
  ane_cpu, the seven ANE islands, and ps_pmp, because dart-ane0 sits in the
  pmp domain (`packaging/dt/t602x-ane.dtsi:118`). All stay on until reboot.
- DARTs (kernel `apple-dart`): a probe reset (51 writes per DART), attach
  (TTBR, TCR[0] = 0x9, ENABLE_STREAMS), one TLB flush per map or unmap,
  error IRQ, resume restore. No tunable, DIAG_LOCK, bypass or PERF write.
- Engine: P-1 writes 12 words at engine+0x000..0x900 (`ane_t6021_boot.h:555-582`),
  P1 scratch, CPU_CONTROL 0 then 0x10, SCRATCH polls, the SCRATCH3 ack.
- Per CALL: the IPI doorbell at 0x285844000 (ring, pending read, clear) and
  DRAM rings. With `trace_td=1` only: the TM TD word and the seven island
  ps words, read.
- Never: the SET window, the DAPF, the ASC mailbox data path (bound, never
  started), the PMP, DCS/AMCC, fabric QoS, CLPC.

## Step 3: diff, ranked (`diff-ranked.tsv`)

| # | block, registers | macOS | Linux | plausibility |
|---|---|---|---|---|
| 1 | dart-ane1/2: 0x20c, 0x220, 0x224, per-SID 0x800-0x83c | RMW at init; sid 0 field[19:16] = 6 | reset value | high |
| 2 | dart-ane1/2: 0x300 enable, 0x308/0x310 DVA window | enabled, window 0x100_0000_0000..0x3ff_ffff_c000 | reset value; BO IOVAs below 4 GiB | medium |
| 3 | PMP (ANE DVFS) | runs | node disabled | high for the clock-bound part; not in the trace |
| 4 | engine-low 0x038, 0x03c, 0x600, 0x738, 0x798, 0x7f8, 0x900, 0x410, 0x420, 0x430 | never written | written at P-1 | medium |
| 5 | dart-ane0: 0x20c..0x310 | in the 27.0 ADT; not in the 13.5 trace | reset value | low-medium |
| 6 | DAPF LLT + TCR[15] bypass | set | neither; the PMU page is mapped through sid 0 | low-medium |
| 7 | SET window +0x0, +0x30..+0x38, +0x2dc | written | never | low, unknown |
| 8 | ane_sys_mpm | off under load | on | low |
| 9 | VENC rails | cycled, then off | not touched | none |
| 10 | DIAG_LOCK, ERROR_DISABLE, DISABLE_STREAMS | written | ERROR_MASK = 0 | none |

Notes on the top rows:

- Row 1: the 13.5 trace values equal the RMW of the ADT values in every
  case (the script asserts this). The 26.6.2 j414c iBoot carries the same
  two 22-record tables at 0x1de5fc (BRD) and 0x1de710 (BWR). On T8103, m1n1
  writes 0xf0f0f and 0x80808 to the ANE DART at 0x68 and 0x6c (m1n1 v1.6.1
  `src/tunables_static.c:92-97`), the same values as T6021 0x220 and
  0x224. The M1 is the chip with the small macOS/Linux gap (113 vs 141 ms);
  that is a correlation, not a proof.
- Row 1, reset bits: the trace gives the bits outside each mask before the
  write. 0x20c had 0x48 set; 0x310 had bits 1:0 set; all other words had no
  bits outside their masks. The masked fields under Linux are unknown until
  a read.
- Row 2: 0x308 and 0x310 hold vm-base and vm-base + vm-size in 4 KiB units.
  The driver sets a 32-bit DMA mask (`rtclient_main.c:1910`), so every BO
  IOVA lies below 4 GiB, outside that window. If the window gates a feature,
  Linux BOs would not get it even with the tunable applied. If it gates
  translation, enabling it could fault the BOs.
- Row 4: Linux writes 10 bridge-page words that macOS never writes. Seven
  of them (0x038-0x900) carry the values of m1n1's T8103 ANE pmgr table
  (m1n1 v1.6.1 `src/tunables_static.c:79-89`); 0x410, 0x420 and 0x430 get
  0x1100 from the W8 grant run. AfBridgeRun S3 replaced only 0x000 and
  0x400 and kept these 10.

## DART and TLB, with numbers (`tlb.txt`, `dart-tunables.tsv`)

- macOS translates the ANE data stream: TCR[0] = 0x9 (translate, 4-level) on
  all three DARTs, the same value Linux writes. Only sid 15 gets
  BYPASS_DART (TCR[15] = 0x2), and ENABLE_STREAMS enables sid 0 only. The
  ADT `page-size` is 0x4000, and the Linux DART page-table format maps one
  page size only, with no block entries (`io-pgtable-dart.c:238,302`). So
  neither system uses a larger granule, a bypass or a linear map for ANE
  buffers.
- One encoder CALL moves 3.47 GB (estimate: 413.6 MB weights by kernel DMA,
  3,054 MB tile traffic). With 16 KiB pages that is 25,244 distinct weight
  pages, about 3,723 scratch pages (61 MB), and 211,646 page-equivalents of
  traffic.
- Holding the activation working set in the TLB needs 3,723 entries. The
  DART TLB size is not known.
- Average traffic: Linux 13.7 GB/s (253.10 ms), macOS 38.8 GB/s (89.3 ms),
  if the native build moves the same bytes.
- The gap of 163.8 ms is 0.774 us per page-equivalent, or 5.65 us per
  distinct page.
- Serialized walk bound, with an assumed 0.10 to 0.60 us per miss: one miss
  per distinct page costs 2.9-17.4 ms; one miss per page-equivalent costs
  21.2-127.0 ms. So translation can explain the whole gap only if almost
  every 16 KiB of traffic misses and the walks serialize at about 0.77 us
  each. The DART PERF counters (TLB, ST and CTC hit/miss/fill at
  0x760-0x788) can measure the miss count directly.

## Step 4: experiments, ranked

Timing method for every write experiment: the NativeVsCross harness, 20
blocks of 16 calls, encoder min-of-min against 253.1-254.3 ms, plus
prog_006, prog_020 and matvec 2048x5120; numerics gates first (bit-exact
encoder, golden prog_020). The AfBridgeRun rule applies: REJECTED below a
3% drop. The `trace_td` family sums show which task family moved.

1. E1, read only, low risk. On Linux, with the device held and the island
   guard, read 0x000-0x00c, 0x20c, 0x210, 0x220-0x22c, 0x300-0x310,
   0x700-0x788 and 0x800-0x83c on all three DARTs, then read the PERF
   words again after one encoder call. This gives the reset values of the
   masked fields and the TLB misses per call. Expected: the masked fields
   differ from the macOS values; the miss count tells which TLB model holds.
2. E1b, read only, macOS. The same addresses on macOS 27 through the
   `tools/macos-regdump` kext (`ranges.txt` is runtime data, so no rebuild),
   around one CoreML encoder run. Expected: the macOS values and the macOS
   miss count. Equal counts and a lower time would mean miss cost, not miss
   count.
3. E2, write, medium risk. Apply the row-1 RMWs (0x20c, 0x220, 0x224,
   0x800-0x83c with the ADT masks) on dart-ane1 and dart-ane2 after the
   DART attach and before the firmware start, with readback; leave
   0x300-0x310 alone. The 2026-09-24 replay wrote these values with
   readback and no fault, but ran no job. Failure mode: a DART fault or a
   stalled CALL; recovery is a reboot. Expected if the DART is the limit:
   the attention and PE families (about 14 GB/s now) speed up most.
4. E4, write, medium risk. Skip the 10 Linux-only P-1 words and keep the
   26 macOS bridge RMWs. Read those 10 words before P-1 on a fresh boot
   first. P-1 is part of the firmware start, so a failure leaves the ANE
   unusable until reboot.
5. E3, write, medium-high risk. E2 plus the 0x300-0x310 window, only after
   BO IOVAs move into [0x100_0000_0000, 0x400_0000_0000) (a wider DMA mask).
   Test the IOVA move alone first; whether the firmware accepts BO IOVAs
   above 4 GiB is unknown.
6. E5, write, medium risk. Also apply the six dart-ane0 (LLT) words.
7. E6, firmware property, low risk. `CH_PROPERTY_WRITE` 0x1701 = 1 (PPT).
   This comes from the firmware analysis, not from the trace; listed so the
   order is complete.
8. E7, high risk, needs prerequisites. Start the PMP (enable the node, ADT
   tunable payloads). The trace gives nothing for it.

Not proposed: the DAPF and sid 15 bypass (the firmware path already works
through the IOVA==PA PMU map), the SET window (external-abort class from
Linux), ane_sys_mpm and VENC (power form only).

## What the trace cannot tell

- Anything after the firmware start: job submission, the doorbell and
  mailbox traffic, property writes and their ids, and the firmware's own
  MMIO. No hv trace has a started ANE.
- Reads, apart from the 60 PMGR-hook reads.
- Blocks outside the windows: PMP, DCS/AMCC, fabric QoS, CLPC, SMC, AIC,
  the DPE block, the DAPF and TM.
- The masked fields of the DART tunables before the write, and so the Linux
  values.
- Time: the log has event ordinals only.
- macOS 27 behaviour: the trace is 13.5. The 27.0 ADT adds dart-ane0
  tunables that the 13.5 trace does not write.

## Files and reproduction

    python3 receipts/2026-10-01-t6021-macos-vs-linux-mmio/mmio_diff.py . \
        DART_IOREG OUTDIR

`DART_IOREG` is the macOS 27.0 (26A428) `ioreg` text of the dart-ane0 node:
ane-linux-experiments `d3dc1aef`,
`receipts/2026-09-23-m2-macos-denominator/m2-macos-window/ane-evidence/dart-ane-nodes.txt`.
The script also reads the iBoot image
`receipts/2026-09-20-iboot-j414c/iboot_j414c_dec.bin` (macOS 26.6.2 25G83,
sha256 `818990e1…`). Stdlib only; its asserts check the parse (151 writes,
26 bridge writes in events 2816-2841, the RMW match of every traced
tunable, two iBoot tables of 22 records).

| file | content |
|---|---|
| `mmio_diff.py` | the parse and diff script |
| `macos-trace-writes.tsv` | the 151 writes with block and phase |
| `macos-trace-blocks.tsv` | step 1, per block |
| `linux-footprint.tsv` | step 2, per block, with source lines |
| `linux-baseline-counts.tsv` | the 2026-09-24 baseline per block, and which phases the default module still runs |
| `diff-ranked.tsv` | step 3 |
| `dart-tunables.tsv` | ADT, trace, iBoot and Linux per DART word |
| `dapf-ane.tsv` | the DAPF allow-list |
| `tlb.txt` | the TLB arithmetic |
