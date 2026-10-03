# Ultra die-1 ANE support: T6002 (M1 Ultra) and T6022 (M2 Ultra)

Status: design. No Ultra runs Linux today. The measured basis is the
2026-10-03 read-only capture of one M1 Ultra desktop (macOS 26.6.2) and the
13.5 ADT decode of both Ultra boards:
[receipts/2026-10-03-ultra-die1](../receipts/2026-10-03-ultra-die1/README.md).

## 1. What the measurement says

Die 1 is a pure +0x20_0000_0000 address translation of die 0, for every window,
on both families. Three things are not a pure translation:

1. Interrupts. Each die has the same AIC line numbers (ANE 770 T600x / 884
   T602x; DART 771 / 885), but die-1 lines sit at +4096 in the flat macOS ADT
   numbering (ane2 = 4866, dart-ane2 = 4867, T6022 ane1 = 4980). The aurora
   tree expresses this as `interrupt-parent = <&aic>` with `AIC_IRQ DIE_NO
   <line>`: the die number moves into the IRQ specifier, the line stays.
2. Firmware. Each ANE instance has its own image (T6002: `t600x_ane0..3_fw_eos_jc3x`;
   T6022: `t602x_ane0_fw_selene_rc4x` and `t602x_ane1_fw_selene_rc4x`).
3. The `ane-subtype` word differs (0 on ane0, 2 on ane2); meaning unknown.

Power-state names, order and offsets are identical per die; only the pmgr page
moves. DART count (3), SID use (sid 0 per DART) and compatible (`dart,t6000`,
`dart,t8110`) are identical per die. Clock/power gates carry a die flag
(bit 28) instead of separate names.

macOS drives one ANE per die on the desktop: the live tree has ane0 and ane2
only. The 13.5 IPSW ADT still lists ane1/ane3 and their DARTs; macOS boots with
them pruned, and aurora keeps their pmgr states `#if 0` ("seems to be disabled
on shipping hardware"). macOS shape at the IOService layer: two `H11ANEIn`
devices, both `FirmwareLoaded=Yes`, under one `H1xANELoadBalancer`
(`ANEDevicePropertyNumANEs=2`) that serves `aned` and direct-path clients.

## 2. Per-die descriptor

Today every descriptor is die-0-only:

- `ane.ko` (`ane/src/ane_drv.c`): `ane_soc_t6000.ps_base = 0x28e08c000`,
  compiled per compatible. The SET base is the one compiled constant by
  design: a wrong SET base external-aborts the SoC, so constants enter only
  from a source that proved the address on hardware.
- `ane_t6021` (`ane/t6021/ane_t6021_fwload.c`): `ane_t6022_soc` carries
  `pmu_pa = 0x28e084000` (die-0 page; the firmware's power service maps this
  PA at IOVA == PA) and the selene image pin.

Design, per die:

| Driver | Die-1 SET/pmgr base | Qualification | Firmware |
| --- | --- | --- | --- |
| `ane.ko` (T6002 die 1) | 0x228e08c000 | RECOGNIZED until a die-1 run; `tm_retention = true` as die 0 | none (host TM; iBoot preloads per instance, `pre-loaded=1` measured on ane2) |
| `ane_t6021` (T6022 die 1) | SET from the node reg; `pmu_pa = 0x228e084000` | die-0 tier until a die-1 run | `t602x_ane1_fw_selene_rc4x` pin |

Mechanism: the SET/ps base moves from compiled constant to node reg entry.
The T602x overlays already name `reg-names = "engine", "pmgr", "set"` on the
node; the T600x overlays get a third `set` entry (`pmgr + 0xc000`, 0x38 bytes
mapped). The driver prefers the node `set` base and falls back to the
descriptor. Qualification stays a descriptor property, keyed by
(compatible, SET base): die-1 bases enter as `ANE_RECOGNIZED` and bind only
with `ane.allow_unqualified=1` until a die-1 run proves them. A guessed SET
base stays impossible to load silently.

For `ane_t6021`, `pmu_pa` derives from the node: `set_base - 0x8000` (holds on
T6020/T6021/T6022 die 0: 0x28e08c000 - 0x8000 = 0x28e084000; T8112 does not
follow it and keeps its compiled `pmu_pa` + `ps_off`). The die-1 firmware pin
is a second image entry with its own hashes and iBoot patch replay
(`ane/t6021/ane_fw_validate.h`), keyed by the node's `apple,firmware-index`
property (MEASURED: the 13.5 BuildManifest names `Ap,ANE1` separately). If the
ane1 image is byte-identical to ane0 (missing fact, static check), the pin is
an alias and no new bytes enter the tree.

## 3. Second device, not one device

Decision: die 1 binds as a second accel device. One device that schedules
across both dies is rejected for v1.

The driver structure already gives each platform device everything it needs:

- `ane_platform_probe` runs once per DT node: `devm_drm_dev_alloc` per device,
  `drm_dev_register` per device. Two enabled nodes produce `/dev/accel/accel0`
  and `/dev/accel/accel1`. No minor allocation code changes.
- Per-device state is already per-`struct ane_device`: `engine_lock`,
  IOMMU domain, BO lists, `ane_stats` sysfs/debugfs, wedged flag. Two devices
  get two independent stats files and two independent wedge states. A wedged
  die 1 does not take down die 0.
- Runtime PM is per device (section 4).
- `libane` already enumerates: `device_open(dev_id)` walks
  `/dev/accel/accel0..63`, verifies the "ane" DRM name, and returns the
  dev_id-th device; `MAX_ANE_DEVICES = 2` already fits. `tools/ane-run` needs
  a `--dev` passthrough; `omarchy-ane-smoke` gains a `die` field and runs the
  20-call check per device.
- The omarchy-mlx worker (`docs/ane-worker.md`) owns one accel file per
  daemon; two dies mean two workers (or one worker that opens dev 1 instead of
  0), with no shared scheduler. macOS precedent: two `H11ANEIn` devices behind
  one balancer — Apple also ships two devices, not one merged device.
- T6021-family firmware state is per device by construction: the die-1 device
  boots its own firmware copy, its own RTKit handshake, its own DART domains
  and its own pmu page. No shared ChMan state exists in the driver.

A one-device cross-die scheduler would need a new UAPI, a cross-device lock
and a work-stealing model that nothing in the driver or libane models. The
two-device shape costs no abstraction and matches macOS.

## 4. Runtime PM per die

Each die gets its own genpd domains. The aurora pmgr template instantiates
every power state twice (die 0 and die-1 pass with `DIE_NODE`), so die-1
devices attach `ps_ane_sys_die1`, `ps_ane_sys_cpu_die1`,
`ps_ane_set0..4_die1` (T600x) or `ps_ane_sys_die1`, `ps_ane_cpu_die1`,
`ps_ane_sys_mpm_die1`, `ps_ane_td_die1`, `ps_ane_base_die1`,
`ps_ane_set1..4_die1` (T602x). The driver's existing per-device autosuspend
then gates each die independently: die 0 can idle at its autosuspend delay
while die 1 runs. No cross-die coupling is modeled; none is measured either
(missing fact). Conservative default: leave `autosuspend_ms` per device and
watch idle power per die in the collector.

## 5. Device tree shape

### 5.1 aurora in-tree (#155-style)

The aurora tree already models both dies with the `multi-die-cpp.h` template
pass: `t6002.dtsi` includes `t600x-die0.dtsi` + `t600x-dieX.dtsi` in the die-0
`soc@200000000` bus, then `t600x-dieX.dtsi` + nvme in the die-1
`soc@2200000000` bus (die-1 bus ranges translate child 0x2_x to parent
0x22_x). The ANE nodes live only in `t600x-die0.dtsi` / `t602x-die0.dtsi`
today; `t6022.dtsi:331` flips `&ane` to `apple,t6022-ane`; the t600x pmgr
template already creates die-1 ANE states via `DIE_NODE`.

Die-1 shape, following the tree's own conventions:

- New `t600x-ane-die1.dtsi` (and `t602x-ane-die1.dtsi`) with the ANE, its
  three DARTs and (T602x) the mailbox, node names and regs untranslated:
  `ane@284000000` under `&die1` (bus translation makes it 0x2284000000),
  `iommu@285800000` and siblings likewise. `DIE_NODE()`/`DIE_LABEL()` for the
  pmgr references: `&ps_ane_sys_cpu_die1`, `&ps_ane_set1_die1`, ...
- Interrupts: `interrupt-parent = <&aic>`; `interrupts = <AIC_IRQ 1 770 ...>`
  (T600x ANE), `<AIC_IRQ 1 771 ...>` (DARTs), `<AIC_IRQ 1 884/885>` (T602x).
  Same line numbers as die 0; the die cell does the routing.
- Compatibles unchanged: `apple,t6000-ane` on both T6002 nodes;
  `apple,t6022-ane` on both T6022 nodes (matching `dart,t6000`, which is also
  the same compatible on both dies). The kernel driver must therefore be
  per-node, which section 2 already makes true (SET base from the node).
- T602x die-1 extras: `memory-region` ane-alias IOVA (0x10000000000) is a
  die-0 DART address; the die-1 DART's vm base needs its own reservation
  (missing fact: die-1 dart vm-base). The die-1 mailbox keeps line 884
  (INFERENCE) and address 0x2285408000 (+0x20G, INFERENCE).
- The `#if 0` ps_ane1_* states (second instance per die) stay out: their
  devices are absent from the live tree.

### 5.2 omarchy-ane overlay

Two new overlays, opt-in like their die-0 parents:

- `packaging/dt/t6002-ane-die1.dts` (key `ane-t6002-die1`): die-1 DARTs by
  path under `/soc@2200000000`, die-1 pmgr controllers at
  `/soc@2200000000/power-management@228e080000/power-controller@268|2c8|c000..c028`,
  `ane@284000000` under `/soc@2200000000` with `reg-names = "engine", "pmgr",
  "set"`, `interrupts = <0 1 770 4>`, `iommus = <&ane_dart0 0 ...>` (die-1
  DART labels), `power-domains` = the die-1 states.
- `packaging/dt/t6022-ane-die1.dts` (key `ane-t6022-die1`): same shape over
  the T602x dtsi values (mpm/td/base/set1..4 states, mailbox, ane-alias IOVA
  reservation for the die-1 DART), `compatible = "apple,t6022-ane"`,
  `interrupts = <0 1 884 4>`.
- `packaging/dt/overlays`: two new lines, state `opt-in`.
- `omarchy,skip-if-compatible`: none on the die-1 overlays. The die-0
  overlays' `skip-if-compatible` guards the die-0 node only; a die-1 overlay
  must not list `apple,t6000-ane`/`apple,t6022-ane`, or `omarchy-ane-dt`
  would skip it whenever the kernel tree (or the die-0 overlay) already
  carries that compatible.
- `omarchy-ane-dt validate()` rules stay as-is and apply unchanged: the result
  tree must be stock plus new or changed nodes whose references resolve to
  enabled providers — the die-1 pmgr controller nodes must therefore be added
  by the same overlay (they are `status = "okay"` there), and `#power-domain-cells`/
  `#iommu-cells`/`#mbox-cells` references are checked as today. `status`
  prints both nodes (`ane` and `ane1`) when both apply.
- `tools/test_ane_overlays.py`: four new applications (t6002-j375d,
  t6022-j180d, t6022-j475d × die-1 overlays) plus the negative case (no
  opt-in key, no apply).

## 6. Test plan

1. Static, no hardware (CI-runnable): overlay build and application tests on
   the t6002-j375d / t6022-j475d / t6022-j180d DTBs (fixtures already in the
   repo), `validate()` refusals for unresolved phandles, `libane`
   `device_open(1)` unit test against two registered fakes, and the
   `pmu_pa = set - 0x8000` derivation table (T6020/T6021/T6022 die 0 rows).
2. Probe fields on first die-1 bind (T6002): journal shows two `loaded ane`
   lines; per-device `ANERD ps probe act=0xffffff` on both SET windows; genpd
   debugfs shows the `ps_ane_*_die1` domains raised only while die 1 resumes.
3. Smoke per device: `omarchy-ane-run --dev 0|1`; `omarchy-ane-smoke` emits
   `chip` plus `die` and runs 20 bit-exact calls per device against the H13
   fixture; `ane_stats` sysfs exists on both platform devices and counts
   independently (submit on die 0, check die 1 idle).
4. Runtime PM: autosuspend per device — open only accel1, watch die-0 domain
   stay suspended while die-1 work runs; then both idle and both suspend.
5. Firmware boot (T6022 die 1 only): the die-1 firmware reaches
   `RTKit HELLO` under `ane_t6021` with the ane1 pin; fault lines scoped by
   the die-1 DART/mailbox addresses.

## 7. Promotion rule extension (per die)

A row must say which die it ran. Changes:

- Collector (omarchy-mlx `scripts/collect_deep.py`, and the omarchy-ane-check
  line): new field `die` (integer, default 0) in the `omarchy_ane` block, next
  to `chip`. The smoke result gains `die`. Rows without `die` count as die 0.
- `tools/promotion_check.py`: key verdicts by `(soc, die)` instead of `soc`.
  `GOLDEN` stays per SoC (the fixture is identical per die). `soc(row)` keeps
  parsing `chip`; a `die_key(row) = (soc, die)` helper feeds `verdict()`,
  `json_verdict()`, `unattempted()` and the `ON`/`default_on()` sets (the
  overlays file gains its die-1 lines, so `default_on` needs the die split:
  key `t6002-die1` etc.). Verdict labels and targets unchanged per die.
- `ANE_LINE`: extend the DART alternative with `2285[0-2]0000` and the mailbox
  alternative with `2285408000`, so die-1 faults decide a row.
- Targets: a PROMOTE of die 0 flips the die-0 overlay key (`promote_chip.py`);
  a PROMOTE of die 1 flips the die-1 key and, for an intree passing row, adds
  the aurora die-1 include (`aurora_dt.py`). A chip is fully ON when both dies
  pass; the die-1 row cannot ride a die-0 pass.
- What a community Ultra row must prove before promotion, per die: ready
  `omarchy-ane-check`, the chip's driver bound to that die's device, exactly
  20 bit-exact smoke calls against the golden, no ANE/DART/mailbox fault line
  (die-1 scoped), `die` field present. Same rule as today, evaluated per die.

## 8. Milestones

Estimates are single-agent working days including receipt writing; they are
estimates, not commitments. Every milestone leaves a receipt and, when it
runs on silicon, a community row.

- **M0 — T6002 die 0 on Linux (opt-in proof).** Install Omarchy on the M1
  Ultra desktop (owner decision needed), add the `ane-t6002` key, boot, bind
  `ane.ko` (`apple,t6000-ane`), run the 20-call smoke. Evidence: dmesg probe
  fields, smoke golden `5ad7eccd…`, community row. Risk: SET-window word
  layout unverified on this silicon (the descriptor is QUALIFIED from T6001,
  the ps word order is not); mit: the ps ACTUAL probe logs before any write.
  Est: 1–2 days. Smallest real-silicon step.
- **M1 — die-1 plumbing, static.** `ane.ko` SET-from-node + die-keyed qual
  table; die-1 overlays + `overlays` lines + tests; `ane-run --dev`;
  `omarchy-ane-smoke` die field; promotion_check per-die keys + ANE_LINE
  extension; collector field spec. No hardware. Est: 2–3 days. Gate: full
  static suite green on the fixture DTBs.
- **M2 — T6002 die 1 as second device.** Apply `ane-t6002-die1`, boot, two
  binds, per-device smoke, per-die autosuspend checks. Evidence: two `loaded
  ane` lines, two accel nodes, two passing rows (die 0 + die 1), genpd
  per-die idle evidence. Risk: die-1 SET base unproven (RECOGNIZED gate,
  `allow_unqualified=1` first boot); die-1 pmgr states may need the die-1
  `AFR` parent proven. Est: 2–3 days.
- **M3 — T6022 die 0 on Linux.** Needs an M2 Ultra Linux host (none exists;
  no Ultra Linux rows at all). The known T6021 path applies (own-memory
  firmware, RTKit, mailbox) plus the T6022 die-0 constants; the missing ane0
  `segment-ranges` (iBoot-patched ADT) must come from a live capture first.
  Est: 3–5 days after a host exists.
- **M4 — T6022 die 1.** ane1 firmware pin, `pmu_pa` die-1, die-1 mailbox/IRQ
  checks, second firmware boot, per-device smoke. Est: 2–4 days after M3.
- **M5 — cross-die use.** Two workers (or a worker pair) on the Ultra hosts;
  collector rows with `die`; promotion of both dies; optional libane-level
  device choice by env for omarchy-mlx. Est: 2–3 days.

## 9. Missing facts

1. No Ultra runs Linux; no Ultra community rows exist. M0 needs an owner
   decision to install Omarchy on the M1 Ultra desktop (or another Ultra).
2. `t602x_ane1_fw_selene_rc4x` versus `t602x_ane0_...`: byte identity unknown
   (static IPSW check decides the firmware-pin shape).
3. T600x die-1 ps word layout (`set0/base/set1..4` at `0xc000..0xc02c`): the
   macOS-side capture cannot see the word order; the first die-1 probe resume
   logs the ACTUAL nibbles.
4. T6022 ane0 `segment-ranges` (iBoot-patched, T6021 values assumed): not
   captured; needed before M3. The live M1 Ultra capture shows the property
   exists per instance, so the T6022 shape should be reachable the same way.
5. Die-1 DART vm-base (T602x alias IOVA) and die-1 mailbox address/IRQ:
   INFERENCE (+0x20G, line 884) until a live T6022 capture.
6. `ane-subtype` semantics (0 vs 2).
7. Cross-die runtime-PM coupling (does die-0 idle power change while die 1
   runs?): unmeasured.
8. macOS ps entries for die-1 ANE states are invisible to ioreg (the live DT
   carries no ps entries at all); the Linux-side genpd names come from the
   aurora template and the 13.5 ADT gates, not from this capture.
