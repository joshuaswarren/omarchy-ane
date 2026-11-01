# J700/H17: the shipped Linux ANE driver and what it proves about power

Read-only source and module inspection. No device power, MMIO, module load,
or reboot. The running kernel `7.2.11-6-1-ARCH` ships an in-tree ANE driver
pair, and its source is the authoritative, m1n1-referencing Linux power-on
reference — even though it does not target T8140.

## Evidence identities

- Source checkout: `/home/admin/linux-aurora` at commit
  `b6f070c91193…` (our recorded Linux baseline), `drivers/accel/ane/`.
  A second, older checkout exists at `/home/admin/aurora-linux/drivers/accel/ane/`
  (smaller `ane_t6021_boot.c`, fewer comments).
- Shipped modules (running kernel):
  `/lib/modules/7.2.11-6-1-ARCH/kernel/drivers/accel/ane/ane.ko` and
  `ane_t6021.ko`.
- m1n1 source is **not** checked out on this machine; only the compiled
  `boot.bin` is present at `/home/admin/dev4/boot/efi/m1n1/`. The driver
  header cites m1n1 as the source of the ASC register shapes ("m1n1
  ASCRegs +4 shape", "m1n1 R_CPU_STATUS shape", "m1n1 proxyclient/m1n1/
  hw/asc.py ASCRegs").

## Compatible list (no T8140)

- `ane.ko` matches `apple,t8103-ane`, `apple,t6000-ane`
  (`ane_drv.c` `ane_of_match`, ~line 1133).
- `ane_t6021.ko` matches `apple,t6020-ane`, `apple,t6021-ane`,
  `apple,t6022-ane`, `apple,t8112-ane` (`ane_t6021_rtclient_main.c`
  `ane_rtclient_of_match`, lines 2529–2533; confirmed in `modinfo` aliases).
- No `t8140` string anywhere in `drivers/accel/ane/`.

## Finding 1 — the ANE is powered via genpd, never raw ps-word writes

`ane_t6021_boot.c` (header comment, lines 20–27):

> CPU power: … the CPU island is powered with write32(PMGR+0x2e0,
> 0x0000000f), validate 0xff … On Linux genpd raises the island before
> any of this code runs, and probe checks its ACTUAL state before the
> first engine read; the write is therefore not replicated (**a direct
> ps-word write from kernel context has hung the machine**).

The probe sequence (`ane_t6021_rtclient_main.c`, ~lines 2118–2160):

1. `ane_rtclient_attach_genpd()` — attach the DT `power-domains`.
2. `pm_runtime_resume_and_get(dev)` — raise the island through runtime PM.
3. `devm_of_iomap(dev, …, 1)` — map the PMGR window, then
   `readl(pmgr + ps_cpu_off)`.
4. Refuse unless `FIELD_GET(ANE_PS_ACTUAL, ps_cpu) == ANE_PS_ON`.
5. (T8112 only) map the "set" window, `readl(set + pwgate_off)`, and
   refuse if PWGATE bits 29:28 are set, **before any engine read**.
6. Only then `readl(engine + CPU_STATUS)` / `readq(engine + RVBAR)`.

## Finding 2 — eight power domains, two of them mandatory

`ane_t6021.h` (lines 40–43):

> power-domains = eight, raised in order (sys_mpm→td→base→set1..4,
> ane_cpu last): ane_sys_mpm@4000, ane_td@4008, ane_base@4010,
> ane_set1..4@4018-4030, ane_cpu@2e0. **ane_td and ane_base are
> required: with only the other six raised, block access resets the
> machine.**

Per-SoC ps-word/PWGATE offsets (`ane_t6021_fwload.c`, lines 194–218):

| SoC | `ps_cpu_off` | `pwgate_off` |
| --- | --- | --- |
| t6020 / t6021 / t6022 | `0x2e0` | none |
| t8112 | `0xc008` | `0x8b8` |

ps-word field layout (`ane_t6021.h`, lines 70–77):

| Field | Bits |
| --- | --- |
| `ANE_PS_ON` | value `0xf` |
| `ANE_PS_TARGET` | `GENMASK(3, 0)` |
| `ANE_PS_ACTUAL` | `GENMASK(7, 4)` |
| `ANE_PS_WAS_GATED` | `GENMASK(9, 8)` |
| `ANE_PS_BUSY` | `BIT(11)` |
| `ANE_PS_AUTO_ENABLE` | `BIT(28)` |

`ACTUAL == 0xf` is the "on" condition the probe gates on.

## Finding 3 — H17 CPU-block offset confirmed independently

`ane_t6021.h` (lines 78–82):

> CPU block = ANE+0x1400000 for h14g: CPU_CONTROL is 0x1400044, without
> the 0x200000 offset of the h16g/**h17**/h18g parts (whose block is the
> 0x1600044 one).

This is the same `0x1600044` CPU-control (and `0x1600048` status) that our
compiled Apple-kernelcache trace attributed to J700 (ane-type 0x200, H17).
The H14G register map is otherwise specific to that part (CPU_STATUS
`0x1400048`, RVBAR `0x1050000`, SCRATCH `0x1840048…0x1840064`), so only the
CPU-block offset carries over to H17, not the rest.

## The read whitelist and the never-touch windows

`ane_t6021_boot.c` read whitelist (lines 72–90), each read documented with
its pre-boot observed value:

- RVBAR `eng+0x01050000` — read64, returns `0x1`.
- CPU_STATUS `eng+0x1400048` — reads `0x2a` before the CPU starts.
- ASC mailbox controls `eng+0x1408110`/`0x1408114` — read `0x00020001`;
  polling neither changes them nor aborts.
- SCRATCH `eng+0x1840048…0x1840064` — all zero before attach.
- TICK `eng+0x1160008` — readable once the CPU is released.

Explicitly never touched on this part:

- the `+0x1608xxx` mailbox-class reads (the h16g analog) — a different
  address, described as fatal.
- the `0x1854000…0x1c04000` fabric-fatal window.

## What this means for J700 (T8140)

1. The safe power-on method is established (genpd + runtime PM), and the
   unsafe one (direct PMGR ps-word write) is documented as a hang.
2. The J700 DTS `ane` node declares a **single** power domain
   (`ps_ane_sys@0x290`, `t8140-pmgr.dtsi` line 828) and `reg-names`
   `"engine"` only — far short of the eight domains and the pmgr/set
   windows the sequencer requires. The safe path cannot be reused as-is.
3. The H17 CPU-block offset (`0x1600044`) is now confirmed by an
   independent Linux source, but the H17 ps-word offset and the H17 power
   domain set remain unknown and unqualified.

## Limits

Everything here is about T602x/T8112 (H14G) source and the shipped module.
None of it was exercised on a device. It does not qualify any T8140
register read, power transition, or firmware path — it only pins the method
and closes one offset gap.
