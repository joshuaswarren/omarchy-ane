# J700/H17: Linux support plan, pinned register map, and DTS patch

Offline work only. No device power, MMIO, module load, or reboot.

## Deliverables (this commit)

- `docs/j700-h17-linux-support-plan.md` — the fix plan (DTS → driver →
  register map → clocks → firmware, with the hardware ladder).
- `docs/t8140-ane-power.patch` — an applyable DTS patch for linux-aurora
  (`git apply --check` passes against `b6f070c91193…`).
- This receipt.

## New register-map findings (H17, pinned from the retained kernelcache)

The legacy direct-boot block in `ANEHWDevice::ANE_Init()` at
`0xfffffe0008f3e138..0xfffffe0008f3e198` resolves the boot handshake:

| Register | H14G (shipped) | H17 (pinned) | Evidence |
| --- | --- | --- | --- |
| RVBAR | `0x1050000` | **`0x1050000` (same)** | `write64(0x1050000, entry)` at `0x8f3e138` |
| CPU_CONTROL | `0x1400044` | **`0x1600044`** | field 352; write `0` then `0x10` (RUN=bit 4) at `0x8f3e14c`/`0x8f3e15c` |
| CPU_STATUS | `0x1400048` | **`0x1600048`** | status-diagnostic source |
| SCRATCH0..7 | `0x1840048..64` | **`0x1880020..3c`** | fields 252..280, `InitANEScratchRegisters` |
| SCRATCH7 DONE | `0x08042006` | **`0x08042006`** | poll at `0x8f3e190`, cmp `0x08042006` |

RVBAR lives in the ASC CPU-base block and does **not** shift between H14G
and H17; only the CPU control/status block shifts by `+0x200000`
(`0x1400000` → `0x1600000`). The boot order is: write RVBAR (64-bit
entry), clear CPU_CONTROL, set CPU_CONTROL RUN (`0x10`), poll SCRATCH7 for
`0x08042006`.

Two extra configured offsets remain unlabeled: field 292 = `0x1880064`
(read in a firmware-command path) and field 312 = `0x1c201060` (read
widely). The H17 ASC mailbox control offsets and the CPU_STATUS pre-boot
idle value (H14G `0x2a`) are still unpinned.

## What the fix requires (source-level, no hardware)

1. **DTS** — add the `pmgr` and `set` windows and a `ps_ane_sys_cpu`
   pwrstate node at `0xc000`, so genpd raises the H17 CPU island instead
   of a direct ps-word write (which the shipped driver documents as a
   hang). See `docs/t8140-ane-power.patch`.
2. **Driver** — the register map in `ane_t6021.h` is hardcoded H14G
   (`0x1400044`, `0x1840048`, mailbox `0x1408110/0x1408114`, …); it must
   become per-SoC fields before a `ane_t8140_soc` descriptor
   (`ps_cpu_off = 0xc000`, `pwgate_off = 0x268c`) is meaningful. See the
   plan's section 2b for the concrete descriptor.
3. **Firmware** — out of scope; H17 uses the exclave
   (`com.apple.service.ANEExclave`), which has no Linux transport.

## Power-domain evidence (from ADT A1, `data/ane-soc/t8140.json`)

`ANE_SYS` id 60 ps-word `0x300700290`; `ANE_CPU` id 129, `ANE-SYS-V` id
291, `ANE-SYS-V-PMP` id 338 all `no_ps`. The Apple driver still writes the
CPU island directly at PMGR+`0xc000` — the register exists but is not a
named ADT pwrstate. Whether `no_ps` means genpd cannot own the `0xc000`
word (requiring an alternative owner) is unresolved and flagged in the
plan.

## Limits

Everything is compiled-source, ADT, or shipped-source observation. The DTS
patch is a proposal that applies cleanly but has not been booted or
DT-validated on a device. None of the H17 register offsets above were read
from hardware; the hardware ladder remains closed.
