# J700 (T8140 / H17) Linux ANE support plan

Status: **source-pinned plan; nothing device-qualified.** All register
offsets, window sizes, and power facts below are compiled-source or ADT
observations, not device behavior. Hardware gates stay closed.

Progress: the boot register map is pinned (RVBAR `0x1050000`, CPU control
`0x1600044`, status `0x1600048`, SCRATCH `0x1880020`, DONE `0x08042006`),
the T8140 firmware is verified against its fetch pin, and a T8140 soc row
now exists in `ane/h17/ane_h17_soc.{h,c}` for this repo's own driver. The
exclave analysis is in
`receipts/2026-11-01-j700-h17-exclave-vs-sep/`.

## Context

Two Linux ANE drivers exist. This repo's `ane/` tree builds its own
`ane.ko` plus the opt-in `ane/h16/` (T8132/T6040/T6041) and
`ane/t6021/` family, with a per-SoC `struct ane_h16_soc` register table.
The running aurora kernel also ships `drivers/accel/ane/` (`ane.ko` for
t8103/t6000, `ane_t6021.ko` for t6020/t6021/t6022/t8112). Neither
matches `apple,t8140-ane`. The T8140 H17 target differs from the shipped
H14G/T8112 support in three pinned ways:

1. H17 CPU block sits at engine+`0x1600044` (the h16g/h17/h18g
   `+0x200000` offset), not H14G's `0x1400044`.
2. The H17 ANE-CPU power-state word is at PMGR+`0xc000` (the Apple
   driver's "PS" control), not T8112's `0xc008`.
3. The H17 PWGATE word is at set-window+`0x268c`/`0x26b0`, not T8112's
   `0x8b8`.

## Window and control binding (from ADT A1 + Apple trace)

| Apple control | direct index | ADT resource | Linux node | H17 offsets |
| --- | --- | --- | --- | --- |
| ANE | 0 | `0x400000000 + 0x2000000` | engine | scratch `0x1880020..3c`, CPU ctrl `0x1600044`, status `0x1600048` |
| PS | 1 | `0x300700000 + 0x18000` | pmgr | CPU ps-word `0xc000`, `0xc008` |
| PWGATE | 2 | `0x300724000 + 0x4000` | set | `0x268c`, `0x26b0` |
| PTD | 3 | `0x3003c0000 + 0x30000` | — | not created on this config (guard byte 14036 never stored) |
| ANEHAL1/2/3 | 4/5/0 | — | — | HAL tunables; unqualified |

The Apple J700 CPU power-on (`0xc000 := 0xf` → validate `0xff` →
`0xc000 := 0x1000000f` → validate `0x100000ff`) is the standard
`apple-pmgr-pwrstate` protocol (TARGET 3:0, ACTUAL 7:4, AUTO_ENABLE bit
28), so the H17 CPU island is a real ps-word even though the ADT lists
`ANE_CPU` as `no_ps`.

## Offline tasks

### 1. DTS: express the CPU ps-word as a genpd node

Extend `ane` in `arch/arm64/boot/dts/apple/t8140.dtsi`:

```dts
ane: ane@400000000 {
	compatible = "apple,t8140-ane";
	reg = <0x4 0x0 0x0 0x2000000>,
	      <0x3 0x00700000 0x0 0x18000>,
	      <0x3 0x00724000 0x0 0x4000>;
	reg-names = "engine", "pmgr", "set";
	interrupt-parent = <&aic>;
	interrupts = <AIC_IRQ 655 IRQ_TYPE_LEVEL_HIGH>,
		     <AIC_IRQ 668 IRQ_TYPE_LEVEL_HIGH>;
	iommus = <&ane_dart0 0>, <&ane_dart1 0>, <&ane_dart2 0>;
	power-domains = <&ps_ane_sys>, <&ps_ane_sys_cpu>;
	status = "disabled";
};
```

Add to `arch/arm64/boot/dts/apple/t8140-pmgr.dtsi`:

```dts
ps_ane_sys_cpu: power-controller@c000 {
	compatible = "apple,t8140-pmgr-pwrstate", "apple,t8103-pmgr-pwrstate";
	reg = <0xc000 4>;
	#power-domain-cells = <0>;
	#reset-cells = <0>;
	label = "ane_sys_cpu";
	power-domains = <&ps_ane_sys>;
};
```

This converts the Apple driver's direct `0xc000` write into a genpd raise
— the only path the shipped Linux driver trusts ("a direct ps-word write
from kernel context has hung the machine").

Uncertain: whether H17 has H14-style `td`/`base`/`set1..4` islands. The ADT
lists only `ANE_SYS` (ps `0x290`) plus `ANE_CPU`, `ANE-SYS-V`,
`ANE-SYS-V-PMP` (all `no_ps`), suggesting a simpler structure than T8112's
eight islands. Do not add chains not evidenced in the ADT or Apple trace.

### 2. Driver: add a T8140 row to this repo's H16/H17 table

This repo's own driver family already has the right shape: `ane/h16/`
defines `struct ane_h16_soc` with per-SoC `cpu_control`, `cpu_status`,
`rvbar`, `scratch0`, `irq_status`, `irq_ack`, `mbox`, `wrapper_pa`,
`cpu_pa` and `ps_off[5]`, and its header names `ane/h17/ane_h17_soc.h`
as the H17 extension point. The T8132 row already uses
`0x1600044`/`0x1600048`/`0x1050000`/`0x1880020`/`0x1608000` — the same
layout H17 needs.

Authored this round: `ane/h17/ane_h17_soc.{h,c}` with `ane_t8140_soc`
and the THEIA_D9X firmware descriptor (compiles and passes its
type/value check). Wiring it in is the remaining step:

- Add `{ .compatible = "apple,t8140-ane", .data = &ane_t8140_soc }` to
  `ane_h16_of_match` in `ane/h16/ane_h16_main.c`, and build
  `ane_h17_soc.o` into the module. **Not done on purpose**: the H16
  driver would then claim a boot path that has never run. Gate this on
  hardware qualification.
- The mailboxes are engine-relative (`mbox = 0x1608000`), so no DT
  mailbox node is needed. The DTS patch supplies the `pmgr`/`set`
  windows and the `ps_ane_sys_cpu` pwrstate.

An alternative target is the in-tree `drivers/accel/ane/ane_t6021.ko`
(uses the `ane_t602x_soc` descriptor, ps `c000`/pwgate `268c`); its
register map is hardcoded H14G, so that path needs a bigger refactor.

### 3. Pin the H17 register map (kernelcache, offline)

The legacy direct-boot block in `ANEHWDevice::ANE_Init()`
(`0xfffffe0008f3e138..0xfffffe0008f3e198`) resolves the boot handshake:

| Register | H14G (shipped) | H17 (pinned, this plan) | Evidence |
| --- | --- | --- | --- |
| RVBAR | `0x1050000` | **`0x1050000` (same)** | `write64(0x1050000, entry)` at `0x8f3e138` |
| CPU_CONTROL | `0x1400044` | **`0x1600044`** | field 352; write `0` then `0x10` (RUN=bit4) at `0x8f3e14c`/`0x8f3e15c` |
| CPU_STATUS | `0x1400048` | **`0x1600048`** | status-diagnostic |
| SCRATCH0..7 | `0x1840048..64` | **`0x1880020..3c`** | fields 252..280, `InitANEScratchRegisters` |
| SCRATCH7 DONE | `0x08042006` | **`0x08042006`** | poll at `0x8f3e190`, cmp `0x08042006` |

RVBAR lives in the ASC CPU-base block and does **not** shift between H14G
and H17; only the CPU control/status block shifts by `+0x200000`
(`0x1400000` → `0x1600000`). The boot sequence is: write RVBAR (64-bit
entry), clear CPU_CONTROL, set CPU_CONTROL RUN (`0x10`), then poll
SCRATCH7 for `0x08042006`.

The ASC mailbox is engine-relative: the T8132/H16 row uses
`mbox = 0x1608000`, so with the shared `+0x200000` shift the H17 control
words are `0x1608110`/`0x1608114` (mbox + `0x110`/`0x114`). That value is
a T8132-template candidate for H17, not yet traced.

Still unpinned: the CPU_STATUS pre-boot idle value (H14G `0x2a`) and the
roles of two extra configured offsets (field 292 = `0x1880064`, field 312
= `0x1c201060`).

### 2b. Driver: what the change really entails

The register map is currently **hardcoded H14G** in `ane_t6021.h`
(`ANE_ASC_CPU_CONTROL 0x1400044`, `ANE_ASC_CPU_STATUS 0x1400048`,
`ANE_ASC_RVBAR 0x1050000`, `ANE_MBI_SCRATCH0 0x1840048`, the
`0x1408110/0x1408114` mailbox, `ANE_ASC_RTB_STATUS 0x1840088`, and the
read-whitelist constants in `ane_t6021_boot.c`). Supporting H17 needs
those to become per-SoC fields, then a descriptor:

```c
/* ane_t6021_fwload.c — probe/register-read descriptor only.
 * No firmware: H17 ownership (exclave) is a separate, unresolved path.
 */
const struct ane_t602x_soc ane_t8140_soc = {
	.soc = 0x8140, .soc_revision = 0x00, .revision_fuse = true,
	.fw = NULL,			/* exclave path: no RTKit image */
	.tunables = NULL,		/* unqualified */
	.ps_cpu_off = 0xc000,		/* H17 ANE-CPU ps-word (Apple "PS") */
	.pwgate_off = 0x268c,		/* H17 PWGATE word (Apple "PWGATE") */
	/* pmu_pa/ps_off unknown until the H17 power-domain set is pinned */
};
```

Per-SoC register offsets to add (H17 column pinned this round):

| Field | H14G | H17 |
| --- | --- | --- |
| `cpu_control_off` | `0x1400044` | `0x1600044` |
| `cpu_status_off` | `0x1400048` | `0x1600048` |
| `rvbar_off` | `0x1050000` | `0x1050000` |
| `scratch0_off` | `0x1840048` | `0x1880020` |
| mailbox ctrl | `0x1408110/0x1408114` | `0x1608110/0x1608114` (candidate) |

Add `{ .compatible = "apple,t8140-ane", .data = &ane_t8140_soc }` to
`ane_rtclient_of_match`. This gets probe + a gated register read; firmware
execution stays out of scope (exclave transport).

### 4. Clocks

ADT `clock-ids [327, 326, 325]`; Apple votes 327 always and 326 with the
`pmp-virt-notify` flag. Linux `t8140-pmgr.dtsi` has no clock controllers,
so the Linux gate identity for these IDs is unknown. Options: add clock
nodes for 327/326/325, or prove `ps_ane_sys` alone gates them. Least
pinned item.

### 5. Firmware (not an offline task)

H17 uses the exclave (`com.apple.service.ANEExclave`, RPC
`_aneexclave_aneexclavexnu_preparepoweroff`), which has no Linux
equivalent. Power and register reads can be qualified without it;
execution cannot. Kept strictly separate.

## Hardware ladder (after review of 1–4)

Read-only first, each rung gated on the previous, never a direct ps-word
write, reboot-recovery planned per the M1 rules:
genpd raise → read `ACTUAL` via pmgr → read CPU_STATUS/scratch → then,
only if safe, bounded power experiments.
