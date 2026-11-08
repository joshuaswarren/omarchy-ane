# J700/H17: engine read + Apple teardown — clean, and it stays clean

Boot `c647ceb2-47b2-4fb5-8bd3-d409d71c68e3`, kernel `7.2.11-14-1-ARCH`,
cold boot (charger disconnected >10 s).

This is the combination that has not been run before: the stage-4 engine
read followed by the real power-off sequence, in one raise/release
lifecycle. Every earlier probe did the read and then dropped the domains
with a bare `dev_pm_domain_detach()`.

## Command

```sh
sudo insmod ane/h17/ane_h17_poweroff_probe.ko optin=1 apply=1 reads=1
sudo rmmod ane_h17_poweroff_probe
```

Module SHA-256
`6925043a9455ef3defeb02f98cfa2304472f5a932e74c5189e0ba4a5b7951b2c`,
commit `5a1db90` + the `reads` parameter (uncommitted at run time, now in
the tree).

## Observed

Reads, identical to stage 4 on boot `3134730e-…`:

```
CPU_STATUS(0x1600048)=0x30006a  RVBAR(0x1050000)=0x10000000001
SCRATCH0(0x1880020)=0x0  A2I(0x1608110)=0x20001  I2A(0x1608114)=0x20001
```

Teardown:

```
ps_cpu before=0x1f0003ff after=0xf000100 steps_failed=0
PWGATE 0x268c=0x3 0x26b0=0x0 before and after
RESULT verdict=PASS reason=apple-poweroff
```

At 1240 s (probe) and again at 1603 s: same boot id, no PMU boot error,
`ane_sub0/1/2` all `off-0`.

## Comparison with the failing runs

| Boot | Last ANE access | Reset | Interval |
|---|---|---|---|
| `3e32247f` | read probe 13:47 | 13:49 | ~2 min |
| `f1a4bae3` | read probe 13:41 | 13:43 | ~2 min |
| `b4c6a3e7` | read probe 14:36 | 14:39 | ~3 min |
| **`c647ceb2`** | **read + teardown 14:57+** | **none at 26 min** | **>6 min, no reset** |

## What this establishes

- Raising the domains, reading the five whitelisted engine registers, and
  then running the nine-step power-off sequence completes on T8140 with
  no abort, no timeout, no reset.
- The register values are unchanged from the previous boot's read, so the
  raise path is behaving reproducibly.
- This run passes the interval at which the last three reset boots failed.

## What this does not establish

- **That the teardown is what prevents the reset.** This is one clean run
  against three failing ones, on a boot that already differs (cold power
  cycle, no USB mass-storage chain, different uptime profile). A
  correlation fixed by changing two variables is not a mechanism.
- Long-run stability: the earlier clean boot `b4c6a3e7` also passed its
  probe and reset three minutes later. 26 minutes is better than that,
  not proof.
- Repeatability: one raise/read/teardown cycle in this boot. The next
  rung is several cycles in one module load.
- Anything about the engine beyond five read offsets.

## Correction: the machine reset anyway

The title of this receipt is wrong. Boot `c647ceb2` was checked clean at
1603 s (26.7 min) and **reset at ~15:29:16**, about 31.8 min into the
boot and **5.5 min after the last ANE access** (cycle 3 of the repeat
test at 15:23:46).

Evidence for the reset, from `journalctl -b -1` on the next boot:

- The last log line of any kind is 15:29:16, mid-restart of
  `wayshare.service` (restart counter 871). No kernel message after that.
- No panic, no OOM, no watchdog, no SError, no DART fault, no ext4 error.
- The next boot logs `PMU logged 1 boot error(s) and 0 panic(s)`.

So the Apple power-off sequence runs cleanly, clears bit 28, and does not
prevent the eventual reset.

### Revised timeline for this boot

| uptime | action |
|---|---|
| 552 s | dry run (domains raised, PWGATE read, no engine MMIO) |
| 891 s | `apply=1` (nine-step sequence) |
| 1240 s | `apply=1 reads=1` (engine reads + sequence) |
| 1620–1635 s | 3 × `apply=1 reads=1` |
| ~1910 s | **reset**, no log line, 5.5 min after the last access |

### What the whole day now looks like

| boot | ANE access | outcome |
|---|---|---|
| `f1a4bae3` | read probe | reset ~2 min later |
| `3e32247f` | read probe | reset ~2 min later |
| `b4c6a3e7` | read probe at 48 min uptime | reset ~3 min later |
| `7239761b` | none (warm boot after) | reset ~2 min |
| `192f5a9a` | none (warm boot after) | reset ~4 min |
| `c0a9ac9f` | none (warm boot after) | reset ~2.5 min |
| `c647ceb2` | cold boot, reads + teardown | **reset ~32 min**, 5.5 min after last access |

That pattern fits one model better than any other: **ANE access leaves a
state that trips the PMU some minutes later, and that state survives a
warm reboot.** The short no-ANE boots were still dirty from the previous
boot's probes; the cold boot cleared it, and new access re-dirtied it.

The teardown changed the delay (2–3 min → 5.5 min), not the outcome. That
could mean it removes part of the fault and leaves the rest, or that the
delay is simply variable.

### Next discriminator

The model predicts that a **pure genpd raise with no engine MMIO and no
raw PS writes** also resets, and that a cold boot with no ANE access does
not. Those two runs separate "engine read" from "raising the domains" as
the cause. Neither has been run.
