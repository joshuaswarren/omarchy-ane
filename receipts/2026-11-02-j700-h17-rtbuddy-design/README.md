# J700/H17: RTBuddy-branch redesign and mailbox pinning (offline)

No hardware access. Follows the stage-5 review that found the drafted RUN
release incomplete.

## Mailbox: pinned by family pattern

`ASC + 0x8000`, and the H17 CPU/ASC block is `engine + 0x1600000`, so the
mailbox base is `engine + 0x1608000` with A2I/I2A control at `+0x110`/
`+0x114`, send at `+0x800`/`+0x808`, receive at `+0x830`/`+0x838`.

Supporting evidence:
- `ane/t6021/ane_t6021.h:114` — "The real ASC mailbox, at ASC+0x8000";
  the H14G constants `0x1408000/0x1408110/0x1408114/0x1408800/…` are that
  base plus the same relative offsets.
- `t8112.dtsi` `ane_mbox` at `engine + 0x1408000` (`0x26b408000`).
- `ane/h16/ane_h16_soc.c` — the T8132 and T6040 rows both use
  `mbox = 0x1608000`.
- H16/H17 share the `0x1600000` CPU block.

This is **not** a J700 trace. The ANE interface text has no `0x1608xxx`
constant, the recorded ADT has no mailbox property, and the Linux DTS has
no ANE mailbox node. The ASC block cannot be confirmed without an engine
read, which the hardware halt forbids.

## RTBuddy start design

Legacy S4–S7 (SCRATCH7 READY/DONE, init publication, `0xf7fbdff9` wake) do
not apply: the shipped driver states the RTBuddy firmware does not publish
`BOOT_ACK` and aliveness is gated by the RTKit HELLO.

Designed sequence:
1. genpd raise + `ACTUAL=0xf` gate (proven by stage 3c);
2. clear SCRATCH0..7, `SCRATCH6 = 0`, pulse SCRATCH7 `1 -> 0`;
3. read RVBAR, require latched non-zero, do not write;
4. `CPU_CONTROL = 0`, then `= 0x10`;
5. RTKit HELLO exchange over the mailbox, reusing
   `rtkit/h14_rtkit_hello.py` (HELLO_REPLY, EPMAP, STARTEP,
   SET_IOP_PWR_STATE_ACK);
6. success = a HELLO observed and answered, not a SCRATCH7 word.

Full write-up: `docs/j700-h17-rtbuddy-branch.md`.

## Open questions

- Whether the J700 image speaks RTKit at all (the H14 13.5 firmware sent
  no HELLO on any recorded boot).
- Whether `SCRATCH6 = 0` is the mode a pre-staged, exclave-owned image
  expects.
- Whether the exclave performs setup the Linux side cannot see.

## State

Hardware access remains halted. The stage-5 draft stays quarantined
(not in `obj-m`, `module_init` returns `-EPERM`). The soc-row `mbox`
comment now records the family-pattern provenance instead of calling it a
bare candidate.
