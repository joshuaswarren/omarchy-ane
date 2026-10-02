# 2026-10-02-ane-gen-h15: M3 family (H15) ANE SoC data, design, plan

Actor: AneGenH15 (subagent of Main). Host: omp-studio-local. Static, no hardware touched.
Time box: ~120 min. Branch: `agent/ane-gen-h15` from `origin/main` 87e1df9.

## Source ledger

| id | kind | ref | sha256 |
| --- | --- | --- | --- |
| S1 | ipsw-adt | macOS 27.0 26A428 ADTs (j433, j434, j504, j613, j615 for T8122; j514s, j516s for T6030; j514c, j514m, j516c, j516m, j575d for T6031; j514m, j516m for T6034) under artifacts/AneAllSoc/every-soc/receipt/adt-27.0.txt and adt-13.5-ane.txt | per artifacts/AneAllSoc/every-soc/SHA256SUMS |
| S2 | buildmanifest | macOS 27.0 26A428 BuildManifest.plist, receipt artifacts/AneAllSoc/every-soc/receipt/firmware-27.0.txt | per artifacts/AneAllSoc/every-soc/SHA256SUMS |
| S3 | kernelcache-kext | NOT FETCHED this run; the M3-class kernelcache would be fetched on demand via HTTP range read of the IPSW zip (BuildManifest lists the 27.0 kernelcache by build id) | — |
| S4 | aurora-dts | aurora-silicon/linux asahi-7.1.13-3 tag 94fb2334 (read-only via git show REF:path; not modified) | per arch/arm64/boot/dts/apple/*.dtsi; ANE-relevant files: t6030.dtsi, t6030-pmgr.dtsi, t6031.dtsi, t6031-die0.dtsi, t6031-pmgr.dtsi, t6034.dtsi, t8122.dtsi, t8122-pmgr.dtsi |
| S5 | community-row | lab survey 2026-08-27 (private) + receipts/2026-09-05-ane-community/anecompile-cross-target.json (h15 = subtype 6, hwx_bytes=49152, ISA 17/subtype 7) | per receipts/2026-09-05-ane-community/SHA256SUMS |

## Chip-to-marketing map

Marketing names are Apple's external product names. SoC ids are Apple's silicon
part numbers. The mapping is the public one (Apple's Mac lineup + the IPSW
device-class identifiers in BuildManifest).

| Marketing | SoC id | Internal | Boards (j-id) | Source |
| --- | --- | --- | --- | --- |
| M3 | T8122 | H15G | j433, j434, j504, j613, j615 | S1 (ADT arm-io,t8122), S2 (BuildManifest: t8122 j433ap..j615ap), S5 (H15G = "M3 base") |
| M3 Pro | T6030 | H15J (Lobos) | j514s, j516s | S1 (ADT arm-io,t6030), S2 (BuildManifest: t6030 j514sap/j516sap ANE=t603x_ane0_fw_erebus_ls5x.im4p), S4 (t6030.dtsi comment: 'Apple T6030 "M3 Pro" SoC, Other names: H15J, "Lobos"') |
| M3 Max | T6031 | H15J | j514c, j514m, j516c, j516m, j575d | S1 (ADT arm-io,t6031), S2 (BuildManifest), S4 (t6031.dtsi comment: 'Apple T6031 "M3 Max" SoC') |
| M3 Ultra (not shipped) | T6034 | H15J (dual-die T6031) | j514m, j516m (T6034 .dtsi exists in kernel S4; no shipped board) | S4 (t6034.dtsi includes t6031.dtsi), S1 (no shipped T6034 board in 27.0) |

The lab survey 2026-08-27 had T6034 as "M3 Ultra"; that is wrong — M3 Ultra
never shipped. T6034 is a kernel-reserved SoC for a future M3 Ultra (analog
to T6022 = M2 Ultra or T6002 = M1 Ultra). See data/ane-soc/t6034.json for the
note.

## Per-SoC data files

| File | Source citations | State |
| --- | --- | --- |
| data/ane-soc/t8122.json | S1, S2, S4, S5 | data-only |
| data/ane-soc/t6030.json | S1, S2, S4, S5 | data-only |
| data/ane-soc/t6031.json | S1, S2, S4, S5 | data-only |
| data/ane-soc/t6034.json | S1, S2, S4, S5 | data-only (no shipped SKU) |

Every leaf value is `{v, src}` or `{v: null, reason}` per the shared schema.
Null values: dart.sid, dart.vm_base, dart.vm_size, mailbox.interrupts ordering
detail, kext.class, kext.protocol, firmware.sha256, iommu-parent phandle.
Each null has a reason describing what would unblock it (HTTP range re-extract
of the ADT, or kernelcache fetch S3).

## Missing for a future overlay (re-extract / re-fetch)

1. T8122/T6030/T6031: iommu-parent phandle in the ane ADT node (not in
   the receipt summary script; the 13.5 detail script preserved it as
   'iommu-parent = 0x14b -> ?' on dart-ane0 j314c). Re-run the detail
   script on the 27.0 ADTs to obtain the phandle. Without it, the data-
   only overlay cannot write a working `iommus = <&ane_dart 0>` line.
2. T8122: the kernel S4 has only 4 asc-mailboxes (smc, aop, mtp, ans);
   the ANE's mailbox at 0x311050000 is not yet an exposed mbox node.
   Until the kernel adds an ane_mbox, the data-only overlay cannot
   wire `mboxes = <&ane_mbox>`. The T6021 overlay pattern (target-path
   to a known-good mailbox) does not apply here because the address
   doesn't match any of the four existing mboxes.
3. T8122/T6030: ANE_CPU pmgr power domain. ADT says gates
   ANE_CPU + ANE-SYS-V. The kernel pmgr S4 for these SoCs defines
   only ps_ane_sys. Need to add ps_ane_cpu (T8122: 0x2d070c000+0x4
   per the ADT pmgr list; T6030: 0x35070c008+0x4 per the ADT pmgr
   list) before the data-only overlay can write
   `power-domains = <&ps_ane_sys>, <&ps_ane_cpu>`. T6031 has all
   five labels (ps_ane_sys/mpm/cpu/td/base) in the kernel.
4. All M3 SoCs: DART sid/vm_base/vm_size from the ADT detail. The
   summary script does not preserve these. Need a second pass with
   the 13.5-style adt-ane detail script on the 27.0 corpus.
5. All M3 SoCs: kext class strings (AppleHxxANEInterface version +
   CSNE_CMD_* symbols) from the 27.0 kernelcache im4p. Fetched on
   demand via HTTP range read of the IPSW zip; the
   omarchy-ane-firmware-fetch range-read machinery is the same
   pattern. The T8112 22G74 kext was fetched that way
   (artifacts/T8112Data/t8112-ane/kext, 1.07 MB); M3 kext is the
   same fetch pattern.
6. All M3 SoCs: firmware payload sha256 after im4p unwrap. The fetch
   script produces this; the M2 selene hash a9c4b771… is published
   in ane/t6021 source (driver sha-pin). M3 hash is fetched on demand
   and not in this run's notebook.

## Driver family and new-for-generation

Driver family: ane_t6021's RTKit/firmware model, extended.

Rationale (S1 + S4 + S5):
- H14 (T6021) and H15 (T8122/T6030/T6031) both use the apple,asc-mailbox-v4
  mailbox doorbell class (kernel S4: t6031-die0.dtsi:121, t6031-die0.dtsi:193,
  t6031-die0.dtsi:269, t6031-die0.dtsi:283; t8122.dtsi:833, t8122.dtsi:899,
  t8122.dtsi:995, t8122.dtsi:1059). The poll-TX apple-mailbox fix on
  omarchy-linux `86c727e6e` (per AGENTS.md lineage) covers both.
- H14 firmware is FUD-loaded (selene on 13.5 stub). H15 firmware (themis /
  erebus) is FUD-loaded too per BuildManifest pattern; the driver can boot
  the ASC firmware from a host-side fetch the same way ane_t6021 does.
- H14 has RTKit-class mailbox semantics; H15 is the same doorbell layout.
- H14's ane_t6021 driver does pmgr power-domain sequencing with
  ps_ane_sys, ps_ane_mpm, ps_ane_cpu, ps_ane_td, ps_ane_base; the
  M3 Pro/Max/Ultra family has the same set (kernel S4 t6031-pmgr.dtsi
  has all five labels at addresses 0x520/0x5a0/0x5a8/0x5b8/0x5c0
  matching the ADT pmgr list at S1).

What is new for H15 (vs H14):

| Aspect | H14 (T6021) | H15 (T8122/T6030/T6031) | Source |
| --- | --- | --- | --- |
| ANE node compatible | `ane,t8020` | `iop,ascwrap-v6` | S1 (ADT 27.0 ane0/ane nodes) |
| Number of ANE reg windows | 3 (engine 0x284000000+0x2000000, pmgr 0x28e080000+0x4034, pmgr_ps 0x28e08c000+0x4000) | T8122: 5, T6030: 8, T6031: 8 (an extra 0x6c000 ascwrap block at the ane base, plus 0x4000 mailbox, plus a firmware-data window) | S1 |
| DART compatible | `dart,t8110` | `dart,t8110` (same; T8110 family) | S1 |
| Mailbox compatible | `apple,t6021-ane-mailbox,apple,asc-mailbox-v4` | `apple,t6031-asc-mailbox,apple,asc-mailbox-v4` (T6030/T6031); T8122 will likely use `apple,t8122-asc-mailbox,apple,asc-mailbox-v4` (kernel dtsi pattern) | S4 |
| DART IOVA width | 40-bit (0x100_0000_0000 base) | 40-bit (T8110 family); no evidence in the lab of a wider window yet | S1 + S4 (no IOVA width change) |
| Tunables (pmgr word offset) | 0x260/0x2e0/0x4000/0x4008/0x4010/0x4018/0x4020/0x4028/0x4030 (9 ps words) | T6031: 0x520/0x5a0/0x5a8/0x5b8/0x5c0 (5 ps words); T6030: 0x498 only (1 ps word); T8122: 0x438 only (1 ps word) | S1 + S4 |
| Power sequencing | sys-mpm-td-base-set1-set2-set3-set4-cpu (H14 J-class) | sys-mpm-cpu-td-base (T6031); sys-cpu (T6030/T8122 collapses mpm/td/base into sys-cpu) | S1 + S4 |
| ANE clock-ids | 4 (318-321) | ADT 27.0 receipt summary does not preserve clock-ids; T8110/T6020/T6021 family has 4. Not verified for T8122/T6030/T6031. | S1 (13.5 has them; 27.0 receipt summary omits) |
| RTKit mailbox endpoints | RX/TX doorbells at 0x285408000+0x4000 (T6021 j414c) | Doorbell at the ane_alt 0x6c000 block; the 4-IRQ doorbell pattern at 0x3c9050000+0x4000 (T6031) / 0x309050000+0x4000 (T6030) / 0x311050000+0x4000 (T8122) | S1 + S4 (kernel dtsi mailbox pattern) |
| iommu-parent | phandle to a single dart-ane0 | phandle to dart-ane (T8122) or dart-ane0 (T6030/T6031) | S1 (13.5 detail; 27.0 summary omits) |
| ascwrap block | 0x6c000 not present in H14 | 0x6c000 ascwrap block at ane base (T8122 0x311400000, T6030 0x309400000, T6031 0x3c9400000) | S1 |
| Firmware model | RTKit + EPMAP + CSNE_CMD (T6021) | Same RTKit + EPMAP + CSNE_CMD model; the ascwrap-v6 mailbox may add a new 'ane_ping' or doorbell layout, but the H14 RTKit client is the starting point | S5 (design inference; kext S3 not extracted) |

## Compiler gap (mil-hwx-compiler)

The cross-target compile on macstudio 2026-09-05 already emits H15 HWX with
subtype 6, 49152 bytes (S5, receipts/2026-09-05-ane-community/anecompile-cross-target.json).
The lab has:
- One M1 Ultra running macOS 26.6.2 build 25G83 that cross-compiles
  oracles for h13..h17 (including h15).
- mil-hwx-compiler with H15 target emitting 49152-byte HWX
  (subtype 6; ISA 17/subtype 7).
- A H14 backend (172 oracle records, complete block tables) but no
  H15 backend. The H14 → H15 delta is unknown; H15 may add new task
  descriptors (the ascwrap-v6 mailbox may need new CSNE_CMD_* opcodes).

What is needed for H15 to execute (estimate, 5–10 days):
1. Reverse H15 task descriptors from 27.0 kernelcache (S3; the H15 ANE
   class binary is in kernelcache.release.mac1? in the 27.0 IPSW). This
   is an m1n1 proxy LZFSE + im4p unwrap + class-dump pass.
2. Identify the H15-specific CSNE_CMD_* opcodes (compare H14 172
   records to H15; expect ~190+).
3. Add an H15 backend to mil-hwx-compiler (the freedomtan parser table
   maps h15 → subtype 6; the ISA template is 17/7). The H14 backend
   template is the starting point.
4. Validate the H15 HWX on macstudio via ANECCompile (M1 Ultra running
   macOS 26.6.2; the M1 Ultra H15 firmware path may or may not exist —
   the M1 Ultra is H11, not H15. Cross-compile only, no execution on
   the oracle machine).
5. Once the kernel driver can run H15 HWX on an M3 host (not the M1
   oracle), validate end-to-end on hardware (estimate 5–10 more days,
   tester with M3 Pro or M3 Max Linux).

Risk: H15 may add TM/TQ semantics that the H14 backend never exercised
(mailbox CSNE_CMD_*). Without the H15 firmware image to inspect, the
H15 backend is a cross-compile-only oracle and will not execute on
real hardware until a tester with M3 silicon is online.

## Aurora state (S4)

The aurora-wip kernel tree (asahi-7.1.13-3 tag 94fb2334) has:
- T8122.dtsi: soc node, ps_ane_sys at 0x438. NO ane node, NO ane_mbox, NO
  ane_dart, NO ps_ane_cpu/pmgr window for ANE. M3 base is incomplete.
- t8122-pmgr.dtsi: ps_ane_sys (1 power domain, reg 0x438+4). M3 base has
  the simplest power plan of the M3 family.
- t6030.dtsi, t6030-pmgr.dtsi: ps_ane_sys (1 power domain, reg 0x498+4).
  NO ane node, NO ane_mbox, NO ane_dart, NO ps_ane_cpu. M3 Pro is
  incomplete.
- t6031.dtsi, t6031-die0.dtsi, t6031-dieX.dtsi, t6031-pmgr.dtsi: ane-
  mpm/cpu/td/base labels at 0x520/0x5a0/0x5a8/0x5b8/0x5c0 (5 power
  domains, M3 Max has the full set). NO ane node, NO ane_mbox, NO
  ane_dart. M3 Max has the most complete pmgr but still no ane node
  or mailbox or DART.
- apple,asc-mailbox-v4 class is in the kernel S4 for t6031-die0.dtsi
  and t8122.dtsi. poll-TX apple-mailbox driver change
  omarchy-linux `86c727e6e` works on the v4 mailbox.

What aurora-wip needs for H15 (estimate, depends on driver work):
- An ane node per SoC (compatible `apple,t8122-ane` / `apple,t6030-ane`
  / `apple,t6031-ane`, reg from the JSON, power-domains from pmgr dtsi,
  mboxes from a new ane_mbox node, iommus from a new ane_dart node).
- An ane_mbox node per SoC (compatible `apple,asc-mailbox-v4`, reg
  from the JSON mailbox.reg).
- An ane_dart node per SoC (compatible `apple,t8110-dart`, reg from
  the JSON dart.reg).
- For T8122/T6030: ps_ane_cpu pmgr word (reg 0x2d070c008+4 on T8122,
  reg 0x35070c008+4 on T6030) so the driver can sequence the cpu gate.

## Staged plan to a working opt-in experimental driver

Honest estimates; the largest unknown is whether the H15 mailbox doorbell
layout matches the H14 RTKit client (likely, but not proven) and whether
the H15 firmware file unpacks to a known RTKit binary (likely yes, but not
proven).

| stage | what | who_runs | risk | estimate (agent-days) | needs hardware |
| --- | --- | --- | --- | --- | --- |
| 0 | Re-extract ADT detail for T8122/T6030/T6031 to obtain iommu-parent phandle, dart sid/vm_base/vm_size, clock-ids, mailbox interrupt ordering. | AneGenH15 (this run only partial) | low (HTTP range + LZFSE; ADT corpus already on disk) | 0.5 d | none |
| 1 | Fetch 27.0 kernelcache im4p and extract the H15 ANE kext (AppleHxxANEInterface version + CSNE_CMD_* symbols). Confirm ascwrap-v6 mailbox doorbell layout matches H14 RTKit client assumptions. | AneGenH15 + AneProbeTool | medium (the kext may add a new CSNE_CMD_* opcode that needs a new kernel-side handler) | 1.5 d | none |
| 2 | Add ane_mbox + ane_dart + ane nodes to aurora-wip for t8122/t6030/t6031. Add ps_ane_cpu pmgr word for T8122/T6030. | AneDataOnlyFramework (kernel side) | low (T6031 pmgr is already complete) | 1.0 d | none |
| 3 | Write data-only overlays for t8122/t6030/t6031. Verify dtc/fdtoverlay applies them to lab DTBs; verify ane node enabled with the expected compatible. | AneDataOnlyFramework (once tools/validate_ane_soc.py lands) | low | 0.5 d | none |
| 4 | Add ane_t6021's RTKit/firmware model extension to omarchy-ane: probe the ascwrap-v6 ane node, boot the themis/erebus firmware from a host-side fetch (same path as ane_t6021_fwload), RTKit handshake, EPMAP, CSNE_CMD. | AneGenH15 | high (the ascwrap-v6 mailbox may not match H14's poll-TX semantics; if the doorbell is wired differently, the RTKit client needs a new doorbell/recv path) | 3.0 d | none (compile/test on the lab CT) |
| 5 | M3 Pro (T6030) boot + firmware load + RTKit handshake on real hardware. | AneGenH15 + an M3 Pro tester | very high (touching the engine on an unproven mailbox can wedge the SoC, requiring a power button; one failed boot is recoverable per the safety list, two in a row is a power-button ask) | 2.0 d | M3 Pro Linux (no M3 Pro in fleet yet) |
| 6 | M3 Max (T6031) boot + firmware load + RTKit handshake. | AneGenH15 + an M3 Max tester | very high (same as M3 Pro but with ane0/ane1 dual-die) | 2.0 d | M3 Max Linux |
| 7 | M3 (T8122) boot + firmware load + RTKit handshake. | AneGenH15 + an M3 tester | very high (the M3 base has the simplest pmgr but the smallest fw + the new ascwrap-v6 mailbox) | 2.0 d | M3 Linux |
| 8 | Compile-runner for H15 HWX (H15 backend in mil-hwx-compiler). | mil-hwx-compiler (separate repo) | medium (HWX is already 49152 bytes; ISA is the same as H16/H17; the templates are H14-derived) | 5–10 d | none (cross-compile oracle on macstudio) |
| 9 | H15 HWX execute on M3 hardware (Parakeet, Qwen). | AneGenH15 + a tester | high (depends on stages 5–8; this is the 'models on H15' stage) | 5+ d | M3 Pro or M3 Max Linux |

Total estimate (stages 0–7): 12.5 agent-days, 1 tester with M3 silicon and
the Asahi stub (no disk password; one power-button ask at most per failed
boot, per the ane-fleet-verify-before-human rule).

Risk: H15's mailbox may be sufficiently different from H14 that the H14
RTKit client cannot drive it. The mitigations: (a) the apple,asc-mailbox-v4
class is shared (kernel S4), so the doorbell layout should match; (b) the
27.0 kext extract (stage 1) confirms before any hardware work.

Out of scope: T6034 / M3 Ultra. No shipped SKU. Kernel has t6034.dtsi but
the T6034 hardware was never sold. The data-only state for T6034 documents
this and the JSON has nulls where appropriate.

## Files in this branch

```
data/ane-soc/t8122.json     # M3 base; reg/IRQ/pmgr/firmware cited; dart.sid + kext = null + reason
data/ane-soc/t6030.json     # M3 Pro; same shape
data/ane-soc/t6031.json     # M3 Max; same shape; j575d ane1 included
data/ane-soc/t6034.json     # M3 Ultra placeholder; no shipped SKU
receipts/2026-10-02-ane-gen-h15/README.md   # this file
```

No overlays yet: the iommu-parent phandle is missing from the receipt
summary (per the rule "ONLY if the ADT gives enough cited values to write
the nodes (reg, interrupts, iommus, power domains); otherwise only the
JSON and say what is missing"). Stage 0 of the plan re-extracts the ADT
detail; stage 2/3 produce the overlays.

## Branch and PR

- Branch: agent/ane-gen-h15 from origin/main 87e1df9.
- No PR: the shared rule (per AneGenH15 directive) is to open a PR only
  when the framework agent's tools/validate_ane_soc.py + CI gate land.
  This run delivers data + report; the PR will follow stage 3 of the
  plan when the framework tooling is ready.

## Not verified

- macOS 27.0 26A428 kernelcache not fetched (S3). The H15 ANE kext
  class name, version, and CSNE_CMD_* symbols are not in this run's
  notebook.
- 27.0 ADT detail (dart sid, vm_base, vm_size, iommu-parent phandle,
  clock-ids) not in this run's notebook. Receipt script preserves a
  subset; the 13.5 detail script was not re-run for 27.0.
- Firmware payload sha256 (after im4p unwrap) for h15_ane_fw_themis_j51y
  / t603x_ane0_fw_erebus_ls5x / t603x_ane0_fw_erebus_pc5x. The fetch
  script produces this; the M2 selene hash a9c4b771… is published; M3
  hash is not yet captured.
- M3 / M3 Pro / M3 Max hardware is not in the lab fleet. No M3 Linux
  boot was attempted. The driver plan is stages 5–7; stages 0–4 are
  static.
- T6034 / M3 Ultra: no shipped SKU in 27.0. The data is a placeholder.
