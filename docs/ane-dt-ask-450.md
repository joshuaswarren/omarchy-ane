# ANE device-tree ask for `aurora-silicon/linux` — 2026-11-28 refresh

Refresh of the `omarchy-ane` issue #450 ask (itself a 2026-11-20 refresh against
`integration/aurora` @ `5b596e6e377afb8e18fbbeb8a8b80dac86982321`) against
`aurora-silicon/linux` **`aurora-edge`** @ `f0b72363b06d88b70e370ddc589ec4b34225d23c`
and `joshuaswarren/omarchy-ane` `main` @ `9ab1d972dac8e83de651d92b9aaf06df4e88f05d`.

**Branch note:** `integration/aurora` no longer exists on the remote. `aurora-edge`
carries the lineage (`5b596e6e` is an ancestor of `f0b72363b`); 575 commits have
touched ANE-relevant paths since the audited base.

## Deltas since the 2026-11-20 audit

| Item | 11-20 claim | 11-28 verified state on `aurora-edge` |
| --- | --- | --- |
| M3 (`t8122`) pmgr | part of open ask | **Landed** — `t8122-pmgr.dtsi` now carries all five `ps_ane` nodes (`ps_ane_sys` `@438`, `ps_ane_mpm/cpu/td/base` `@c000..c018`) |
| M3 schema | ask #1 pending | **Landed for t8122** — `apple,ane.yaml` has `apple,t8122-ane` entries (`:41-43, :377, :423`); `t603x` schema still absent |
| Driver match | `ane_t6021` lists 9 SoCs | Now 10 — `apple,t8122-ane` added (`ane_t6021_rtclient_main.c:4547`) |
| `t6050` board enables | 2 of 11 (`j714s`, `j716c`) | **4 of 11** — `k114c` and `k114s` added via shared `t6050-k114.dtsi:14-23` (which includes `t6050-j714s-ane.dtsi`); still missing on `j714c`, `j716s`, `j775c`, `j775d`, `j873s`, `k116c`, `k116s` |
| `t603x` pmgr | `t6030` 1 of 5, `t6031` all 5 | Holds — `t6030-pmgr.dtsi:833` has `ps_ane_sys` only; `t6031-pmgr.dtsi` has all 5 (now `DIE_NODE()` macro form at `:835-940`) |
| `t603x`/`t8122` arm-io nodes | all missing | Holds — zero `ane_dart`/`ane_mbox`/`neural-processor` nodes in `t8122.dtsi`, `t6030.dtsi`, `t6031.dtsi`, `t6032.dtsi`, `t6034.dtsi` |
| `t602x` `ane_dart0` power domain | wrong (`ps_pmp`) | Holds — still `power-domains = <&ps_pmp>` at `t602x-die0.dtsi:26` |
| `t6002`/`t6022` die-0 ANE | disabled | Holds — `t6002.dtsi` has zero `ane` refs; `t602x-die0.dtsi:66` `status = "disabled"`, `t6022.dtsi:329` only overrides compatible |
| `t8112` enabled unverified | enabled | Holds — `t8112.dtsi:1958-1973` `ane` node carries no `status` (enabled) |
| `apple_dart_of_match` | 5 entries | Now 9 — added `apple,t8152-dart-gen3`, `apple,t8152-avd-dart`, `apple,t8152-acio-dart`, `apple,t8152-pciec-dart` (`apple-dart.c:8028-8038`). Fallback note unchanged: `t6020`/`t8112`/`t8122`/`t603x`/`t6050` ANE DARTs still bind via `apple,t8110-dart` |
| KUnit coverage | 17 files / 18 suites / 267 cases | **19 files / 21 suites / 336 cases** in `drivers/accel/ane/*_test.c` |
| `t6050` preload placeholders | disabled in `t6050-j714s-ane.dtsi` | Holds — `ane_fw_text`/`ane_fw_data` still `status = "disabled"` pending boot-loader hand-off |
| `t6050` Ultra die-1 | not described | Holds — `t6050-ultra.dtsi` has no ANE/DART/PMGR nodes |
| `send-empty` IRQ, `of_platform_device`/`genpd`/`pmgr` safety | verified on `5b596e6e` | Claims 1–4 of issue §4 re-checked on `f0b72363b`: `of_device_is_available` gate, `genpd_power_off_unused`, boot-off islands, and `ane_fw_platform_preflight` `-ENODEV` all still hold |

## Open upstream PRs (state on 2026-11-28)

| PR | State | Covers |
| --- | --- | --- |
| `aurora-silicon/linux#2331` | OPEN | M3 `t8122`/`t603x` disabled ANE/DART/mailbox nodes |
| `aurora-silicon/linux#1694` | OPEN | `t602x` `ane_dart0` `ps_pmp` → `ps_ane_sys` fix |
| `aurora-silicon/linux#2293` | OPEN | Share `t6050` ANE across every board |
| `aurora-silicon/linux#2564` | **MERGED** 2026-11-26 | `t6050-k114c` ANE enable (landed as shared `t6050-k114.dtsi`, also enabling `k114s`) |
| `aurora-silicon/linux#1607` | CLOSED | Superseded by #2564 |
| `aurora-silicon/linux#1115` | CLOSED | `t6002`/`t6022` die-0 enable — closed unmerged; needs an M1 or M2 Ultra to verify both dies |

## Concrete ask, revised

1. **Land PR #2331 for the remaining M3 gaps:** `t8122` pmgr and schema are done;
   still needed are `t6030-pmgr.dtsi`'s `ps_ane_mpm/cpu/td/base` (`t6031` already has
   all five), the disabled `ane_dart0..2`/`ane_mbox`/`ane` arm-io nodes for
   `t8122`/`t6030`/`t6031`/`t6032`/`t6034`, and `t603x` entries in `apple,ane.yaml`.
   Keep every M3 node `status = "disabled"` until M3 firmware start is proven.
2. **Land PR #1694:** `t602x-die0.dtsi:26` `power-domains = <&ps_pmp>` → `<&ps_ane_sys>`.
3. **Land PR #2293 for the last 7 `t6050` boards:** `j714c`, `j716s`, `j775c`,
   `j775d`, `j873s`, `k116c`, `k116s` still lack the ANE include and fabric votes.
4. **`t6002`/`t6022` die-0 enable still gated on Ultra hardware** (#1115 closed);
   #1694 is a prerequisite for `t6022`. Die-1 follow-ups (`m2_ane1` IOVA alias,
   `t6050-ultra` die-1 nodes) unchanged.
5. **`t8112` default unchanged:** still enabled at `t8112.dtsi:1958` with no silicon
   verification past its closed power gate — driver-side default refusal carries
   the load.

## What we give back (updated)

- Out-of-tree match strings (`omarchy-ane` `main` @ `9ab1d97`): `ane.ko`
  (`ane/src/ane_drv.c:1251-1252`: `t8103`, `t6000`), `ane_t6021.ko`
  (`ane/t6021/ane_t6021_rtclient_main.c:3277-3280`: `t6020`, `t6021`, `t6022`,
  `t8112`), `ane_h15` (`ane/h15/ane_h15_main.c:3739-3742`: `t8122`, `t6030`,
  `t6031`, `t6034`), `ane_h16` (`ane/h16/ane_h16_main.c:2232-2235`: `t8132`,
  `t6040`, `t6041`, `t8140`), `ane_h18` (`ane/h18/ane_h18_main.c:4868-4869`:
  `t8152`, `t8132`).
- `tools/test_ane_overlays.py` + `.github/workflows/aurora-dtbs.yml` keep
  `data/ane-soc/*.json` schema-validated against Aurora's tree.
