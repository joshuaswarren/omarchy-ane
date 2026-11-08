# J700/H17 safe round: stage-3b re-confirmation and offline pinning limits

Offline source work plus PMGR-only hardware reads. No engine-window
access, no ps-word write, no firmware, no reboot.

## Hardware: stage 3b re-run after the reboot and stage-3c vote

Boot `3134730e-2fb8-49ae-aa43-42f37ec08457`, kernel
`7.2.11-14-1-ARCH`, preflight PASS.

```
ane_h17_cpu_ps ane_sys off=0x290  word=0x23ff    target=0xf actual=0xf
ane_h17_cpu_ps sub0    off=0xc000 word=0xf000300 target=0x0 actual=0x0 was_gated=0x3
ane_h17_cpu_ps sub1    off=0xc008 word=0x300     target=0x0 actual=0x0 was_gated=0x3
ane_h17_cpu_ps sub2    off=0xc010 word=0x300     target=0x0 actual=0x0 was_gated=0x3
ane_h17_cpu_ps sub3    off=0xc018 word=0x0       target=0x0 actual=0x0 was_gated=0x0
ane_h17_cpu_ps RESULT stage=3b verdict=PASS reason=pmgr-read-ok
```

Identical to the pre-reboot measurement: ANE_SYS on, every sub-domain off
after the stage-3c vote released, `0xc018` all-zero.

## Offline findings

- **IRQ status/ack are not in the ANE interface text.** The interrupt
  handler (`0xfffffe0008f5721c`) reads field 336 and writes field 340
  through the ANE control (`[x26, #800]`, `x26 = this + 0xe000` →
  `this + 0xe320`), so the driver expects two configured register
  offsets — but no 32-bit store to either field exists in the driver
  text, and no `0x18xxxxx` constant appears in the ane-type `0x200`
  branch. They stay unpinned.
- **The mailbox is not in the ANE interface either.** The H16 header
  already says the mailbox belongs to the RTBuddy transport, not the
  engine interface. No `0x1608xxx` constant appears in this text, so the
  H17 `mbox = 0x1608000` value remains a T8132-template candidate.
- **`0xc018` is not an island on this part.** It reads all-zero (no
  `WAS_GATED`, no `AUTO`), unlike `0xc000/0xc008/0xc010`, which all carry
  the pmgr flags. It is excluded from the genpd set.

## Why the engine read still waits

The stage-4 probe is built but not run: the first engine-window read can
stall the bus if the domain set is insufficient, and recovery is a
reboot. The user's current constraint forbids another reboot, so this
round stops at PMGR reads.

## Register-map status after this round

| Field | Value | Status |
| --- | --- | --- |
| `cpu_control` | `0x1600044` | pinned (kernelcache) |
| `cpu_status` | `0x1600048` | pinned |
| `rvbar` | `0x1050000` | pinned |
| `scratch0` | `0x1880020` | pinned |
| `ps_off[0]` ANE_SYS | `0x290` | pinned (ADT + hardware) |
| `ps_off[1..3]` | `0xc000/0xc008/0xc010` | pinned as ps-words and raised (stage 3c); role unassigned |
| `ps_off[4]` | `0xc018` | reads zero; not an island |
| `irq_status`/`irq_ack` | — | unpinned |
| `mbox` | `0x1608000` | candidate (T8132 template) |
