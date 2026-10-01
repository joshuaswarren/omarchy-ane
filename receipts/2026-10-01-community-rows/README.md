# Community rows against the ANE overlays and driver data (2026-10-01)

Static check. No Mac was touched. The public mlx-omarchy community dataset was read on 2026-10-01 at 23:12Z:
96 rows (42 deep, 51 quick, 3 e2e; sha256 of the sorted row-id list `5315105e56745c2c…`).
A row is cited by the first 12 hex digits of its content sha256. This receipt keeps derived values and row ids
only; the rows themselves stay in the dataset.

Sources of our values: `packaging/dt/t600x-ane.dtsi`, `t602x-ane.dtsi`, `t6020-ane.dts`, `t6022-ane.dts`,
`t8112-ane.dts`, `ane/t6021/ane_fw_validate.h`, `ane/t6021/ane_t6021_rtclient_main.c` (origin/main 91f7056), and
their receipts ([every SoC](../2026-10-01-ane-every-soc/README.md), [T8112](../2026-10-01-t8112-ane/README.md),
[T602x own memory](../2026-10-01-t602x-independent/README.md)).

## Discrepancies

| # | Value | Ours | Community rows | Verdict |
| --- | --- | --- | --- | --- |
| D1 | T6020 `RTK_soc_revision` (chip revision that iBoot writes into the ANE firmware) | 0x11 for every T602x (`ANE_T602X_SOC_REVISION`, the T6021 capture; marked as an assumption for T6020) | macOS `ANEDevicePropertyANEMinorVersion` = 1 on all four T6020 rows: 4e8224a39f40, e6069fe959ad (J416s, 26.5.1, kext 9.511.3), dc53badc436a, ea8c35bd20bf (J414s, 26.6.2, kext 9.512.0). = 17 (0x11) on T6021 2b83108f0645, f209393a8d2d (kext 9.512.0, the same kext) and T6022 90c6b9bf3e03. 17 = 0x11 = the T6021 preload capture. | **Wrong for the measured T6020 parts (0x01).** The 26.6.2 iBoot computes `RTK_soc_revision` from two eFuse words (j414c iBoot, function at 0x46c24, read by the T8112 opt-in work), so it is the chip revision of each part. The fix (a per-SoC `soc_revision` in `struct ane_t602x_soc`, T6020 0x01) is in the T8112 opt-in change, not in this one; until it merges, `ane_t6021` on T6020 replays 0x11. |
| D2 | T6020 ASC tunables | the 24 T6021 capture records for every T602x | not in any row | **Wrong for T6020.** The 26.6.2 j414s iBoot has the same 24 offsets with 13 different values (T8112 opt-in work). The fix is in the same change as D1. |
| D3 | `ane_soc_from_collect.py` (mlx-omarchy) on T6022 | — | refuses T6022: it takes `ane_nodes[0]`, and on 90c6b9bf3e03 that is `ane1` (die 1) | Tool defect, not an overlay defect. Sent to the collector lane. |
| D4 | IRQ decode of old macOS rows | — | `IOInterruptSpecifiers` is stored as Python bytes repr on a3e974f8e6f9, 857e1e6c61b0 and the M1 Ultra rows; `ane_soc_from_collect._irq_number` reads `[b't\x03..']` as 0x7427625b, not 884. On the M5 row 9610cb9f861d the dart-ane value is lossy (U+FFFD) and cannot be decoded. | Tool and collector defect. Decoded here with `ast.literal_eval` (scripts/extract.py). |

No address, interrupt, DART window, power-domain path, label or compatible in our overlays disagrees with a row.

## T6020 (M2 Pro)

43 rows: 19 Linux with a device tree block (j414s 13, j416s 6; linux-asahi 7.1.13-3-1 and -3-2), 5 macOS with
ANE nodes. macOS IORegistry addresses are arm-io relative; the physical address adds 0x200000000.

| Value | Ours (source) | Community rows | Agree | Verdict |
| --- | --- | --- | --- | --- |
| engine reg | 0x284000000+0x2000000 (13.5 ADT j414s/j416s/j474s) | ane0 reg[0] 0x84000000/0x2000000: 4e8224a39f40, e6069fe959ad, dc53badc436a, ea8c35bd20bf, 857e1e6c61b0 | yes | confirmed on 26.5.1 and 26.6.2 |
| pmgr reg | 0x28e080000+0x4034 | ane0 reg[1] 0x8e080000/0x4034: same 5 rows | yes | confirmed |
| SET reg | 0x28e08c000+0x4000 | ane0 reg[2] 0x8e08c000/0x4000; `set_base_candidate` 0x8e08c000, driver_window_confirms true: 4 rows (not 857e1e6c61b0, older collector) | yes | confirmed |
| ANE IRQ | AIC 884 | 0x374 = 884, die 0: 5 rows | yes | confirmed |
| DART windows | 0x285800000, 0x285810000, 0x285820000, each 0x4000 | dart-ane0 reg 0x85800000, 0x85810000, 0x85820000 (+0x85804000), each 0x4000: 5 rows | yes | confirmed; the fourth window 0x285804000 (ADT DAPF instance) is not used by the overlay |
| DART IRQ | AIC 885 | 0x375 = 885: 5 rows | yes | confirmed |
| DART compatible | `apple,t6020-dart`, `apple,t8110-dart` | the same pair on all DARTs of the 19 Linux rows | yes | confirmed |
| AIC | `/soc/interrupt-controller@28e100000`, 4-cell interrupts | same path, `apple,t6020-aic`, 4 cells: 19 Linux rows | yes | confirmed |
| power-domain paths | pmgr 28e080000: 2c8 (pmp), 2e0, 4000, 4008, 4010, 4018, 4020, 4028, 4030 | every path exists with labels pmp, ane_cpu, ane_sys_mpm, ane_td, ane_base, ane_set1..4 on all 19 Linux rows (pmgr block reg 0x28e080000/0x8000; ane_sys at 0x260, not used) | yes | confirmed (scripts/paths.py: 0 missing) |
| node status | overlay applies when the kernel has no enabled `apple,t6020-ane` node | no row has an ANE node (`ane_node_present` false, 19 rows) | yes | the overlay applies on every reported kernel |
| ane-type | 160 (ADT j414c, same as T6021) | `hw_board_type` 160, ANEVersion 128, h14g, 16 cores, firmware loaded: 5 rows | yes | T6020 takes the T6021 kext path |
| RTK_soc | 0x6020 (compatible) | `platform.soc_id` t6020: 4 rows | yes | consistent (iBoot: 0x6020 or fuse bits) |
| chip revision | 0x11 | ANEMinorVersion 1 | **no** | D1 |
| ASC tunables | T6021 capture | none | — | D2 (from iBoot, not rows) |
| mailbox 0x285408000, IRQs 884 + 1833 | v3 lab overlay | none (the macOS probe walks only `ane*`, `dart-ane*`, `mapper-ane*`; stock trees have no node) | — | unmeasured |
| firmware placement (`segment-ranges`), reserved-memory | none needed: own memory | none | — | unmeasured; not needed by the own-memory path |
| DART vm-base / IOVA | `ane-alias-iova` 0x10000000000 (ADT vm-base) | none | — | unmeasured |
| boot chain | 13.5 selene image pinned (`a9c4b771…`) | iBoot2 8422.141.2, os-fw 13.5, m1n1 stage 2 v1.6.1 on all 19 Linux rows | yes | these Macs preload the 13.5 generation the driver pins; stock m1n1 adds no ane-firmware nodes |
| dart-id | not used | 30 (T6021 37, T6022 63/64) | — | ADT-only fact |

## T6022 (M2 Ultra)

1 row: 90c6b9bf3e03 (macOS 27.0, J475d). No Linux row.

| Value | Ours (source) | Community row | Agree | Verdict |
| --- | --- | --- | --- | --- |
| die-0 engine, pmgr, SET reg | as T6020 | ane0 0x284000000+0x2000000, 0x28e080000+0x4034, 0x28e08c000+0x4000 | yes | confirmed |
| die-0 ANE IRQ, DART windows, DART IRQ | 884; 0x285800000/810000/820000; 885 | 884 die 0; dart-ane0 same 3 (+0x285804000); 885 die 0 | yes | confirmed |
| die-1 ANE (left out of the overlay) | ADT ane1 0x2284000000, IRQ 884 on die 1, DARTs 0x2285800000 | ane1 0x2284000000+0x2000000, 0x228e080000+0x4034, 0x228e08c000+0x4000, IRQ 0x1374 (die 1, 884); dart-ane1 0x2285800000/810000/820000/804000, IRQ die 1 885, dart-id 64 | yes | confirmed; die 1 stays out (driver has die-0 addresses) |
| ane-type, instances | 160 | `hw_board_type` 160 on both instances; 2 instances, both firmware loaded | yes | — |
| RTK_soc | 0x6022 | `soc_id` t6022 | yes | — |
| chip revision | 0x11 (assumption) | ANEMinorVersion 17 = 0x11, both instances | yes | consistent with the row; one machine |
| AIC path, pmgr paths, DART compatible on a T6022 kernel tree | t602x-die0 paths (linux-asahi j180d, j475d DTBs) | no Linux row | — | unmeasured |
| mailbox, placement, tunables | as T6020 | none | — | unmeasured |

## T6000 (M1 Pro)

6 rows: 4 Linux with a device tree block (j314s c901d2bb47ba, 1c239648b330; j316s 8bdc4c196309, fbc20029b7fd),
2 without one (5486ff40b283, ada079186a54). No macOS row.

| Value | Ours (source) | Community rows | Agree | Verdict |
| --- | --- | --- | --- | --- |
| AIC | `/soc/interrupt-controller@28e100000`, 4 cells | same, `apple,t6000-aic`: 4 rows | yes | confirmed |
| pmgr paths | 268 (ane_sys), 2c8 (ane_sys_cpu) | same labels and paths: 4 rows | yes | confirmed |
| pmgr reg | overlay sets 0x1c000 (stock 0x4000) | 0x28e080000/0x4000: 4 rows | yes | as the dtsi comment says |
| DART compatible | `apple,t6000-dart` | present on the 4 rows | yes | confirmed |
| node status | no stock ANE node | `ane_node_present` false: 4 rows | yes | overlay applies |
| engine, IRQ 770, DART windows, IRQ 771 | 13.5 ADT j314s/j316s (= j316c) | no T6000 macOS row; the M1 Ultra rows below have the same die-0 values | — | unmeasured on T6000 itself |
| boot chain | iBoot loads the H13 firmware | iBoot2 8422.141.2, os-fw 13.5: 4 rows | — | — |

## T6002 (M1 Ultra), quick rows only

6 quick macOS rows (26.6.2; J375d: 755c2d8d3b9a, ffe77ec70b57; Mac13,2 with older collectors: 14fbcbe67589,
5e75a04c9727, 869e1c8ba779; e0079bffb2ea has no node data). Die 0: ane0 0x284000000+0x2000000,
0x28e080000+0xc02c, IRQ 770; dart-ane0 0x285800000/810000/820000/804000, IRQ 771 (5 rows): the `t600x-ane.dtsi`
values. Die 1: ane2 0x2284000000+0x2000000, 0x228e080000+0xc02c, IRQ 770 on die 1; dart-ane2 at 0x2285800000.., IRQ
771 on die 1. The live IORegistry has ane0 and ane2 only, not the ADT ane1 (0x508000000) or ane3.
`hw_board_type` 96, ANEVersion 96, h13g, ANEMinorVersion 17 on both instances (2 rows).

Missing for T6002: any Linux row (AIC path, pmgr paths and labels, DART compatibles, node status, boot chain,
kernel); any deep row; `set_base_candidate` has driver_window_confirms false (H13 has no separate SET window: the
SET words are inside reg[1] 0x28e080000+0xc02c), so `ane_soc_from_collect.py` refuses it; no `ane.ko` run.

## T8112 (M2)

4 rows: 137d85c4c8d8, eafd56edf365 (Linux deep, j413), 9a083a60b9aa (e2e, j413), cf234e0237d2 (macOS 26.6.2,
Mac14,2, no ANE port detail at all).

| Value | Ours (`t8112-ane.dts`) | Community rows | Agree | Verdict |
| --- | --- | --- | --- | --- |
| AIC | `/soc/interrupt-controller@23b0c0000`, 3-cell interrupts | same, `apple,t8112-aic`, 3 cells: 2 rows | yes | confirmed |
| ANE_SYS | pmgr 23b700000/4a8 (ps_ane_sys) | `ane_sys` at that path: 2 rows | yes | confirmed |
| pmgr block | children at 0xc008..0xc038 added | block reg 0x23b700000/0x14000 (covers 0xc038): 2 rows | yes | the new children are inside the block |
| DART compatible | `apple,t8112-dart`, `apple,t8110-dart` | both that pair and `apple,t8110-dart` alone in the trees | yes | confirmed |
| node status | no stock ANE node | false: 2 rows | yes | — |
| engine 0x26a000000, IRQ 520, DARTs 0x26b800000.., IRQ 521, vm-base, mailbox, SET base, power words | ADT + kext | none | — | unmeasured |
| chip revision, tunables | none in the overlay (the T8112 opt-in work handles them) | none | — | unmeasured |

## `ane_soc_from_collect.py` against our overlays

Run on all 96 rows (mlx-omarchy origin/main 416da0e6d). It emits candidates for t6020 and t6021 and refuses the
others (no macOS row for t6000, t6001, t8103, t8112, t8122; driver_window_confirms false for t6002,
t6030, t8142; D3 for t6022). The t6020 candidate has the same engine (0x284000000/0x2000000), IRQ 884, DART
windows, DART IRQ 885, AIC path, pmgr path and SET base (0x28e08c000) as `t602x-ane.dtsi`. The differences are
shape only: the generator writes the H13 model (one `engine` reg, no pmgr/SET reg, no mailbox, no
memory-region), `apple,t6020-dart` without the `apple,t8110-dart` fallback that the rows' DARTs carry, every
DART in the `ane_cpu` domain, and five ANE power domains (ane_cpu and ane_set1..4, not ane_sys_mpm, ane_td,
ane_base). No value conflicts.

## Fields the collector still lacks

Sent to the collector lane (w6Z) on 2026-10-01; `tools/promotion_check.py` reads the Linux names below.

Linux, new block `summary.ane_port_detail.runtime.omarchy_ane`:
1. `machine_id`, `owner_id`: first 16 hex digits of the sha256 of a random per-install token. Never a serial
   number, UUID or MAC address.
2. `check`: exit code, status (`ready`/`FAILED`), the UNTESTED flag and up to 40 lines of `omarchy-ane-check`.
3. `module`: name (`ane` or `ane_t6021`), version, srcversion and `parameters/*` from `/sys/module/<m>/`.
   Today the runtime probe reads `/sys/module/ane` and the `ane ` line of `/proc/modules` only, so `ane_t6021`
   is invisible.
4. `firmware`: path and sha256 of each `/lib/firmware/apple/ane/*`; `opt_in`: the `ane-*` lines of
   `/etc/omarchy-platform/dtb-overlays.opt-in`.
5. `smoke` (with explicit consent only): 20 calls of the encoder, one fp16 output sha256 per call, errors,
   min and median ms. Prerequisite: the package does not ship the encoder ANEC and inputs yet
   (`tools/parakeet_encoder_whole.py` is a lab tool), so no row can carry this until it ships.
6. `uptime_s`; `dmesg`: the first 200 lines (160 B each, timestamps kept) that match ane, ane_t6021, apple-dart,
   apple-mailbox or pmgr; `dmesg_faults`: those lines that also match a fault word.
7. Device tree: the ANE mailbox node (the `mboxes` target) and the `/reserved-memory` children named `ane*`.
   Only the lab m1n1 adds `ane-firmware` nodes; on stock m1n1 their absence is the fact.
8. `dtb_sha256` is null in all 59 rows with a Linux device tree block (`/sys/firmware/fdt` is root-only).

Size: the block needs its own 48 KiB cap (dmesg about 32 KiB, check 6.4 KiB, faults 5 KiB, smoke 1.4 KiB); drop
the dmesg tail first, never check, module, smoke or faults.

macOS (`ane_port_detail.macos`):
9. The IODeviceTree plane (`ioreg -a -p IODeviceTree -n <name> -l`), not IOService, for `ane`, `ane0..3`,
   `dart-ane*`, `mapper-ane*`, with a key whitelist: name, compatible, reg, interrupts, IOInterruptSpecifiers,
   segment-ranges, ane-type, ane-subtype, ane-id, die-id, die-ane-id, clock-gates, power-gates, iommu-parent,
   AAPL,phandle, vm-base, vm-size, page-size, sids, bypass-15, instance, dapf-instance-0, dart-id,
   dart-options; binary values as hex. `segment-ranges` is the iBoot firmware placement; no IPSW ADT has it.
10. Privacy: never dump the root or `/chosen`; drop serial-number, IOPlatformSerialNumber, IOPlatformUUID,
    mlb-serial-number, unique-chip-id (ECID), mac-address*, any *uuid*, boot-nonce, nonce-seeds, random-seed.
    A whitelist, not a blacklist.
11. `IOInterruptSpecifiers` as hex, one entry per specifier (D4).
12. Keep `ANEDevicePropertyANEMinorVersion` (D1: it is the chip revision). Find out why cf234e0237d2 has no ANE
    port detail.
13. Tools: pick ane0 (die 0) by name in `ane_soc_from_collect.py` (D3) and decode the bytes-repr IRQ form (D4).

## Promotion rule dry run

`tools/promotion_check.py --remote` (2026-10-01, 96 rows): t6000 6 rows, t6001 6, t6020 37, t6021 5, t8103 8,
t8112 3, t8122 1; 0 judged for every SoC (no row has the `omarchy_ane` block), so every SoC stays as it is. The
same result from the downloaded rows. `tools/test_promotion_check.py` covers each row criterion at its boundary,
each SoC count, a failing row that blocks, the one-board T6002 case and the SoCs with no golden.

## Reproduce

```
python3 scripts/query_community_data.py --source remote list --json      # mlx-omarchy; then fetch each row:
#   GET https://mlx-omarchy-community-data.joshua-s-warren.workers.dev/v1/results/<sha256>  -> ROWS/<sha>.json
python3 receipts/2026-10-01-community-rows/scripts/extract.py ROWS facts.json
python3 receipts/2026-10-01-community-rows/scripts/table.py facts.json ROWS      # per-SoC values + row ids
python3 receipts/2026-10-01-community-rows/scripts/paths.py ROWS packaging/dt    # overlay target paths per row
python3 scripts/ane_soc_from_collect.py ROWS/*.json --outdir gen                 # mlx-omarchy
tools/promotion_check.py --remote
```

The dataset grows; a later run can show more rows. The `.dtbo` bytes of t6000, t6001, t6002, t6020, t6021 and
t6022 are the same before and after this change (comment-only edits; dtc 1.7.2-g53373d13).

## Limits

- The rows measure what macOS and the booted Linux tree show. They do not measure the mailbox, the firmware
  placement, the ASC tunables or any Linux ANE run on T6000, T6002, T6020, T6022 or T8112.
- Rows from one machine can repeat (quick and deep from the same Mac); no row has a machine id, so the number of
  T6020 machines is at least two (two boards), not known exactly.
- D1 rests on the T6021 match (17 = 0x11 = capture) and on the iBoot eFuse code; no T6020 preload is captured.
