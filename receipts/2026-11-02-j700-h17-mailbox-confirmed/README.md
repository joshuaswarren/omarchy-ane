# J700/H17: mailbox confirmed on hardware (read-only engine probe)

Read-only engine access. No engine write, no firmware start, no mailbox
message, no RECV read (receive is pop-on-read). The only write anywhere
is the genpd vote on the PMGR words, via the proven path.

## Identities

- Kernel `7.2.11-14-1-ARCH`; boot ID
  `ee7d48fa-22da-4827-bf6a-dd76b310ee92`.
- `ane/h17/preflight.py`: `PASS`.
- Module `ane/h17/ane_h17_read_probe.ko`, vermagic `7.2.11-14-1-ARCH`.
- Domains before and after: `off-0`; the module raises `ane_sys` +
  `ane_sub0/1/2` for the read and releases on exit.

## Result

```
ane_h17_read CPU_STATUS(0x1600048)=0x30006a
ane_h17_read RVBAR(0x1050000)=0x10000000001 bit0=1 entry=0x10000000000
ane_h17_read SCRATCH0(0x1880020)=0x0
... SCRATCH1..6 all 0x0 ...
ane_h17_read SCRATCH7(0x188003c)=0x0
ane_h17_read A2I_CTRL(0x1608110)=0x20001
ane_h17_read I2A_CTRL(0x1608114)=0x20001
ane_h17_read RESULT verdict=PASS reason=read-only-ok
```

`rmmod` exited 0; all domains returned to `off-0`.

## What this proves

- **The ASC mailbox is at `engine + 0x1608000`.** The two control words
  respond with `0x00020001`, exactly the value the H16 boot notes record
  for a pre-boot ASC mailbox (`ENABLE=1`, `EMPTY=1`, `FULL=0`,
  `OVERFLOW=0`, `FIFOCNT=0`, pointers at origin). The mailbox pin is now
  a measurement, not a family-pattern inference.
- **The mailbox is enabled and has carried no traffic**: both directions
  read the same empty state, so no message has crossed since reset.
- **SCRATCH0..7 are all zero**, i.e. no firmware READY/DONE publication
  and no leftover wake word. `SCRATCH6 = 0`, which is the RTBuddy-mode
  value, is consistent with the RTBuddy branch being the right one.
- CPU_STATUS and RVBAR reproduce stage 4 exactly (`0x30006a`, latched
  entry `0x10000000000`), so the read is stable across boots.

## What it does not prove

- No engine write and no firmware start: the CPU still reads
  stopped/idle, and nothing has been told to run.
- No HELLO was sent or received; the mailbox is confirmed present, not
  proven to speak RTKit.
- Reading one offset set does not qualify the neighbourhood; the
  fabric-fatal ranges remain untouched.

## Effect on the design

`docs/j700-h17-rtbuddy-branch.md` can drop its first open question about
the mailbox base. Remaining open questions: whether the J700 image speaks
RTKit at all, and whether `SCRATCH6 = 0` is the mode a pre-staged,
exclave-owned image expects. Both are write-path questions.
