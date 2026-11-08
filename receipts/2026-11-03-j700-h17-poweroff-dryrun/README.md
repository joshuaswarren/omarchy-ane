# J700/H17: teardown dry run — PWGATE is already in the power-off state

Boot `c647ceb2-47b2-4fb5-8bd3-d409d71c68e3`, kernel `7.2.11-14-1-ARCH`,
cold boot (shutdown, charger disconnected >10 s). Uptime 552 s at the
probe, 860 s at the last check. No reset.

## Command

```sh
sudo insmod ane/h17/ane_h17_poweroff_probe.ko optin=1   # apply=0 default
sudo dmesg | grep ane_h17_poweroff
sudo rmmod ane_h17_poweroff_probe
```

Module SHA-256
`03f595aee56d6a5c4183738895519636a5896220a0fb44147b36069d13124912`,
commit `d2a645b`.

## Observed

```
sub0(0xc000)=0x1f0003ff target=0xf actual=0xf auto=1
sub1(0xc008)=0x3ff      target=0xf actual=0xf auto=0
sub2(0xc010)=0x3ff      target=0xf actual=0xf auto=0
ps_cpu(0xc000)=0x1f0003ff target=0xf actual=0xf auto=1
DRYRUN pwwindow=0x300724000 0x268c=0x3 0x26b0=0x0
RESULT verdict=PASS reason=dry-run-read-ok
```

Post-unload: `ane_sub0/1/2` all `off-0`; no PMU boot error; same boot id.

## What this establishes

1. The PWGATE window at the ADT address `0x300724000` is readable with the
   domains raised, and its two named words read there:
   `0x268c = 0x3`, `0x26b0 = 0x0`.
2. **Those are exactly the values Apple's power-off sequence writes**
   (step 4 writes `0x3` to `0x268c`, step 6 writes `0x0` to `0x26b0`).
   While the ANE domains are *on*, the gate words already sit at the
   power-off target. So steps 4–7 of `DisableCPUClocksAndPower` are
   no-ops at this point, and the gate writes were never the missing
   teardown step.
3. `0xc000` reads `0x1f0003ff` with `APPLE_PMGR_PS_AUTO` (bit 28) set.
   `0xc008`/`0xc010` read `0x3ff` with the bit clear.

## The narrower hypothesis this leaves

The unqualified difference between Apple's teardown and ours is now
**bit 28 (`AUTO_ENABLE`) on `0xc000`**.

- Apple: `read 0xc000` → `write 0xc000 &= ~0x10000000` → poll bit 28
  clear → then gate → then `write 0xc000 := 0x300`.
- Ours: genpd release, which writes the TARGET field to 0 and polls
  ACTUAL, leaving bit 28 set.

If `AUTO_ENABLE` lets the power state re-assert itself after a bare
TARGET drop, that is a plausible mechanism for a state that survives a
warm reboot and clears on a cold one. Labelled inference: bit 28 is
`APPLE_PMGR_PS_AUTO_ENABLE` from the PMGR source, but nothing observed
here shows what the ANE hardware does with it.

`apply=1` runs the full nine-step sequence and will show whether clearing
bit 28 and parking `0xc000` at `0x300` is stable. It has not been run yet.

## Not established

- Whether `apply=1` is safe or idempotent — untested.
- Whether the gate words read the same when the domains are *off*; the
  dry run only sampled them with the domains raised.
- Any causal link between the teardown and the earlier resets. This
  receipt is a single clean run, not a repeat of the failing condition.
