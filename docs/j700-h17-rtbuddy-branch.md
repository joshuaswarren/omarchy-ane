# J700/H17: RTBuddy-branch start design (offline)

The stage-5 review showed the drafted RUN release implemented only the
legacy SCRATCH7 handshake, and that only partially. This document redesigns
the start path for the branch J700 most likely uses: **RTBuddy / RTKit**,
not legacy ChMan.

Nothing here has run. All writes listed are design steps, not instructions.

## Why RTBuddy, not legacy SCRATCH7

- The J700 image is pre-staged by iBoot/exclave; stage 4 measured RVBAR
  latched (`0x10000000001`, entry `0x10000000000`).
- macOS owns the ANE through `com.apple.service.ANEExclave` and
  `ANEExclaveProxy` (RPC), not through the legacy direct-boot handshake.
- The shipped driver's RTBuddy branch (`SCRATCH6 = 0`) states the firmware
  does **not** publish `BOOT_ACK` to SCRATCH7 and no legacy publication
  follows; aliveness is gated by the RTKit HELLO. The legacy S4–S7 poll,
  publish and wake word therefore do not apply.

## Mailbox pinning

The mailbox is the ASC block at `ASC + 0x8000`. For H17 the CPU/ASC block
is `engine + 0x1600000` (the same `+0x200000` shift stage 4 confirmed), so:

| Register | Offset | Engine address |
| --- | --- | --- |
| mailbox base | `0x1608000` | `0x401608000` |
| A2I control | `+0x110` | `0x1608110` |
| I2A control | `+0x114` | `0x1608114` |
| A2I send 0/1 | `+0x800`/`+0x808` | `0x1608800`/`0x1608808` |
| I2A recv 0/1 | `+0x830`/`+0x838` | `0x1608830`/`0x1608838` |

Evidence for `ASC + 0x8000`:
- `ane/t6021/ane_t6021.h`: "The real ASC mailbox, at ASC+0x8000"; H14G
  constants `0x1408110/0x1408114/0x1408800/...` are exactly base `+0x110`
  etc. over `0x1408000`.
- `t8112.dtsi`: `ane_mbox` at `engine + 0x1408000` (`0x26b408000`).
- `ane/h16/ane_h16_soc.c`: the T8132 and T6040 rows both use
  `mbox = 0x1608000`.
- H16/H17 share the `0x1600000` CPU block, so the same `+0x8000` lands at
  `0x1608000`.

This was a family-pattern pin, and it is now **confirmed on hardware**:
the read-only probe (`receipts/2026-11-02-j700-h17-mailbox-confirmed/`)
reads `A2I_CTRL` and `I2A_CTRL` as `0x00020001` — the documented
pre-boot ASC value (enabled, empty, no traffic). The interface text still
carries no mailbox constant (it belongs to the RTBuddy transport), so the
confirmation is behavioural, not a symbol trace.

## RTBuddy start sequence (design)

1. **Domains.** Raise `ane_sys` + `ane_sub0/1/2` through genpd; require
   every PMGR word `ACTUAL=0xf` (stage 3c proves this works).
2. **Mode + scratch.** Clear SCRATCH0..7, set `SCRATCH6 = 0` (RTBuddy),
   pulse SCRATCH7 `1 -> 0` to clear stale state.
3. **RVBAR.** Read; require latched and non-zero. Do not write (it is
   already programmed).
4. **RUN release.** `CPU_CONTROL = 0`, then `= 0x10`.
5. **RTKit HELLO.** Drive the mailbox with the existing H14 protocol
   (`rtkit/h14_rtkit_hello.py`): receive HELLO, reply HELLO_REPLY with the
   chosen version, answer EPMAP, STARTEP the announced endpoints, echo
   SET_IOP_PWR_STATE_ACK.
6. **Report.** Record the first HELLO/EPMAP traffic; success is a reply
   observed and answered, not a SCRATCH7 word.

No legacy publication (SCRATCH0/1 init DVA) and no `0xf7fbdff9` wake word
are part of this branch.

## Open questions (must be answered before any run)

1. Does the J700 image actually speak RTKit? The H14 13.5 firmware sent no
   HELLO on any recorded boot (`hello_wait_ms` exists precisely for this).
2. Is `SCRATCH6 = 0` the right mode for a pre-staged J700 image that the
   exclave may have configured differently?
3. Does iBoot/exclave leave the firmware staged-but-bootable, or does the
   exclave normally perform additional setup we cannot see?
4. The mailbox base is now hardware-confirmed (`A2I_CTRL`/`I2A_CTRL` read
   `0x00020001`), so only the write-path questions remain.

## Risk

Steps 2 and 4 are engine **writes** (SCRATCH words, CPU_CONTROL).
The H16 sources record the pre-CPU table writes and the ps-word class as
machine-resetting, and this branch adds mailbox traffic. It needs a
reboot-tolerant session and independent review before execution.

## RTKit protocol detail (from `rtkit/h14_rtkit_hello.py`)

The repo already implements the first exchange for the H14 family; the
same layout applies here because the ASC block is at `0x1600000` and the
mailbox at `+0x8000`:

```
ASC       = 0x1600000
MBOX      = ASC + 0x8000          # 0x1608000
A2I_CTRL  = MBOX + 0x110
I2A_CTRL  = MBOX + 0x114
A2I_S0/S1 = MBOX + 0x800 / +0x808
I2A_R0/R1 = MBOX + 0x830 / +0x838
```

### Message encoding

`msg0` is 64-bit; the MGMT type lives in bits 59:52 (the **high** half).
`msg1` carries the endpoint in its low byte.

| Type | Value | Payload |
| --- | --- | --- |
| `HELLO` | 1 | min ver [15:0], max ver [31:16] |
| `HELLO_REPLY` | 2 | chosen ver in both fields |
| `STARTEP` | 5 | ep at bits 39:32, flag bit 1 |
| `SET_IOP_PWR_STATE` | 6 | state (`ON = 0x20`) |
| `SET_IOP_PWR_STATE_ACK` | 7 | echo |
| `EPMAP` | 8 | base [34:32], bitmap [31:0], LAST bit 51, MORE bit 0 |
| `SET_AP_PWR_STATE` | 0xb | state |

Version window: `RTKIT_MIN = 11`, `RTKIT_MAX = 12`.

### Mailbox access rules

- Receive: `I2A_CTRL` bit 17 is EMPTY; when clear, read `I2A_R0` as two
  32-bit halves and `I2A_R1` low byte as the endpoint. **Receive is
  pop-on-read** — a 32-bit or memcpy-style read would eat a message.
- Send: wait until `A2I_CTRL` bit 16 (FULL) is clear, write `A2I_S0`
  (two 32-bit halves), then `A2I_S1` with the endpoint, then wait for
  FULL to clear again. The FIFO is one deep; never overwrite an in-flight
  slot.

### Endpoint sets

- System: `(1, 2, 3, 4, 8, 0xa)` — crashlog, syslog, debug, ioreport,
  oslog, tracekit (per upstream `rtkit.c`).
- Application: `(1..6)` — the kext's `InitializeRTBuddyEndpoints` opens
  exactly these; ep0 is an empty slot.

### What the J700 run would need to observe

A first success is not a SCRATCH7 word but a **HELLO** read from I2A and a
HELLO_REPLY written to A2I. If no HELLO arrives, the image either does not
speak RTKit or was not released correctly — the same failure the H14 13.5
firmware showed on every recorded boot.

## Prerequisite surfaced 2026-11-03: the ANE clocks are unmodelled

The full ADT (`/dev/mtd1ro`) gives the ANE node:

```
clock-ids   = <0x147 0x146 0x145>   # 327, 326, 325
clock-gates = <0x123 0x152>         # 291, 338 = ANE-SYS-V, ANE-SYS-V-PMP
power-gates = <0x123 0x152>
```

Linux models none of them: no PMGR clock driver in this tree, no
`clock-controller@` nodes in `t8140-pmgr.dtsi`, and neither the H16 nor
the T6021 driver votes clocks. The Apple driver votes them through
`enableDeviceClock` over the `clock-ids` table.

The ID-to-register map is not in the ADT; it lives in
the platform-IO driver's `function-clock_gate`. See
`receipts/2026-11-03-j700-h17-clock-gap/`.

Any engine read or write should wait for this to be closed: a gated clock
is the best explanation for the intermittent PMU-level resets.
