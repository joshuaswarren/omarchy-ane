# 2026-10-03-ane-h15: M3 family (H15) ANE b0

Actor: AneH15Driver2. Static desk work on the lab CT. No Mac was touched, nothing was
booted or loaded, and nothing was fetched from Apple. Branch `agent/ane-h15-b0` from
`origin/main` `16cfa87`.

Inputs: data that was already on disk. These are the AneAllSoc ADT and BuildManifest
receipts that the M3 data files cite, `receipts/2026-10-02-ane-gen-h15`, the four M3 data
files, the `ane_t6021` sources, and the aurora tree that the data files cite (read-only).
The output of the stopped AneH15Driver run (`artifacts/AneH15Driver/h15-b0`) was not used.

## Decision

The M3 ANE is `ane_t6021`-like at the ASC layer. It does not need a new ASC-IOP client.

- MEASURED: the macOS 27.0 ADT describes the M3 ANE as a generic ASC IOP
  (`iop,ascwrap-v6`). It publishes two windows at the same engine-relative offsets that
  `ane_t6021` programs on the M2 Max. Window 0 is the ASC wrapper (engine + 0x1400000,
  where `ane_t6021` sets CPU_CONTROL RUN at +0x44). Window 1 is engine + 0x1050000, which
  is RVBAR on T6021. The DART is at engine + 0x1800000 and has the T6021 four-window shape.
  This holds on T8122, T6030 and T6031, and on both dies of j575d.
- MEASURED: every other M3 ASC mailbox in the kernel tree is `apple,asc-mailbox-v4` at its
  ASC base + 0x8000, the same hardware class and offset as the T6021 ANE mailbox.
- So the plan reuses the kernel `apple_rtkit` client (HELLO, EPMAP and STARTEP live in
  `rtkit.c`), `apple-mailbox`, and the `ane_t6021` ASC start sequence. Only the per-SoC data
  changes, and the M3 ADT also names the mailbox interrupts, which the T6021 ADT does not.
- INFERENCE, not measured: the ANE mailbox at wrapper + 0x8000, the mailbox interrupt roles,
  and that the M3 firmware speaks RTKit at all. The T6021 transport that works today does not
  carry over by default. On the 13.5 selene image, RTKit HELLO never arrived on any recorded
  boot. Commands use the legacy ChMan ring and the IPI doorbell at engine + 0x1844000, a
  contract tied to that one image (R3, R4).
- The M2 ANE has no ASC wrapper version in the local data. Its ADT node is `ane,t8020`. The
  "ascwrap-v5" label in the AneGenH15 notebook hypothesis has no source.

b1 stays a plan (see the last section). Several table rows that b1 needs are INFERENCE or
missing, so no code is written.

## Sources

| id | path | sha256 |
| --- | --- | --- |
| A1 | `~/.local/share/apple-silicon-lab/artifacts/AneAllSoc/every-soc/receipt/adt-27.0.txt` (macOS 27.0 26A428 ADT summary) | `04fe38b8ee009700f02e2c01e83bf608afab5e843f5878a83f162973a1ffdb84` |
| A2 | `~/.local/share/apple-silicon-lab/artifacts/AneAllSoc/every-soc/receipt/firmware-27.0.txt` (BuildManifest ANE rows) | `3e17bc4d66bd523251436f46281495ff30baa9abf81e772270feb24c05594f7d` |
| A3 | `~/.local/share/apple-silicon-lab/artifacts/AneH15Driver2/h15-b0/logs/offsets.txt`, output of `scripts/h15_offsets.py` (`215c05d6…`) over A1 | `a49d23d3caca88bded85e3058b5337978a66931412057943368504cfd0fcc98d` |
| A4 | `~/.local/share/apple-silicon-lab/artifacts/AneH15Driver2/h15-b0/logs/aurora-s4.txt`: aurora `94fb2334` dtsi, `rtkit-helper.c` and `rtkit.c` lines, each file hashed | `4db8c28812e857de158326dee9b03cabc43900f9b7928c93fdd1fd70056781a5` |
| A5 | `~/.local/share/apple-silicon-lab/artifacts/AneH15Driver2/h15-b0/logs/probe-synthetic-m3.json`: `omarchy-ane-probe --root` on minimal M3 trees | `03ff2d444aacc2ea0f82f13db2f7d83be6d7f4702cd5d764bdccc2443fddf374` |
| R1 | `ane/t6021/ane_t6021.h` at `16cfa87` (lines 104-147, 158-163, 195-201, 282-303) | `e6ec12c9317f354939fc9505aa7673b978f1aab8ef99b89adb5ee2767612f947` |
| R2 | `packaging/dt/t602x-ane.dtsi` at `16cfa87` (lines 116-174) | `e48344f1f22fdce829c2125b9ae1222805f7b0ed023e4acc723e00fbb978cd87` |
| R3 | `receipts/2026-09-30-t6021-stock-mailbox/README.md` at `16cfa87` (lines 101-190, 200-220) | `e35edf71f7344305c934c27b9d725359cd97748cf07ace57e2557e2c5ea0a96c` |
| R4 | `docs/t6021-ane-bringup-findings.md` at `16cfa87` (§2, §3, §7, §20 lines 1120-1131 and 1250-1262) | `115fe2ef8e421a5e0c7f04147e7f3a5ccd05ca14461ce1023ee3fcbba5b60c1c` |
| R5 | `receipts/2026-10-02-ane-gen-h15/README.md` at `16cfa87` (payload hashes, BuildManifest flags, AppleASCWrapV6 addendum) | `1d006a171017aa2be64b85a6b9602a8f212c168daf70e2adcf4092c30cec98bf` |
| R6 | `tools/omarchy-ane-probe` at `16cfa87` | `4c9625bed91df17d09f93bd5e8f91a28193c5a5424265a4fdc8ffdd0daa4acbe` |
| R7 | `packaging/omarchy-ane-firmware-fetch` at `16cfa87` (FETCH table, lines 47-53) | `f4f293e4593c9a35415b2f5d2df582ced30d366599a6b3337ef54316b5cab29f` |

## Per-SoC table

T6034 (j514m, j516m) has the T6031 ADT nodes (A1 groups them under `arm-io,t6031`), so the
T6031 column applies to it. j575d is also `arm-io,t6031` in the ADT, but its BuildManifest
chip id is 0x6032 and it carries two ANEs (A2 line 19). Engine-relative offsets are
arithmetic on A1 values (A3).

### Identity and firmware

| Fact | T6021 (M2 Max, reference) | T8122 (M3) | T6030 (M3 Pro) | T6031 (M3 Max), T6034, j575d | Status | Source |
| --- | --- | --- | --- | --- | --- | --- |
| ADT ANE compatible (ASC wrapper version) | `ane,t8020`; no wrapper version named | `iop,ascwrap-v6` | `iop,ascwrap-v6` | `iop,ascwrap-v6` (j575d ane0 and ane1) | MEASURED | A1 |
| ane-type | 160 | 176 | 192 | 224 | MEASURED | A1 |
| BuildManifest chip id and boards | 0x6021 j414c j416c j475c | 0x8122 j433 j434 j504 j613 j615 | 0x6030 j514s j516s | 0x6031 j514c j516c; 0x6034 j514m j516m; 0x6032 j575d | MEASURED | A2 |
| 27.0 IPSW ANE image | `t602x_ane0_fw_selene_rc4x` | `h15_ane_fw_themis_j51y`, payload 1605632 B, `19b6a499…` | `t603x_ane0_fw_erebus_ls5x`, `43da9d56…` | `t603x_ane0_fw_erebus_pc5x`, `ddac37c7…`; j575d also `t603x_ane1_fw_erebus_pc5x` | MEASURED | A2, R5 |
| Image flags | not in these sources | IsLoadedByiBoot true, plain Mach-O, no KBAG | same | same | MEASURED | R5 |
| Image the Linux boot runs | 13.5 stub image, pinned `a9c4b771…`, 0x4C5B28 B | the stub OS's image; stub version unknown | unknown | unknown | T6021 MEASURED; M3 missing | R4 §2, R7 |
| macOS class for `iop,ascwrap-v6` | `H11ANEIn` binds `ane,t8020` | `AppleASCWrapV6` (IONameMatch `iop,ascwrap-v6`, read in H17 kernelcaches only) | same | same | INFERENCE for M3 | R5 addendum, `README.md:31` |

### Windows, mailbox and interrupts

| Fact | T6021 | T8122 | T6030 | T6031 (die 1 on j575d) | Status | Source |
| --- | --- | --- | --- | --- | --- | --- |
| Engine window | reg[0] 0x284000000+0x2000000 | reg[2] 0x310000000+0x2000000 | reg[2] 0x308000000+0x2000000 | reg[2] 0x3c8000000+0x2000000 (0x23c8000000) | MEASURED | A1, A3 |
| ASC wrapper (CPU_CONTROL +0x44, CPU_STATUS +0x48) | engine + 0x1400000; not an ADT window | reg[0] 0x311400000+0x6c000 = engine + 0x1400000 | reg[0] 0x309400000+0x6c000 | reg[0] 0x3c9400000+0x6c000 (0x23c9400000) | address MEASURED; M3 role INFERENCE | A3, R1:107-119 |
| RVBAR block | engine + 0x1050000; live read 0x00000001 (valid, entry 0) | reg[1] 0x311050000+0x4000 = engine + 0x1050000 | reg[1] 0x309050000+0x4000 | reg[1] 0x3c9050000+0x4000 (0x23c9050000) | address MEASURED; M3 role INFERENCE | A3, R1:104-109 |
| ASC mailbox | engine + 0x1408000 = 0x285408000+0x4000; CTRL words read live 0x00020001 | expected 0x311408000+0x4000, inside reg[0] | expected 0x309408000+0x4000 | expected 0x3c9408000+0x4000 (0x23c9408000) | T6021 MEASURED; M3 INFERENCE | R1:121-147, R2:150-152, A4 |
| Mailbox class | `apple,t6021-ane-mailbox`, `apple,asc-mailbox-v4` | expected `apple,t8122-asc-mailbox`, `apple,asc-mailbox-v4` | expected `apple,t6030-asc-mailbox`, v4 | expected `apple,t6031-asc-mailbox`, v4 | T6021 MEASURED; M3 INFERENCE (class of every other M3 ASC mailbox) | R2, A4 |
| ANE IRQs, ADT order | 884 | 507 506 509 508 524 | 559 558 561 560 576 | 965 964 967 966 983 (5061 5060 5063 5062 5079) | MEASURED | A1 |
| Last ANE IRQ = DART IRQ − 1 | 884 / 885 | 524 / 525 | 576 / 577 | 983 / 984 (5079 / 5080) | MEASURED | A3 |
| First four ANE IRQs | none in the ADT | block 506..509, ADT order +1 +0 +3 +2 | block 558..561, same order | block 964..967, same order (5060..5063) | MEASURED | A3 |
| Mailbox IRQ roles | recv-not-empty 884 plus a never-firing send-empty 1833 that no ADT node names | send-empty 506, send-not-empty 507, recv-empty 508, recv-not-empty 509, if the kernel's ascending order for its other mailboxes holds | 558..561, same rule | 964..967, same rule | T6021 MEASURED; M3 INFERENCE | R2:154-155, R3, A4 |
| Role of the last IRQ | 884 stormed at about 700,000/s while the mailbox ran; inferred as the ANE doorbell line | 524, same position | 576 | 983 | INFERENCE | R3 |
| Mailbox and endpoint names in local excerpts | RTKit system EPs 1, 2, 3, 4, 8, 0xa; app EPs INIT, T2FC, T2FH, T2HS, T2HC, T2HT | none: the ADT summary has no mailbox or endpoint fields, and no H15 firmware strings are local | none | none | MEASURED (absence in local data) | R1:286-303, A1 |
| RTKit versions the kernel client accepts | 11 to 12 | same client | same | same | MEASURED (kernel) | A4 `rtkit.c:75-76` |
| Host transport that works today | legacy ChMan ring + IPI doorbell engine + 0x1844000 after the SCRATCH handshake; no HELLO on any recorded 13.5 boot | unknown | unknown | unknown | T6021 MEASURED | R3, R4 §20 |
| Other ADT windows | reg[1] 0x28e080000+0x4034, reg[2] 0x28e08c000+0x4000 (pmgr, SET) | reg[3] 0x2d0700000+0x18000, reg[4] 0x2d0724000+0x4000 | reg[3] 0x350700000+0x18000, reg[4] 0x350724000+0x4000, reg[5] 0x3503c0000+0x24000, reg[6] 0x211000000+0xf74000, reg[7] 0x3642c8000+0x4000 | reg[3] 0x292280000+0x10000, reg[4] 0x292290000+0x4000, reg[5] 0x2903c0000+0x30000 (the same address on die 1), reg[6] 0x404000000+0x2000000, reg[7] 0x2a02dc000+0x4000 | address MEASURED; reg[5..7] role unknown | A1, A3 |

### Power and DART

| Fact | T6021 | T8122 | T6030 | T6031 | Status | Source |
| --- | --- | --- | --- | --- | --- | --- |
| ADT pmgr words | SYS 0x28e080260, CPU 0x28e0802e0, SYS_MPM 0x28e084000, TD +8, BASE +0x10, SET1..4 +0x18..+0x30 (9) | SYS 0x2d0700438 (parent LMX4), MPM 0x2d070c000, CPU +8, TD +0x10, BASE +0x18 (parents TD and CPU) (5) | SYS 0x350700498 (LW20), MPM 0x35070c000, CPU +8, TD +0x10, BASE +0x18 (TD) (5) | SYS 0x292280520 (AFNC5_LW0, AFNC6_LW0), MPM 0x2922805a0, CPU 0x2922805a8, TD 0x2922805b8, BASE 0x2922805c0 (TD) (5) | MEASURED | A1 |
| ANE pmgr words inside the ANE node's pmgr window | yes (reg[1]) | yes (reg[3]) | yes (reg[3]) | yes (reg[3]) | MEASURED | A1, A3 |
| ANE node gates | ANE-SYS-V | ANE_CPU, ANE-SYS-V | ANE_CPU, ANE-SYS-V | ANE_CPU, ANE-SYS-V | MEASURED | A1 |
| Kernel power-domain nodes (aurora `94fb2334`) | all nine, used by the overlay | `ps_ane_sys` @0x438 only | `ps_ane_sys` @0x498 only | all five @0x520 0x5a0 0x5a8 0x5b8 0x5c0, parents as in the ADT | MEASURED | R2, A4 |
| DART compatible and windows | `dart,t8110`; 4 × 0x4000 at engine + 0x1800000, +0x1810000, +0x1820000, +0x1804000 | same shape at 0x311800000 | same at 0x309800000 | same at 0x3c9800000 (0x23c9800000) | MEASURED | A1, A3 |
| DART IRQ and gate | 885, ANE-SYS-DART | 525, no gate | 577, no gate | 984, ANE-SYS-DART | MEASURED | A1 |
| DART use by the driver | three DART nodes (first three windows), IRQ 885 shared, dart0 in the pmp domain, dart1 and dart2 in ane_cpu, sid 0 | same split expected | same | same | T6021 MEASURED; M3 INFERENCE | R2:116-148 |
| DART sid, vm-base, vm-size, iommu-parent | from the 13.5 ADT and the lab tree | not in A1 | not in A1 | not in A1 | missing | A1 |
| Firmware IOVA map | ADT segment-ranges: PA 0x10000848000 → IOVA 0x10000000000 (0xc4000, TEXT), PA 0x10001400000 → IOVA 0x100000c4000 (0x438000, DATA); the ANE DART is off at the iBoot handoff and holds no map | not in A1 | not in A1 | not in A1 | T6021 MEASURED; M3 missing | R4 §3 |

## Corrections to the earlier H15 records

The data files change only where a fact changed. Each changed leaf keeps a reason that names
the old value.

- `mailbox.reg` was engine + 0x1050000 (0x311050000, 0x309050000, 0x3c9050000), cited to
  the ADT. The ADT gives the window, not its role, and that offset is RVBAR on T6021. It is
  now null, with the INFERENCE value wrapper + 0x8000 in the reason.
- `mailbox.interrupts` was a reordered list (for example 506, 508, 509, 507, 524), cited to
  the ADT. No source gives that order. It is now null. The ADT order stays in `ane.interrupts`.
- `t6030.json` `mailbox.compatible` was `apple,t6031-asc-mailbox`. Its own source,
  `t6030.dtsi`, uses `apple,t6030-asc-mailbox`. The T6031 and T6034 compatibles are now null
  too, because no ANE mailbox node exists to cite.
- `t6031.json` called j575d a "MacBook Pro 16-inch M3 Max, dual-die". The BuildManifest gives
  j575dap chip id 0x6032 with two ANE images. The board now records that chip id.
- `t8122.json` said the ADT ane node has no iommu-parent. The receipt summary does not print
  that field, so absence is not shown. The reason now says unknown.
- `driver_family` cited the compiler oracle. It is now a null leaf whose reason holds this
  decision and its basis.
- `receipts/2026-10-02-ane-gen-h15` says that T6030 and T8122 "collapse mpm/td/base into
  sys-cpu", and lists 0x498 and 0x438 as their only power words. That is the kernel tree. The
  ADT lists MPM, CPU, TD and BASE on both (A1). Its mailbox row, which puts the doorbell at
  engine + 0x1050000, is superseded by the table above.

## What an M3 owner can supply with omarchy-ane-probe

The probe runs on the owner's Mac under Linux, as a normal user. Root adds `genpd` and, when
`dmesg_restrict` is set, `dmesg`. It reads the running device tree, `/proc`, sysfs and
debugfs, and never maps a register (R6). On an M3 Linux boot today it supplies:

1. `soc`, `board`, `compatible`: the chip and board as the kernel names them. This shows
   whether j514m and j516m boot as `apple,t6034`, which compatible j575d gets, and which data
   file applies.
2. `kernel.release`, `kernel.version`: the kernel that a b1 module must build against.
3. `pmgr_ane_nodes`: the ANE power-domain nodes of the running tree, with reg, parents and
   status. This shows whether the owner's kernel has more than `ps_ane_sys` on T8122 or T6030.
4. `genpd` (root): the runtime on or off state of those domains when nothing uses the ANE.
   b1 may read the engine window only while the ANE islands are on.
5. `interrupts`: the `/proc/interrupts` lines for mailboxes and DARTs. They give the live AIC
   numbering of the other M3 ASC mailboxes on that machine.
6. `dmesg`: kernel lines that match ane, dart-ane or ascwrap.
7. `firmware`: on M3 this is the refusal of `omarchy-ane-firmware-fetch --check`, because
   there is no M3 pin (R7). It confirms that no firmware is installed.
8. `soc_table.dt_vs_table`: the running tree against `data/ane-soc/<soc>.json`.

Two existing probe defects limit item 8 (A5, not fixed in this change). The `boards` check
never matches: the data files hold `{value: "j433"}` objects or strings such as
"J604 / Mac16,1", and the tree gives `apple,j433`. This affects all 12 data files. Also,
`soc_table` raises `AttributeError` when `ane.reg` is an object, so it is null for
`t6034.json` (seen in A5), and by shape also for `t6050.json`, `t8140.json`, `t8142.json`
and `t8150.json`.

The probe cannot supply the inputs that b1 needs most:

1. The stub macOS version, `/proc/device-tree/chosen/asahi,os-fw-version`. It selects the
   image that iBoot preloads, so it sets the b1 firmware pin. The probe reads the whole tree,
   but prints only the root and the ANE nodes. Printing this string would be a one-field
   probe change.
2. That stub image: size, SHA-256, segment layout, and the other fields of `ane_fw_image`.
   It comes from the IPSW member of that version, in the way `omarchy-ane-firmware-fetch`
   gets the M2 image. The probe cannot read it.
3. ADT `ane` properties: segment-ranges (the firmware IOVA map), ASC tunables,
   iommu-parent, interrupt roles, clock-ids. The Linux tree does not hold them. They need the
   macOS IODeviceTree plane or a full ADT read.
4. ADT `dart-ane`: sid, vm-base, vm-size, page size.
5. The source of RTK_soc_revision. T6021 took it from preload captures, T8112 from eFuse.
6. The firmware protocol: whether HELLO comes at all, the RTKit version, and the EPMAP
   endpoint set.
7. The live ASC state after power-on: CPU_STATUS, RVBAR, and the mailbox CTRL words. Reading
   it needs register access, which the probe never does. That is the b1 smoke itself.

## b1 plan: experimental opt-in firmware-boot smoke (plan, not code)

No code is written. The b1 rows for mailbox address, IRQ roles, DART sid, firmware IOVA map,
firmware pin and RVBAR use are INFERENCE or missing, and no M3 is in the lab.

Shape: a small out-of-tree module, `ane_h15_smoke`, separate from the autoloaded `ane_t6021`.
It reuses `apple_rtkit` and `apple-mailbox` from the kernel and copies the ASC start
constants from `ane_t6021`. It does not bind the stock `apple,rtk-helper-asc4` driver to the
ANE. That driver sets RUN at probe with no preflight (A4 `rtkit-helper.c:112-116`), and an
unchecked release of the ANE CPU is the hazard that this plan must exclude.

Gates:

- No module alias and no autoload. It binds only to `apple,t8122-ane`, `apple,t6030-ane` or
  `apple,t6031-ane` from a non-data-only overlay that the owner applies with an explicit
  opt-in key. The shipped data-only overlays never apply.
- The module parameter `i_have_an_m3=1` is required. Without it, probe returns `-EPERM`
  before any MMIO.

Probe sequence (every step logs, and every failure undoes the steps before it):

1. Power up the ANE domains through genpd in ADT parent order (SYS, then MPM, CPU and TD,
   then BASE). Read each pmgr word, which lies outside the engine aperture, and require
   ACTUAL = 0xf. Otherwise stop.
2. Do a read-only preflight inside the engine aperture, only after step 1: CPU_STATUS
   (wrapper + 0x48), RVBAR (engine + 0x1050000), the mailbox A2I and I2A CTRL words
   (wrapper + 0x8110 and + 0x8114). Log the raw values. Do not read CPU_CONTROL (hostile on
   T6001), never touch engine + 0x1010000 (R4 §7), and keep out of the T6021 fatal-read range
   (R1:158-163) until H15 data clears it.
3. If CPU_STATUS shows the core running, iBoot started it. Then skip RUN and only attach
   RTKit. If the core is stopped, require a DART translation for the RVBAR entry IOVA. With
   the segment-ranges from the owner, map the preloaded segments as `ane_t6021_fwload` does.
   Without them, refuse.
4. Set CPU_CONTROL RUN. `apple_rtkit_boot` waits for HELLO with a 2 s deadline. Log the
   HELLO versions and the EPMAP bitmap. Start only the system endpoints that `rtkit.c` starts
   (crashlog, syslog, ioreport). Start no app endpoints and send no CSNE command.
5. On timeout or crash: clear RUN, quiesce, power down, and fail probe. Unload does the same.

Pass: HELLO in the 11 to 12 window, the EPMAP bitmap logged, and syslog lines from the
firmware. Fail: no HELLO within the deadline (which, as on T6021, points to a non-RTKit
contract), a DART fault, or any probe refusal. Either result fills the missing rows above.

Before b1 becomes code, these must be MEASURED: the mailbox address and IRQ roles, the DART
sid and firmware IOVA map, the stub firmware pin, and the RVBAR value at handoff. Then the
module builds W=1-clean for arm64 against the M2 tree and the aurora tree, and an M3 owner
runs it once with a fall-back boot entry.

## Files in this change

- `data/ane-soc/t8122.json`, `t6030.json`, `t6031.json`, `t6034.json`: the corrections above.
- `receipts/2026-10-03-ane-h15/README.md`: this file.
- `receipts/2026-10-02-ane-gen-h15/README.md`: an addendum that points here.
- `CHANGELOG.md`: one line.
