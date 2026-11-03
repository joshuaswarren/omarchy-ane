# J700/H17: the Apple power-off sequence runs clean on hardware

Boot `c647ceb2-47b2-4fb5-8bd3-d409d71c68e3`, kernel `7.2.11-14-1-ARCH`,
cold boot. Probe at uptime 891 s; last check 1199 s. **No reset.**

This is the first time the compiled `DisableCPUClocksAndPower` sequence
has been executed on T8140 by anything other than macOS.

## Command

```sh
sudo insmod ane/h17/ane_h17_poweroff_probe.ko optin=1 apply=1
sudo rmmod ane_h17_poweroff_probe
```

Module SHA-256
`03f595aee56d6a5c4183738895519636a5896220a0fb44147b36069d13124912`,
commit `d2a645b`.

## Observed

All nine steps reached their expected value (`steps_failed=0`):

```
ps_cpu before=0x1f0003ff after=0xf000100
PWGATE 0x268c=0x3 0x26b0=0x0 before and after
RESULT verdict=PASS reason=apple-poweroff
```

Bit-level, `0xc000`:

| | before | after |
|---|---|---|
| raw | `0x1f0003ff` | `0x0f000100` |
| TARGET (3:0) | `0xf` | `0x0` |
| ACTUAL (7:4) | `0xf` | `0x0` |
| bit 8 | 0 | 1 |
| bits 27:24 | `0xf` | `0xf` |
| **AUTO_ENABLE (28)** | **1** | **0** |

Post-unload: `ane_sub0/1/2` `off-0`, no PMU boot error, same boot id at
20 minutes.

## What this establishes

- The sequence is executable on T8140 from Linux through a raw syscon
  write to `0xc000` plus an ioremap of `0x300724000`, with no external
  abort, no timeout, and no reset.
- Writing `0xc000` does not require going through the pmgr driver's own
  state machine; the genpd reference is still held during the write and
  the driver's later detach is unaffected.
- Clearing `AUTO_ENABLE` (bit 28) sticks: it reads 0 afterwards.
- Bits 27:24 stay set across the sequence; they are not power state.
- Bit 9, written by the final `0x300`, clears itself; bit 8 remains.

## What it does not establish

- That this teardown prevents a reset. The read that preceded the earlier
  resets was **not** run in this experiment — there is no engine MMIO in
  this module at all. This run shows the teardown is safe; it says nothing
  yet about the read-plus-teardown combination, which is the actual
  untested case.
- That `0xc000 := 0x300` is idempotent across repeated raises. One cycle
  was run.
- Any causal link to the earlier resets.
