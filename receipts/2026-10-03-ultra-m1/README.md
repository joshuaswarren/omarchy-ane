# 2026-10-03: Ultra M1 — die-1 plumbing, static (milestone M1)

Implements docs/ultra-die1.md milestone M1 on `agent/ultra-m1` (from origin/main
4c41fa9): ane.ko SET-from-node + die-keyed qualification, ane_t6021 T6022
per-die data, the die-1 overlays + tests, `ane-run --dev`, the smoke `die`
field, per-die promotion keys, and the collector field spec. No hardware was
touched; every die-0 observable is unchanged.

## Changes

| Area | File | Change |
| --- | --- | --- |
| ane.ko | ane/src/ane_drv.c | The SET base comes from the node's `set` reg when the overlay names one, else the descriptor's die-0 constant. Qualification moved to a die-keyed table `ane_qual_table[]`: `(compatible, SET base) -> tier`. Rows: t8103 0x23b70c000 QUALIFIED; t6000 0x28e08c000 QUALIFIED (T6000/T6001/T6002 die 0); t6000 0x228e08c000 RECOGNIZED (T6002 die 1, binds only with `ane.allow_unqualified=1`); t6021 0x28e08c000 RECOGNIZED. A base outside the table refuses (ANE_UNSUPPORTED path, distinct message): a guessed SET base cannot bind silently. t6020 keeps its descriptor-level refusal. `tm_retention` stays a descriptor property (T6002 die 1 rides the T6000 descriptor, as die 0). |
| ane_t6021 | ane/t6021/ane_t6021_fwload.c, ane_t6021.h, ane_t6021_rtclient_main.c | `ane_t6022_soc_die1`: pmu_pa 0x228e084000 (SET base − 0x8000) and firmware pin `t602x_ane1_fw_selene_rc4x` as a data field (`fw_pin`) only — the loader still stages `soc->fw->name`; no die-0 behavior change. Selection `ane_t6021_soc_for(dev)` keys on the node's `set` reg (T6022 only; no window / other SoC / unknown base -> the compatible's own row). `ponytail` ceiling noted in-source: an unknown SET base rides die-0 data until M4 makes it a refusal. |
| overlays | packaging/dt/t6002-ane-die1.dts (new), t6022-ane-die1.dts (new), t600x-ane.dtsi, packaging/dt/overlays | t6002-ane-die1 (key `ane-t6002-die1`, opt-in, no skip-if-compatible): die-1 DARTs by path under /soc@2200000000, die-1 pmgr states (@268/@2c8 enabled where the tree ships them, set0..base..set1..4 @c000..c028 added — aurora keeps those in its DIE_NO == 0 pass), ane1 node `reg-names = "engine", "set"`, IRQs `<0 1 770 4>` / `<0 1 771 4>` (die in the AIC specifier), 3 DARTs, iommus sid 0. t6022-ane-die1: same shape over the T602x values (mailbox @285408000, IRQ 884/885 die 1, ane1 `apple,t6022-ane`, ane-alias IOVA 0x105_00000000 = INFERENCE §9.5) — **data-only** (root `omarchy,data-only`, `omarchy,opt-in = "ane-t6022-die1"`): build-dtbo compiles it, nothing installs or applies it until M4, because ane_t6021 has no qualification gate and an enabled die-1 node would bind and boot unproven die-1 firmware. It also defines the die-1 pmp node fully: aurora compiles DIE_NODE(ps_pmp) for both dies, linux-asahi 7.1.13 ships die-1 @2c8 not at all. t600x-ane.dtsi: the ane node gains the `set` reg (pmgr + 0xc000, 0x38 bytes). Overlays table: `t6002 t6002-ane-die1.dts opt-in`. |
| omarchy-ane-dt | packaging/omarchy-ane-dt | `status` prints every live ANE node (`path=/soc/ane@284000000,/soc@2200000000/ane@284000000` shape) when both dies apply. validate() rules unchanged per design §5.2 — they already resolve per-die references to enabled providers; the tests prove it (broken die-1 refs refuse through the existing paths). |
| tools | tools/ane-run.c, libane/ane.c, ane_m2.h | `--dev N` and env `ANE_DEVICE` (flag wins, default 0 = unchanged) select the accel node libane opens (`device_open()` walks /dev/accel/accel0..63 and returns the dev_id-th "ane" DRM device). `ane_m2_init_ports` gained the dev_id parameter (only caller updated). |
| smoke | packaging/omarchy-ane-smoke | Additive `die` field in both JSON results, derived from the first bound device's DT node reg[0] // 0x20_0000_0000; 0 when no device/reg (tests). |
| promotion | tools/promotion_check.py | Verdicts keyed by `(soc, die)`; `die` read from the collector block, absent/invalid = 0; `ON` stays die-0-only (per-SoC overlay rows), so a die-1 row never rides a die-0 pass and the text/JSON output on today's dataset is unchanged (proof below). ANE_LINE gains the die-1 DARTs `22858[0-2]0000` and mailbox `2285408000` — note: the design's §7 literal `2285[0-2]0000` matches none of the real die-1 DART addresses; corrected to `22858[0-2]0000` (0x22858[0-2]0000 = the +0x20_0000_0000 of the die-0 DART set). |
| docs | docs/collector-die-field.md (new), README.md | The collector field spec the task asked for; README smoke-field list and Promotion-rule surface (die in the smoke JSON). |
| tests | tools/test_ultra_die1.py (new), tools/test_ultra_die1_dt.py (new), tools/test_promotion_check.py, tools/test_ane_dt.py, tools/test_ane_m2.py | Boot-free die-keyed table unit/model test (parses the drivers, models `ane_qual_for`, cross-checks every overlay window against the table bases, asserts the tool wiring); aurora-DTB applications (below); the promotion suite re-keyed + per-die cases; packaged-dtbo lists. |

## Static gates (MEASURED, this session)

- `pytest -q tests tools`: pass (tests/ unchanged; tools collected).
- `make -C tools check`: pass (ane-selfcheck, test_libane_ioctl, test_ane_stats).
- Offline suites: test_ane_dt, test_ane_m2, test_promotion_check, test_promote_chip,
  test_promote_from_verdict, test_validate_ane_soc, test_t8112_kit,
  test_ultra_die1, test_ultra_die1_dt — all ok.
- asahi-tree overlays: `tools/asahi-dtbs /tmp/ultra-m1/asahi-dtbs` (29 DTBs,
  DTC 1.7.2-g53373d13) + `test_ane_overlays.py`: ok (10 overlays, 33
  applications) — the die-1 overlays apply to the linux-asahi t6002-j375d /
  t6022-j180d / t6022-j475d trees too.
- aurora-tree applications (`test_ultra_die1_dt.py`, josh/ane-driver-aurora
  efe6e359d456): t6002-j375d and t6022-j475d each apply die-0 + die-1:
  both ANE nodes present and enabled, 3 DARTs each (distinct sets per die),
  no node collisions, second application idempotent, phandles preserved.
- `promotion_check --remote` before/after: byte-identical output on the live
  dataset (175 rows, exit 0 both runs; diff empty).
- W=1 arm64 module builds, M2 3-1 tree (macstudio ALARM chroot recipe,
  kernel.release 7.1.13-3-1-ARCH): baseline and M1 byte-comparable warning
  sets, no new diagnostics; modules build (vermagic 7.1.13-3-1-ARCH SMP
  preempt mod_unload aarch64). SHA-256: ane base 2a8753a387c8…, ane M1
  8eebf719a3e7…; ane_t6021 base ff801f7b0ef4…, ane_t6021 M1 3e7b73725736…

## Die-0 unchanged evidence

- Qualification model (`tools/test_ultra_die1.py`): every die-0
  (compatible, base) pair keeps its tier — t8103/t6000 QUALIFIED,
  t6021 RECOGNIZED, t6020 UNSUPPORTED — including trees without a `set`
  reg (descriptor fallback), and a guessed base refuses on every compatible.
- The T600x die-0 overlay's new `set` window carries exactly the proven
  die-0 base 0x28e08c000, so a probe through the node lands on the same
  base the descriptor used to provide.
- `promotion_check --remote` before/after identical (the verdict dataset is
  all legacy rows, i.e. die 0).
- asahi overlay suite: 33 applications, same result count as before the
  change minus the new die-1 row's applications (10th overlay), all green.

## Not verified / refused by the tree

- **W=1 builds on the aurora tree: refused by the tree, not by this change.**
  origin/main fails identically: `ane_drv.c:993: error: 'accel_open'
  undeclared` (the aurora 7.1.12 drm_accel does not export it the way the
  7.1.13 omarchy kernel does; ane_t6021 also hits `ANE_M2_MAX_BINDS` /
  `ANE_ABI_M2_MAJOR` config gaps under the 7.1.13 config olddefconfig'd onto
  7.1.12). Warning sets base-vs-M1 identical (ane: 1 pre-existing
  -Wformat-truncation in ane_tm.c; t6021: 5 pre-existing). Fixing the
  aurora-tree compile is an in-tree-driver concern (the aurora tree carries
  drivers/accel/ane), out of M1 scope; the DKMS modules' target kernel is
  the 7.1.13 family, where the builds pass above.
- **Hardware gates: NOT RUN (no hardware in this container; the PR must not
  merge before they pass).** The protocol the lanes must run is below.

## Hardware gate protocol (A/B, per T8103 / T6001 / T6021)

Build the module from this PR on the lane host (same kernel tree as the
installed one), then, per host, run the new-module side and compare against
the retained prior module (ane-96d5a88.ko / the currently loaded one):

1. **Smoke:** `omarchy-ane-smoke` — 20/20 calls bit-exact against the chip's
   golden (H13 5ad7eccd… on T8103/T6001, H14 94041b7c… on T6021), exit 0.
2. **Encoder:** the certified Parakeet encoder E2E pin —
   `omarchy-ane-run --anec <encoder program> --in … --check` per the lane's
   standing recipe (receipts/2026-09-30-t6021-parakeet-encoder and the
   jwm1-encoder-gap lane doc); hashes must equal the pinned values bit for
   bit, wall clock within the lane's noise band.
3. **Autosuspend:** leave the device idle past `autosuspend_ms`, confirm the
   runtime status reads `suspended`, then rerun the smoke cold (the wake
   path raises the islands again); repeat 3 idle/sleep cycles.
4. **map_batch regression:** run the 16-rep alternating-pattern buffer-reuse
   battery with `map_batch=1` (default) and `map_batch=0`; byte-identical
   outputs across reps and across both settings (the certified battery of
   the map_mode comment in ane_drv.c), plus one `map_mode=0` run per host
   (rollback path) with the same byte-identity.
5. **Die-0 probe fields:** journal shows one `loaded ane` line per device,
   `ANERD ps probe act=0xffffff` unchanged, no DART fault lines.
   T6021 additionally: RTKit HELLO reached (existing lane recipe).

A/B pass = every check byte-identical or bit-exact equal to the retained
module's results on the same host. T6002/T6022 die-1 hardware checks are M2/M4
and are not part of this gate.

## Receipts

- Session artifacts: /tmp/ultra-m1 (asahi DTBs + test outputs,
  promotion-before/after.txt, w1-*.log, aurora-build), to be copied into the
  lab store under artifacts/UltraM1/ with SHA256SUMS.
- Notebook: entries/UltraM1/2026-10-03-omp-studio-local-ultra-m1-static-plumbing.md
  (private store; pre-registered before the first edit).
- PR: agent/ultra-m1 -> main (REST-opened, not merged; hardware gates open).
