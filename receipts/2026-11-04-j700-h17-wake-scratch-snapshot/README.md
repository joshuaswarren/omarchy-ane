# J700 H17 — post-wake SCRATCH0..7 snapshot rung (PASS, boot 3f9aa97c)

Date: 2026-11-04. Host: Apple MacBook Neo (J700/T8140), kernel
`7.2.11-14-1-ARCH`, repo `neo/h17-bringup` at
`c52cbdb5106917588d7002ecb59c4acfc6bdc228` (dirty=0). Boot id:
`3f9aa97c-271b-431c-984c-ecf60ffd89c7`.

## What this rung is

The third opt-in read-only diagnostic in the boot-wake path (`ane_h16`,
`scratch_read=1`): after the SCRATCH7 wake word confirms firmware start,
the module reads the eight ANE scratch words (`0x1880020..0x188003c`)
exactly once each and logs them in order. No new write is added: the only
scratch writes are the boot recipe (clear SCRATCH0..7, SCRATCH6 := 1,
SCRATCH7 := 0) that the previous rungs already performed, and the CPU
release. No doorbell, no task, no IRQ ack access. The IRQ-status read
(`irq_read=1`) runs in the same boot. Gated by
`/var/lib/omarchy-ane/h17-bootwake.pending`, one-shot
`omarchy-ane-h17-bootwake.service`; runner
`ane/h17/run-bootwake.sh` requires exactly one line pinned to
`H17 SCRATCH off=0x1880020 values=0x…(,0x…){7}` plus the IRQ line and the
RESULT PASS, and the six-fixture verdict test
(`ane/h17/test-bootwake-verdict.sh`) was green before the run.

## Verdict (verdict.txt)

```
rung=PASS reason=wake-irq-scratch-status-and-poll-skipped irq_status=0x00000000 irq_lines=1 scratch_words=0x00000008,0x0000c440,0x00000100,0x00f00000,0x00000000,0x00100090,0x00000001,0x08042006 scratch_lines=1 boot_id=3f9aa97c-271b-431c-984c-ecf60ffd89c7
```

## Observed values (dmesg-rung.log, raw)

```
SCRATCH7 wake word 0x8042006: firmware is running (CPU_STATUS 0x6c)
H17 IRQ_STATUS off=0x188c000 value=0x00000000
H17 SCRATCH off=0x1880020 values=0x00000008,0x0000c440,0x00000100,0x00f00000,0x00000000,0x00100090,0x00000001,0x08042006
ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-wake
```

Post-wake scratch snapshot (single read each, this boot):

| word | offset | value |
|---|---|---|
| SCRATCH0 | `0x1880020` | `0x00000008` |
| SCRATCH1 | `0x1880024` | `0x0000c440` |
| SCRATCH2 | `0x1880028` | `0x00000100` |
| SCRATCH3 | `0x188002c` | `0x00f00000` |
| SCRATCH4 | `0x1880030` | `0x00000000` |
| SCRATCH5 | `0x1880034` | `0x00100090` |
| SCRATCH6 | `0x1880038` | `0x00000001` |
| SCRATCH7 | `0x188003c` | `0x08042006` |

`IRQ_STATUS` read again `0x00000000` (second boot with that observation;
the wake handshake still raises no IRQ line). `CPU_STATUS 0x6c` at wake,
matching boot a9c7fa8d. Staged preload accepted with 2065 diff bytes
(same count as prior rungs; this boot's stage sha
`2e24adec26c9c530e50154c0d7631e04f77e771233ffe31ccf851000cad5bc3b`).

## Reading against the §14 handshake (receipt
## receipts/2026-11-04-j700-h17-firmware-load-path §14)

Static expectations were recorded in §14.4 before this boot. Observed:

- `SCRATCH0 = 0x00000008` after CPU release. §14.1 step 4 reads
  `count := read32(SCRATCH0)` before the host publishes its table
  address; `ANE_Init` pre-fills 8 channel slots (`dev+0x3ef0+0x10i`).
  A firmware-published value of 8 here is numerically consistent with
  that 8-slot model — *consistent, not proof*; the kext's own count read
  runs on Apple's boot order, ours runs post-wake after different
  preceding writes.
- `SCRATCH1 = 0x0000c440`. In §14.1 step 4 SCRATCH1's low 32 bits feed
  `[dev+0xe1f4] := read32(SCRATCH1) + 1`; the kext never writes SCRATCH1
  before that read, so `0xc440` is firmware-side. Uninterpreted.
- `SCRATCH2 = 0x00000100`, `SCRATCH5 = 0x00100090`: §14 has no SCRATCH2
  role at all (the earlier "SCRATCH2 handshake" note was a scratch-numbering
  slip, corrected in §14.3) and no SCRATCH5 role; both are firmware-side
  values, unrecorded before this boot.
- `SCRATCH3 = 0x00f00000` without host involvement. §14.3 step 14 makes
  SCRATCH3 the host→firmware registration word (host writes
  `0x08042006`, firmware clears). We never wrote it; the firmware left
  `0x00f00000` there. This is the first observation of a non-zero
  firmware-side SCRATCH3.
- `SCRATCH6 = 1`, `SCRATCH7 = 0x08042006`: both are this driver's own
  boot-recipe writes / the firmware's reply word — expected, unchanged
  since boot a9c7fa8d.

What this rung does **not** establish: that SCRATCH0 is a channel count
on this boot order (it establishes only that the firmware writes 8
there), that any channel table exists or is readable, or any command
path. The next offline step stays static: §14.4's table-read rung design
publishes a host table-buffer ANE address via SCRATCH0/1 *writes* and
requests with SCRATCH7 `0xF7FBDFF9` — those are writes beyond the current
read-only whitelist and need their own gate and review before hardware.

## State after this rung

`ane_h16` is loaded and pins itself (`lsmod` shows refcount 1); power
held. **Never `rmmod`; a reboot is required before any further rung.**
One measurement per boot.

## Raw evidence

- `dmesg-rung.log` — the rung's kernel log window (status dump, h17-order
  sequence, wake word, IRQ snapshot, scratch snapshot, RESULT).
- `run.log` — runner console: boot id, repo head `c52cbdb`, `dirty=0`,
  `ane_h16.ko` and firmware sha256 lines.
- `verdict.txt` — the pinned verdict line.
