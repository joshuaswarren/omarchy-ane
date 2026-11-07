# J700/H17 stage 3b: PMGR-only sub-domain read — all ANE islands off

Second device-qualified test in this project. Same safe class as stage 3:
one genpd vote on `ps_ane_sys`, then **PMGR reads only** of the ANE
sub-domain words. No ANE or DART MMIO, no engine access, no firmware,
no mailbox.

## Identities

- Kernel `7.2.11-6-1-ARCH`; boot ID
  `1d31ba24-5f08-4962-ae99-37accde4ad5c`.
- Repo HEAD at commit time (before this receipt): `465e440`.
- Module `ane/h17/ane_h17_cpu_ps_probe.ko`, built against the running
  kernel tree.
- Reuses the stage-3 overlay device (`omarchy,t8140-ane-ps-probe`) so no
  DT change and no reboot were needed.

## Command and result

```
sudo insmod ane/h17/ane_h17_cpu_ps_probe.ko optin=1   # exit 0

ane_h17_cpu_ps ane_sys off=0x290  word=0x23ff      target=0xf actual=0xf was_gated=0x3 busy=0
ane_h17_cpu_ps sub0    off=0xc000 word=0xf000300   target=0x0 actual=0x0 was_gated=0x3 busy=0
ane_h17_cpu_ps sub1    off=0xc008 word=0x300       target=0x0 actual=0x0 was_gated=0x3 busy=0
ane_h17_cpu_ps sub2    off=0xc010 word=0x300       target=0x0 actual=0x0 was_gated=0x3 busy=0
ane_h17_cpu_ps sub3    off=0xc018 word=0x0         target=0x0 actual=0x0 was_gated=0x0 busy=0
ane_h17_cpu_ps RESULT stage=3b verdict=PASS reason=pmgr-read-ok
```

`sudo rmmod ane_h17_cpu_ps_probe` exited 0; `/proc/modules` is clean.

## What the words say

| Word | TARGET | ACTUAL | WAS_GATED | BUSY | Notes |
| --- | --- | --- | --- | --- | --- |
| `0x290` ANE_SYS | `0xf` | `0xf` | `0x3` | 0 | on (matches stage 3) |
| `0xc000` | 0 | 0 | `0x3` | 0 | off; bits 24–27 = `0xf` |
| `0xc008` | 0 | 0 | `0x3` | 0 | off |
| `0xc010` | 0 | 0 | `0x3` | 0 | off |
| `0xc018` | 0 | 0 | 0 | 0 | reads all-zero; may be unused on T8140 |

The ANE sub-domains are **gated off** while ANE_SYS is on. That confirms
the engine is not clocked/powered and that a blind engine read would
have been the documented hang class.

## What this proves

- The PMGR sub-domain chain `0xc000..0xc018` exists on T8140 and can be
  read safely from the always-on block.
- The ANE engine is **not** accessible yet: every sub-domain reports
  `ACTUAL=0`.

## What it does not prove

- Which word is the CPU island. The Apple ane-type `0x200` trace writes
  `0xc000` for CPU power, but the T8132/T8112 family uses `0xc008` for
  ANE_CPU and `0xc000` for ANE_MPM. The mapping needs one more
  qualification.
- The meaning of `0xf000000` in the `0xc000` word (bits 24–27), or of
  the all-zero `0xc018`.
- No engine register was read; no DART, firmware, or mailbox.

## Next step is gated on a planned reboot

To raise a sub-domain the safe way, it must be a registered genpd node:
the DTS patch's `ps_ane_sys_cpu@c000` (or `@c008`) and the pmgr driver's
TARGET-write + ACTUAL-poll. That requires updating the boot DTBO and
rebooting. A direct ps-word write from kernel context is the documented
hang class and is not attempted.

Once the DTBO carries the node, a stage-3c probe can vote it and read
`ACTUAL` — still no engine access until that reports `0xf`.
