# J700/H17: the engine IS writable — in Apple's order

Boot `44aef287-5803-4a45-bbb0-e066d90914e9`, run at uptime 1292 s.
Commit `6c6c2c3`, clean and pushed, `prepare-run.sh` reported `ARMED`.

**No freeze. Every accepted write read back. The CPU state changed.**

```
=== apple-order run at uptime 1292s ===
domains raised ps_cpu=0x1f0003ff actual=0xf (B1/B2 clock+power votes NOT done)
C5 CE_A(0x18a4000) was 0x0, wrote 0x3, now 0x3
C6 CE_B(0x18a4008) = 0x3
D1 PS 0xc000 := 0xf            -> 0xf0003ff
D4 PWGATE 0x268c := 0          -> 0x0
D6 PS 0xc000 := 0x1000000f     -> 0x1f0003ff
E1 PS 0xc008 := 0xf            -> 0x3ff
G1-G8 SCRATCH0..7 := 0         all read 0x0 before and after
G9 SCRATCH6 := 1               -> 0x1     (retained)
G10 SCRATCH7 := 1              -> 0x0     (not retained)
G11 SCRATCH7 := 0              -> 0x0
G13 RVBAR = 0x10000000001
G14 RVBAR := 0x81010000000001  -> reads 0x10000000001 (write did not stick)
G16 CPU_CONTROL := 0           -> 0x0  status 0x6a
G17 CPU_CONTROL := 0x10        -> 0x10 status 0x68
sequence complete, status 0x68
released
```

## Why this is the result we have been missing

Two earlier runs proved that an unordered engine write freezes the
machine instantly:

- writing `0` to `CPU_CONTROL` (`0x1600044`) as the first engine write;
- writing `0` to `SCRATCH0` after the power-on sequence.

Both hung the box. This run wrote to `0x18a4000`, `0x18a4008`,
`SCRATCH0..7`, `RVBAR`, `CPU_CONTROL` **and completed**, with the machine
still up at the end.

So "the ANE rejects writes" was wrong. What matters is the order, and
the step we had never done is the **first one Apple does**:

`0x18a4000` read `0x0`, we wrote `v ^ 3` = `0x3`, it read back `0x3`, and
`0x18a4008` then read `0x3`. Apple performs exactly this 2-bit
handshake before any other ANE MMIO
(`receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md`,
steps C5–C8).

The mechanism is not established. Candidates: the handshake releases a
block that then tolerates writes, or the earlier freezes were a
side-effect of writing `CPU_CONTROL`/`SCRATCH` while that block was
uninitialised. Labelled inference — one clean run.

## Also established

- `SCRATCH6 := 1` **sticks** (`0x1`). The repo's design doc said
  `SCRATCH6 = 0` for RTBuddy; the direct-boot family `0xe0` path writes
  **1**.
- `SCRATCH7` does not retain a written value; it reads `0x0` after both
  the `1` and the `0`.
- `RVBAR` is **not** writable from here: Apple's exact
  `(v & 0xff7dfffffffff800) | 0x0081000000000001` was written and the
  register still reads `0x10000000001`.
- `CPU_CONTROL := 0x10` **is** accepted and changes `CPU_STATUS`:
  `0x6a -> 0x68`. The RUN bit does something.
- `CPU_STATUS` after the power-on sequence reads `0x6a` (`0x30006a`
  before the gate is opened, `0x6a` after).

## Not established

- Whether the CPU is actually executing. `CPU_STATUS` moving `0x6a ->
  0x68` is a register change, not proof of execution. Success for this
  ladder is a HELLO on the mailbox, and the mailbox was not touched.
- Whether the run leaves the machine stable. It completed and released;
  the freeze pattern from earlier runs took minutes to appear.
- That the handshake is *required*. This is the first run that did it and
  the first that did not freeze; a control run that omits only C5/C6
  would separate the handshake from the ordering.
- Whether B1/B2 (provider clock and power votes) are still needed. They
  were not done here, and nothing hung — which is evidence, but not
  proof, that the block is clocked well enough for register access.

## Next

Keep the domains raised, run the sequence, and then poll the mailbox for
RTKit traffic (`A2I_CTRL 0x1608110` / `I2A_CTRL 0x1608114`,
`0x20001` pre-boot) instead of releasing immediately.
