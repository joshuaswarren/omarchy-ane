# J700/H17: full read survey, logged to disk

Boot `8ee27fc5-c329-4e24-9841-0c72d0f71a5a`, kernel `7.2.11-14-1-ARCH`,
run at uptime 709 s. Cold boot at 18:16:14 PST.

## Why the log is a file

ANE access has frozen this machine repeatedly with no kernel message and
no panic, so a probe that reports through dmesg loses its evidence at the
exact moment it matters. `ane_h17_survey_probe.ko` opens
`/var/log/h17-survey.log` with `O_SYNC` and calls `vfs_fsync()` after
every line. Everything below survived the run on disk independently of
dmesg and of the session.

Commit `dbe15a7`; repo clean and pushed (`sync` completed) before the run.

## What it does

Raises the three sub-domains through genpd, reads the PMGR words, then
maps the ANE window and reads the register set that has been measured on
this board. Read-only against the engine. It does **not** run the Apple
power-off sequence; it releases the domains the way the earlier probes
did.

## Result

```
=== run at uptime 709s ===
probe start dev=soc:ane-h17-cpu-vote
domain 0 raised
domain 1 raised
domain 2 raised
sub0(0xc000)=0x1f0003ff target=0xf actual=0xf auto=1
sub1(0xc008)=0x3ff      target=0xf actual=0xf auto=0
sub2(0xc010)=0x3ff      target=0xf actual=0xf auto=0
CPU_CONTROL(0x1600044)=0x0
CPU_STATUS(0x1600048)=0x30006a
RVBAR(0x1050000)=0x10000000001
SCRATCH0(0x1880020)=0x0
SCRATCH1(0x1880024)=0x0
SCRATCH2(0x1880028)=0x0
SCRATCH3(0x188002c)=0x0
SCRATCH4(0x1880030)=0x0
SCRATCH5(0x1880034)=0x0
SCRATCH6(0x1880038)=0x0
SCRATCH7(0x188003c)=0x0
A2I_CTRL(0x1608110)=0x20001 I2A_CTRL(0x1608114)=0x20001
PWGATE 0x268c=0x3 0x26b0=0x0
survey complete
released
```

## Reading

- `CPU_CONTROL` (`0x1600044`) reads **0**: the CPU is held in reset. No
  write has ever been made to it on this board.
- `CPU_STATUS` `0x30006a` and `RVBAR` `0x10000000001` are unchanged from
  stage 4 and from every run since, so the engine comes up in the same
  state each time.
- All eight scratch cells read **0**, including `SCRATCH6`/`SCRATCH7`,
  which the RTBuddy branch would use to clear and pulse.
- Both mailbox controls read `0x20001`, unchanged.

## Not established

- Whether this run causes the freeze that follows ANE access. The run
  completed; the delay before any reset is minutes and variable.
- That `SCRATCH0..7` are all genuinely implemented cells rather than
  aliases of one register. An all-zero read across eight addresses is
  also what an unimplemented range returns; nothing here distinguishes
  the two.
