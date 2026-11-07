# J700/H17: genpd raise alone, no engine MMIO

Boot `8ee27fc5-c329-4e24-9841-0c72d0f71a5a`, kernel `7.2.11-14-1-ARCH`,
cold boot at 18:16:14 PST.

## Baseline was cut short

The zero-ANE cold-boot baseline
(`receipts/2026-11-03-j700-h17-cold-boot-baseline/`) was stopped at
**285 s (4.7 min)**, not the 40 minutes it needed. At that point the host
was clean: same boot id, no ANE modules, no PMU boot-error line.

4.7 minutes is past the 2–4 minute window in which the three warm no-ANE
boots died, so the cold boot had already outlived them. It is **not** a
completed baseline: the last ANE boot survived 32 minutes, so a reset
between 5 and 40 minutes would still be consistent with an unclean host.
This run is therefore not evidence that the host is stable on its own.

## This run

```sh
sudo insmod ane/h17/ane_h17_cpu_vote_probe.ko optin=1
sudo rmmod ane_h17_cpu_vote_probe
```

At uptime 302 s:

```
ane_sys off=0x290  word=0x23ff      target=0xf actual=0xf was_gated=0x3 busy=0
sub0    off=0xc000 word=0x1f0003ff  target=0xf actual=0xf was_gated=0x3 busy=0
sub1    off=0xc008 word=0x3ff       target=0xf actual=0xf was_gated=0x3 busy=0
sub2    off=0xc010 word=0x3ff       target=0xf actual=0xf was_gated=0x3 busy=0
RESULT stage=3c verdict=PASS reason=all-subdomains-on
```

What this module does and does not do:

- It attaches to the three sub-domains as genpds and adds runtime-active
  device links, so the pmgr driver performs the TARGET write and ACTUAL
  poll itself.
- It reads only PMGR words. **No engine MMIO. No raw PS write. No PWGATE
  access. No teardown sequence.**
- `rmmod` drops the links, so the domains fall back to `off-0` through
  the driver's normal path.

`0xc000` again reads `0x1f0003ff` — bit 28 (`AUTO_ENABLE`) set by the
raise, exactly as seen on the previous boot.

## Why this run matters

It is the first ANE experiment on this board that cannot touch the engine
window at all. If the host still resets after it, the engine read is
exonerated and the fault is in raising the domains (or in the host). If
it stays up, the engine access is back as the prime suspect with the
raise path cleared.

## Result

Pending. Probe passed at 302 s; no reset at the time of writing.
