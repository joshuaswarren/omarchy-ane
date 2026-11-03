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

This is a **family-pattern pin**, not a J700 trace: the ANE interface text
contains no `0x1608xxx` constant (the mailbox belongs to the RTBuddy
transport), the ADT carries no mailbox property, and the Linux DTS has no
ANE mailbox node.

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
4. The mailbox base is a family-pattern pin; a single read of
   `A2I_CTRL`/`I2A_CTRL` would confirm the ASC block responds, but that is
   still an engine-window access and stays behind the hardware halt.

## Risk

Steps 2 and 4 are engine **writes** (SCRATCH words, CPU_CONTROL).
The H16 sources record the pre-CPU table writes and the ps-word class as
machine-resetting, and this branch adds mailbox traffic. It needs a
reboot-tolerant session and independent review before execution.
