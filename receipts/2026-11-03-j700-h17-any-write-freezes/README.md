# J700/H17: any engine write freezes the machine — it is not the value

Boot at uptime 134 s, warm boot after the previous freeze. Repo at
`c8b7b04`, clean and pushed, `sync` completed before the run.

## The result

`/var/log/h17-write.log`, complete, recovered from disk after the reset:

```
=== run at uptime 134s cpu0=1 cells=0x1 ===
domains raised all_on=1
before RVBAR=0x10000000001 CPU_STATUS=0x30006a CPU_CONTROL=0x0
write BEGIN CPU_CONTROL(0x1600044) := 0
```

Then the machine was gone. No `write END` line, no teardown, no release.

The probe was configured to do two writes in order: `CPU_CONTROL := 0`,
then `SCRATCH0 := 0`. It died on the first one.

## Why this is stronger than the scratch result

`CPU_CONTROL` already read `0x0` before the write, logged on the line
above it. So this was a write of **the value the register already held**.
A register that reads `0` and rejects a write of `0` is not rejecting a
value; it is rejecting any write.

That rules out, for the first time with evidence:

- the specific value written (it equals the current contents);
- the scratch block (the probe never reached it);
- the RUN-release value `0x10` (never attempted);
- any sequence or ordering effect (one write, no sequence).

Combined with the survey run, which read every scratch cell, `RVBAR`,
`CPU_CONTROL` and both mailboxes and survived:

| operation | result |
|---|---|
| raise domains via genpd | survives |
| read engine registers | survives |
| run Apple power-off sequence (PMGR + PWGATE) | survives |
| **write any engine register** | **machine freezes immediately** |

## What this changes

"ANE access causes resets" was too coarse. Reads are demonstrably safe —
the survey boot ran 13 minutes after a full read sweep. The hazard is
**writes to the engine window**, and it is immediate, not delayed.

A delayed reset after a read-only boot is therefore a different (or a
secondary) phenomenon, and should not be explained by this result.

## Most likely cause, labelled inference

A write to a register block whose clock is not running. The ANE clock
gates are known to be unmodelled on Linux: the ADT names clock ids
`327/326/325` and clock gates `291/338`, and the arm-io provider exposes
them through an `AppleARMFunction` `<0x69,"clkG">` rather than a readable
table (`receipts/2026-11-03-j700-h17-clock-gap/`). The HAL does not touch
them either (`receipts/2026-11-03-j700-h17-offline-subagents/`).

Reads succeeding while writes wedge the bus is consistent with a powered
but unclocked block: retained state can be read back, but a write needs
the block's clock to complete and the transaction never retires.

Alternatives not excluded by this run:

- an ownership/permission gate on the engine window (the exclave analysis
  argued against this, but it argued about *reads* being possible);
- a fault that an SError should have reported, lost because the reset is
  a PMU-level action.

## Next

The clock question is now on the critical path: without a working write
path there is no RTBuddy start, no mailbox, and no submission. The next
work is offline — find whether the running kernel has a PMGR clock
provider that can enable the ANE gate, before spending another boot on a
write attempt.
