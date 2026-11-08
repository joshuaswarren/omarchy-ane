# J700 / T8140 (H17): post-wake IRQ-status rung — status word reads 0x00000000

First hardware read of the H17 IRQ status word (`engine+0x188c000`, the
kext's IRQ-status register per the firmware-load-path receipt §11/§13.3),
taken on the same boot that reproduced the firmware wake for a fourth
time. The read is the opt-in `irq_read=1` diagnostic added in `834fc36`,
with the runner verdict hardening from the independent review in
`b399438`.

## Run

Boot `a9c7fa8d-1b87-4638-aeae-25c3270074d4`, kernel `7.2.11-14-1-ARCH`,
Apple MacBook Neo (J700/T8140). Gated one-shot
`omarchy-ane-h17-bootwake.service` (pending-file gate, self-clearing)
ran `ane/h17/run-bootwake.sh`:

```
insmod ane/h16/ane_h16.ko optin=t8140 stage=boot hello_wait_ms=1000 irq_read=1
```

Verdict (`verdict.txt`, copied beside this file):

```
rung=PASS reason=wake-irq-status-and-poll-skipped irq_status=0x00000000 irq_lines=1 boot_id=a9c7fa8d-1b87-4638-aeae-25c3270074d4
```

Device lines (`dmesg-rung.log`, t+15.4 s):

```
SCRATCH7 wake word 0x8042006: firmware is running (CPU_STATUS 0x6c)
H17 IRQ_STATUS off=0x188c000 value=0x00000000 (single read-only post-wake snapshot; ack register untouched)
H17 post-wake RTKit management poll skipped: H16 mailbox offset is unqualified; RESULT covers wake only
ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-wake
CPU release attempted: boot_error=0; retaining binding, mappings and power; do NOT unload; reboot required
```

Exactly one `H17 IRQ_STATUS off=0x188c000` line in the rung slice
(`irq_lines=1`): the single-read contract held on silicon as it does in
the `pos-h17-irq` sim. No other IRQ offset was touched — the ack word
`0x1890000` was never read or written, no doorbell was rung and no task
was submitted.

## Observation

`IRQ_STATUS (0x188c000) = 0x00000000`, read once ~5 ms after the
SCRATCH7 wake word (`0x08042006`), i.e. after the recipe steps 19–21 and
with the RTKit management poll still skipped.

What this value says: at the snapshot instant none of the 16 W1C
firmware→host IRQ lines was raised — the wake handshake itself is
scratch-word based and does **not** leave an IRQ line set.

What it does not say: it does not show the firmware is silent in
general. A line that was raised and hardware- or firmware-cleared
before the snapshot, or one raised later (after channel registration),
is indistinguishable from "never raised" by this one read, and the
snapshot predates any host→firmware doorbell. A register table is not
device behaviour, and one value is not a protocol: treat
`0x00000000` as a data point for the first command rung (which will
re-read this word after ringing), not as proof of channel state.

## Identities (recorded by the run itself)

| Item | Value |
|---|---|
| Boot id | `a9c7fa8d-1b87-4638-aeae-25c3270074d4` |
| Kernel | `7.2.11-14-1-ARCH` |
| Repo head at run | `b399438` (clean tree, pushed to `mine/neo/h17-bringup`; run.log `dirty=0`) |
| Module SHA-256 | `6da8a0669b95628810bea3cdde9331a233ecab8a5784139b0011db31c309bed5` (`ane_h16.ko` from `b399438`) |
| Firmware SHA-256 | `32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c` (`h17_ane_fw_theia_d9x.macho`) |
| Staged image | accepted_diff_bytes=2065, SHA-256 `10d97ffe304993666e9e685ab34bf7cc347257a9a41d1429daffb7cd5cef4e65` (diff-count unchanged, stage hash new — the per-boot byte variability recorded in the [wake receipt](../2026-11-04-j700-h17-boot-wake-poll-skip/README.md)) |
| RVBAR | latched at the staged entry `0x10000000000` (as every prior wake) |
| CPU_STATUS | `0x30006a` pre-wake → `0x6c` at the wake word |
| DT identity | `boot.bin` `7dc4c03f…` with J700 DTB `e000eaaf…` embedded at offset 9780165; the overlay re-enabled `ane@400000000` (okay) plus the three ANE DARTs (okay) and the `ane-fw-alias-iova` reservation, per the staged plan step 4 |
| Pre-run opt-in backup | `/var/tmp/opencode/dtb-overlays.opt-in.before-irq-rung` (the safe-DT state from the [reconcile receipt](../2026-11-04-j700-h17-safe-dt-reconcile/README.md) was restored by removing `ane-h16-experimental`; this run re-enabled it as planned) |

## Review trail (module side, before the run)

Independent review (full-file read of `ane_h16_main.c`, both SoC rows,
the sim harness and the runner; sim rebuilt in a temp dir, no hardware):

- (a) no MMIO path other than the single `readl_relaxed(engine +
  0x188c000)`; `irq_ack` has zero references in the module;
- (b) default-off behaviour unchanged (`pos-h17-boot` now asserts zero
  access at both status and ack words);
- (c) the timeout path `goto rel_fw`s past the read — no read without
  the wake word;
- (d) two runner holes (unpinned offset, unchecked count) — fixed in
  `b399438` and exercised with synthetic log fixtures (PASS, MISSING,
  MULTIPLE, wrong-offset all classify correctly);
- (e) the bare `ane_h16_rd32` bypasses the ordering helper's
  log-before-access discipline — accepted for a constant compile-time
  offset inside the mapped window; a future parameterized rung should
  route through `ane_h17_order_rw_check`.

## Stop rule

The module pins itself and holds power (`refcount 1`; remove refuses).
**Never `rmmod`; do not reload.** Any further rung — including the
first doorbell-and-re-read rung — starts from a fresh reboot, and the
[§13.4](../2026-11-04-j700-h17-firmware-load-path/README.md) sequence
applies: build the 64-byte RTKit message, enqueue at the channel write
index, store `0x40` into the shmem doorbell word, and re-read
`0x188c000` for a newly raised line.

Raw evidence: `verdict.txt` and `dmesg-rung.log` beside this file; full
dmesg at
`/var/tmp/opencode/h17-bootwake-a9c7fa8d-1b87-4638-aeae-25c3270074d4/dmesg-full.log`.
Host build, offline audit and one register value: this is not
command-path qualification.
