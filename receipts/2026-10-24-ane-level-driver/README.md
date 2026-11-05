# ANE clock level on the 16-inch (T6001, M1 Max), 2026-10-24

Linux never raised the ANE clock: every job ran at level 0 while macOS reaches
a higher level through the PMP. Four hardware runs on the M1 Max 16-inch
(macOS 26.6.2 trace for the order, Linux 7.1.12 for the runs) found the
sequence and the result; `ane/src/ane_dvfs.c` makes it permanent.

## What the PMP needs (all four parts together)

| Step | Write | Where |
| --- | --- | --- |
| 1 | DVFS_ON := 1 | pmgr DVFS block (the reference run RegMap 113 = pmgr ADT reg[39], PA 0x285868000) + 0x2000 |
| 2 | DVFS_CMD := 0x80000000 | same block + 0xa00 |
| 3 | PS-REQ ANE_SYS bit set, wait for PS-ACK | PMP PTD window (0x28e3c0000), update space +0x10000, reg 0xf8 / 0x100 |
| 4 | DVFS_CMD := 0x80000001, level word := 0x2000000000000001 | same block / PTD reg 0x10c |
| release | DVFS_CMD 0x80000010, level word 0x2000000000000000, PS-REQ bit cleared, DVFS_CMD 0x80000000, DVFS_ON := 0 | reverse |

The base of the DVFS block is not ADT reg[113]: macOS 25G83 maps RegMap 113 to ADT index 39. A first attempt at reg[113]
wrote the wrong block and changed nothing; the PMP-only writes (no pmgr words)
changed nothing either.

## Results (rung 20 of the h13 ladder, 200 calls, busy time per job)

| Level | busy per job | vs level 0 |
| --- | --- | --- |
| 0 | 1.0727 ms | 1.00 |
| 1 | 0.6471 ms | 0.603 |
| 2 (A=0) | 0.6331 ms | 0.594 |
| 2 (level word with A=3) | 0.6302 ms | 0.591 |

Qwen decode, one stream, 5 prompts of 64 tokens, step p50: shipped package 0.2009 s (level 0), 0.1158 s
(level 1), 0.1153 s (level 2); int8 package 0.1597 s, 0.0966 s. The logits of all five prompts are
bit-identical at levels 0, 1 and 2. Temperature stayed between 41.0 and 48.1 C over 8 minutes.
The DVFS_CMD word reads back as (level << 4 | level) after a transition, which confirms level 2 was reached
although the program does not get faster (it is memory-bound above level 1). Levels 3 to 5 were never written.

## The driver

`ane_dvfs_up()` runs at the end of runtime resume, `ane_dvfs_down()` at the start of runtime suspend (after
the busy veto). It does nothing unless the DT names the windows (`reg-names` "dvfs" and "ptd"). The up
sequence starts only on the hardware words the proving runs saw: PMP-STATUS 1, DVFS_ON 0, PS-REQ exactly
0x60003000, the ANE level word 0 or a level word 0..5. PS-REQ is read again right before the write (the
two 1 ms steps before it must not have changed it), and after the PS-ACK the PS-REQ readback must be exactly
0x60003400 before the level word is written. Any other value leaves level 0, with the reason in sysfs
`perf_state`; a missing PS-ACK within 50 ms undoes the sequence in the macOS order, and after three
timeouts the driver stops trying until it is reloaded. Release re-reads PS-REQ before it clears the bit.
All MMIO is ioremap_np. The probe error paths unwind in the order dvfs, boost, genpd, and a deferred probe
leaves no mapping and no sysfs file. The overlay package checks the installed T6001 device trees and, because libalpm does not
stop an installation when a scriptlet fails, removes its overlay file after extraction (and again after a kernel update, by hook)
when they are not the ones it was written for; the m1n1 rebuild hook runs after that and builds without the overlay.

`ane/test/dvfs_model_test.py` compiles the real `ane_dvfs.c` against fake kernel headers and checks the
exact write sequence up and down, the refusal paths (nothing written), the ACK-timeout unwind and the
read-modify-write.

## Evidence (exact sources, sha256)

| Claim | Source |
| --- | --- |
| reference run RegMap 113 is ADT reg[39] (PA 0x285868000, 0x4000 bytes) of the M1 Max pmgr | reference run 26.6.2 (25G83) maps RegMap 113 to ADT index 39 (48 `initRegMap(map, adtIndex)` calls); the pmgr node's IODeviceMemory list has index 39 = 0x285868000 length 0x4000 and index 41 = 0x28e3c0000 length 0x20000 |
| DVFS_ON at +0x2000, DVFS_CMD at +0xa00 of that block, the order of the writes, the command words 0x80000000 / 0x800000(old<<4 or new) | a dtrace entry trace of three ANE power cycles on that Mac under the reference run 26.6.2: `s2-trace-primary.txt` sha256 4915d96e179a5542fa5462b60ac0f31f9ec2b4aee1be2d01ba9d4a75039bfb75; decodes `ane-dvfs-write-seq-RE.md` sha256 4c1675cf92d82ebda63caf35e4aaf1a8faf6e29e6ec0c728217987697305c8cd and `ane-dvfs-ladder-RE.md` sha256 27000b7909c49f889d5140202cca63d13a174d6f1d5c8cc43c82ecc15e8fd0a8 |
| The PTD window (0x28e3c0000, read entities at +reg*16, update space at +0x10000 + reg*8), PS-REQ reg 0xf8, PS-ACK reg 0x100, PMP-STATUS reg 1, the ANE level reg 0x10c and the level word 0x2000000000000000 \| level | the PMP nub's `ptd-range` property in the same ADT and `ane-ptd-level-RE.md` sha256 8f4bb58f1202cec7889d5fee1789521ac96fc6c7ecb97f16f5f0bf81af51b979; the in-tree `drivers/pmdomain/apple/pmp-report.c` (T600x offsets tgt_read 0xf80, tgt_write 0x107c0, actual 0x1000, status 0x10) uses the same PS-REQ and PS-ACK entities |
| The sequence moves the clock and level 1 is the useful level | hardware runs on the M1 Max 16-inch with test modules that wrote exactly these words: run logs sha256 e67b4b8be4944554bac2a013c166e737c6cf672c91b8b127c56f5c91aebb77ac (level 1), 9460f2ef29500f84f29ac16f73bb820bd51f3590f24eb288889ba7188c50ca0e (levels 1 and 2, A=0 and A=3), 118893b60d58922c328be4e05c30f50d90439df12b76597b6e47558a7d5bc713 (decode); the one-hour soak through this driver: run log sha256 1528b0ff3b64373e299f10a49f35e943a947187f976b8b5fe42dda6e84d166de |
| The kernel's own device tree values the overlay depends on (ANE node `ane@285c04000`, phandles 0x15, 0x19 to 0x1c, 0x20) | the `t6001-j316c.dtb` of linux-aurora 7.1.12.aurora2-20261013.18, read with `fdtget`; `packaging/arch/omarchy-ane-level-dt` checks them again at install time |

This file and `ane/src/ane_dvfs.c` were first committed together in 2bfb216 of this pull request; the commits stay reachable because the pull request is merged with a merge commit.

## Device tree merge (checked offline)

`dtc -@` compiles `packaging/dt/t6001-ane.dts`; `fdtoverlay` merges it into the stock `t6001-j316c.dtb`:

```
fdtget -t x merged.dtb /soc/ane@284000000 reg           -> 2 85c04000 0 24000 2 85868000 0 4000 2 8e3c0000 0 20000
fdtget -t s merged.dtb /soc/ane@284000000 reg-names     -> engine dvfs ptd
fdtget -t x merged.dtb /soc/ane@284000000 power-domains -> 112 11a 11b 11c 11d 11e
fdtget -t x merged.dtb /soc/power-management@28e080000/power-controller@2e0 phandle -> 11e
fdtget -t s merged.dtb /soc/power-management@28e080000/power-controller@2e0 label  -> pms_sram
```

The sixth power domain (0x11e) is the phandle of the pms_sram controller.

## Tests

`python3 ane/test/dvfs_model_test.py` (the real `ane_dvfs.c` against fake kernel headers: write sequences, every
gate, ACK timeout and readback unwinds, the retry bound, init/exit balance of mappings and the sysfs file) and
`python3 ane/test/probe_unwind_check.py` (the probe's error-path labels and gotos).

## Not done here

- Soak: one hour of decode with runtime suspend and resume cycles on the 16-inch.
- T6000 and T6002 (same die-0 layout expected, ADT index 39 unverified): the overlay change is T6001 only.
- The kernel tree's ANE binding allows one `reg` for the T6000 compatible; the extra windows need a binding change
  before the nodes can go into the kernel's device trees.
