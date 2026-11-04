# J700/H17: mailbox silent; the CE handshake and the AF block do not behave

Same boot `44aef287-5803-4a45-bbb0-e066d90914e9` as the engine-writable
run. Commit `739e16b`, `prepare-run.sh` armed.

## Run 1 — Apple order, mailbox observed (boot probe)

```
G17 CPU_CONTROL := 0x10   -> 0x10  status 0x68
sequence complete, status 0x68
poll 0 status=0x68 sc7=0x0 a2i=0x20001 i2a=0x20001 ctrl=0x10
observation done status=0x68 sc7=0x0 a2i=0x20001 i2a=0x20001
```

Three seconds of polling at 10 ms found **no change at all**: no RTKit
traffic on `A2I_CTRL`/`I2A_CTRL` (both stay `0x20001`, the documented
pre-boot value), no legacy publication on `SCRATCH7`, no `CPU_STATUS`
movement after `0x68`.

So `CPU_CONTROL := 0x10` is accepted and changes the status word, but
nothing demonstrably executes. This is not evidence that the CPU is
running.

## Run 2 — the full HAL C-block (full probe)

```
C1 0x18a4010 v=1  C2 0x18a4018 s=0x0
C5 0x18a4000 := 0x2, now 0x2
  validate 0x18a4008 off=0x18a4008 expect=0x2 mask=0x3 last=0x3 TIMEOUT
C6 0x18a4008=0x3
C7 0x18a4018=0x0
C8 0x18a4008 := 0x2 (64-bit), now 0x1
C9 34 regs OR 0x100000 + 0x1137a50 OR 0x200000 done
C10 secure_lock_wr_dis done
C11 secure_lock_lock done
C-block complete 0x1137ac8=0x0
```

Three anomalies, all real:

1. **The handshake did not complete.** Apple writes `v ^ 3` and then
   requires `0x18a4008 & 3 == v ^ 3`. Here `v = 1`, so it wrote `2`, and
   `0x18a4008` stayed `0x3`. Apple's own code **panics** on this timeout;
   we log and continue.
2. **The 64-bit write is accepted as `1`, not `2`.** `C8` wrote `2` to
   `0x18a4008` as a 64-bit store and it reads back `0x1`.
3. **The AF block writes do not stick.** After writing `0x1137ac8 |=
   0x200000`, it reads `0x0`. The C9 34-register OR and the C10/C11
   secure-lock writes all reported "done" because nothing checks them,
   and the one register we sampled afterwards is unchanged.

## The state drifts inside one boot

The three engine runs on this boot saw different starting states:

| run | uptime | `0x18a4010 & 3` | `0x18a4008` before | `0x18a4000` written |
|---|---|---|---|---|
| apple-order | 1292 s | 0 | `0x20001`-era, CE_A read `0x0` | `0x3` |
| boot | 1340 s | — | — | — |
| full | 1376 s | **1** | **`0x3`** | `0x2` |

`0x18a4008` held `0x3` from the earlier run, so the second handshake
started from a state Apple never sees in a fresh boot and timed out.

**Conclusion: one engine experiment per boot.** Every run after the first
on a boot is measuring residue, not the bring-up path.

## What this does not establish

- That the mailbox is silent because the CPU is not running. It could
  also be that RTKit requires the AP to speak first, or that the firmware
  is not staged in a state we can start.
- That the AF-block writes fail for a hardware reason. The reads may be
  returning zero because the block is unclocked — which is the same
  hypothesis as before, now with a second block showing it.
- Whether the C6 handshake is even required, or whether the previous
  no-freeze run succeeded for another reason.

## Next

Cold boot, then exactly one run of the full probe, so the C-block sees
the state Apple sees. Anything measured after a second run on the same
boot should be discarded.

## Cold-boot rerun (the only clean measurement)

Boot `a6917626-7f54-4b6c-b758-c562eef44b6e`, cold boot, uptime 114 s,
single run of the full probe, then the domain released.

```
C1 0x18a4010 v=0  C2 0x18a4018 s=0x0
C5 0x18a4000 := 0x3, now 0x3
C6 0x18a4008=0x3            <- matches v^3, handshake satisfied
C7 0x18a4018=0x0
C8 0x18a4008 := 0x3 (64-bit), now 0x0
C9 34 regs OR 0x100000 + 0x1137a50 OR 0x200000 done
C10 secure_lock_wr_dis done
C11 secure_lock_lock done
C-block complete 0x1137ac8=0x0
...
G17 CPU_CONTROL := 0x10 -> 0x10 status 0x68
sequence complete, status 0x68
poll 0 status=0x68 sc7=0x0 a2i=0x20001 i2a=0x20001 ctrl=0x10
observation done status=0x68 sc7=0x0 a2i=0x20001 i2a=0x20001
```

On clean state the handshake **does** satisfy (`v = 0`, wrote `3`,
`0x18a4008` read `3`), so the earlier timeout was residue, as suspected.

Three things survive the clean run:

1. **The AF-block writes still do not stick.** `0x1137ac8` is written
   `|= 0x200000` and reads `0x0`.
2. **The 64-bit write to `0x18a4008` does not stick.** Wrote `3`, reads
   `0x0`.
3. **The mailbox stays silent.** `A2I/I2A = 0x20001` throughout,
   `SCRATCH7 = 0x0`, `CPU_STATUS` frozen at `0x68`.

## Where that leaves the hypothesis

Writes to the CPU block, the scratch block and the CE block all take
effect. Writes to the AF block (`0x1130000`) are dropped, and a 64-bit
store to the CE status word is dropped. That split matches an ANE whose
power state is up but whose **clock gates were never voted**: the
always-on register file responds, the functional blocks do not.

That is still inference. But it is now the only unexplained gap between
our sequence and Apple's: the provider clock and power votes (B1/B2)
before any MMIO.
