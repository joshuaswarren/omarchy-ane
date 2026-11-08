# J700/H17 stage-5 review: the drafted RUN release is incomplete

Offline review of `ane/h17/ane_h17_start_probe.c` against the shipped
H14G/H16 boot sequencer (`ane/t6021/ane_t6021_boot.h`,
`ane_t6021_boot_run`). Conclusion: **the drafted module must not run.**

## The documented sequence (shipped driver)

| Step | Action | Constant |
| --- | --- | --- |
| S1 | clear all 8 scratch cells; `SCRATCH6 = rtb_mode ? 0 : 1`; pulse `SCRATCH7 1 -> 0` | `BOOT_REG_SCRATCH0..7` |
| S2 | read RVBAR; write the composed entry only if not latched | `BOOT_REG_RVBAR` |
| S3 | `CPU_CONTROL = 0`, then `= 0x10` | `CPU_RUN_RELEASE` |
| S4 | poll A: `SCRATCH7 == 0x08042006` (READY) | `BOOT_ACK` |
| S5 | publish init suballoc DVA to `SCRATCH0/1` after a barrier | — |
| S6 | write wake word `0xf7fbdff9` to `SCRATCH7` | `BOOT_WAKE_REQ` |
| S7 | poll B: `SCRATCH7 == 0x08042006` (DONE), read back `SCRATCH0/1` | `BOOT_ACK` |

Two modes matter:
- **legacy / ChMan** (`SCRATCH6 = 1`): full S1–S7 handshake.
- **RTBuddy** (`SCRATCH6 = 0`): the firmware does **not** publish
  `BOOT_ACK` to SCRATCH7; aliveness is gated by the RTKit HELLO after
  the run returns.

## What the drafted module actually does

It performs only S3 and a single SCRATCH7 poll, then stops the CPU. It
omits:

1. the scratch clear and the `1 -> 0` pulse, so a **stale** `0x08042006`
   would read as success — the pulse exists precisely to make poll A
   unambiguous;
2. `SCRATCH6`, so the firmware is not told which transport mode to
   expect;
3. the publication of the init structure and the wake word, so the legacy
   path cannot complete;
4. the RTBuddy branch entirely.

Running it would at best time out; at worst it would report a false PASS
on a stale word and then power the CPU off mid-handshake.

## What this changes for J700

The J700 image is pre-staged by iBoot/exclave with RVBAR already latched
(`0x10000000001`, entry `0x10000000000`), and macOS drives it through the
exclave/RTBuddy service. That points at the **RTBuddy branch**
(`SCRATCH6 = 0`, HELLO-gated) rather than the legacy S1–S7 handshake — so
the correct next design is probably "select RTBuddy, release RUN, then a
mailbox/RTKit HELLO", not "poll SCRATCH7". The mailbox offset for H17 is
still unpinned (see the safe-round receipt), so that path is not ready.

## Action

- `ane_h17_start_probe.c` is kept as a draft and **removed from the
  Makefile's `obj-m`** so it cannot be built or loaded by accident.
- No further engine write is attempted until the sequence is redesigned
  against the RTBuddy branch and reviewed.
