# J700 H17: the qualified register map, and the re-derived stage-4 module

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`, branch
`neo/h17-bringup`. Offline work only: no ANE MMIO, no module load, no
reboot. This receipt lands release criteria 1–3 of the
[hang receipt](../../receipts/2026-10-30-j700-h17-hang/README.md);
the sources are the [hal-map
receipt](../../receipts/2026-10-30-j700-h17-hal-map/README.md) and the
[stage-4 receipt](../../receipts/2026-10-30-j700-h17-asc-map/README.md).

## The qualified map

Every row the re-derived module may touch, with its source:

| row | offset(s) | source |
| --- | --- | --- |
| pmgr ANE_SYS | `0x290` | macOS power-up path (stage-4 receipt) |
| pmgr child state | `0xc008` | macOS power-up path (stage-4 receipt) |
| SCRATCH page | `0x1880020…0x1880044` (9 words) | T8140 HAL exports the page (`0x401880000`); the `+0x20` vs `+0x24` word alignment stays param-selected (`scratch0_off`) |
| doorbell | `0x18a4000` ring, `0x18a4008` poll/ack, `0x18a4010`/`0x18a4018` state (64-bit) | T8140 HAL composes each |
| IPI pending | `0x18ac000` | T8140 HAL composes and exports it |
| mailbox-setup writes | `0x1960000 <- 1`, `0x1974200 <- 0` then `<- 1`, `0x195c284 <- 1`, `0x195c288 <- 1` | T8140 HAL composes each |
| wake word value | `0x08042006` in SCRATCH7 | inherited value from the H14/H16 modules (ane/t6021/ane_t6021.h, ane/h16/README-bringup.md); a checked value, not an access |
| CPU wrapper trio | `0x1600044`, `0x1600048`, `0x1050000` | **no T8140 source** (h16g row + M6 parity) — behind `read_wrapper=1`, default off |
| mailbox peek | `mbox_off + 0x110/0x114/0x830/0x838`, `mbox_off` = `0x1608000` or `0x196c000` | **no T8140 source** — behind `mbox_peek=1`, default off |

## What the module does now

`ane/h17/ane_h17_wake.c` is re-derived row by row from that map:

- Default access set = the T8140-sourced rows only. The wrapper trio
  and mailbox peek are param-gated and default off.
- The h16g/M6 RUN write to CPU_CONTROL is retired; with
  `confirm_wake=1` the module runs the T8140 HAL's own control path:
  the mailbox-setup writes, then ring the doorbell with the complement
  of `(STATE0 & 3)` once that value is clear in `STATE1` (refusing
  with `doorbell-conflict`/`doorbell-unclear` otherwise), a bounded
  poll of the ack, the ack write, and the bounded SCRATCH7 wake-word
  poll.
- The quarantine gate stays `0`: module init still refuses
  (`reason=unqualified-register-map`), `run-stage4.sh` stays
  diagnostics-only (exit 2, no lifecycle). The gate lifts only when
  criterion 4 (hang containment) has a design.

## Verification (off device)

`make -C ane/h17 sim-all` — the userspace sim now builds the module
twice. The default build keeps the quarantine assertions (every mode
refuses with zero registrations, ioremaps, pmgr reads, regmap writes,
engine accesses and genpd votes, including `legacy-overrides`). The
qualified build (`-DH17_REGISTER_MAP_QUALIFIED=1`, sim only) runs the
probe against the fake bus in three modes and replaces the quarantine
assertion with **per-access assertions**: every logged MMIO access
must sit in the qualified map (`q-snapshot`: reads only, no engine
write; `q-wake`: the full control path, `wake_seen=1`, five setup
writes; `q-peek`: wrapper and mailbox reads allowed only under their
opt-ins). The mutation control still rejects the ungated build. All
eight modes pass, and `make -C ane/h17 script-test` (the runner
regression) passes.

The module `.ko` was rebuilt against kernel `7.2.11-6-1-ARCH`; the
on-disk module refuses at init as before. A host build is not device
qualification: nothing in this receipt is measured behaviour of the
T8140 ASC.

## Status

- Criterion 1 (source-backed map): met — the table above, each row
  with a source or explicitly gated.
- Criterion 2 (power sequence): met at source level — `0xc008` raised
  before the first engine access (default), same as the macOS
  power-up path; the retired RUN path removes the remaining
  CPU_CONTROL ordering question.
- Criterion 3 (module/sim re-derived row by row): met — this receipt.
- Criterion 4 (hang containment: pending marker + panic
  timeout/watchdog): **open — the gate stays closed until it is
  designed.**
