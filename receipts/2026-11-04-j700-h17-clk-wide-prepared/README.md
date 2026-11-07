# J700/H17: wide clock-window read module, prepared and built

Preparation only. **The module has not been loaded and no device was touched.**
No `insmod`, no `rmmod`, no `modprobe`, no MMIO, no genpd vote, no reboot.
Built at commit `43af9ac` against the running kernel's headers.

Follows `receipts/2026-11-04-j700-h17-clock-window-zero/`, where 24 words of
the window `0x3082c8000` (`0x1000..0x101c`, `0x1100..0x113c`) all read `0x0`
with no fault and no SError. Twenty-four zeros fit three different stories:
gated block, wrong base, or genuinely zero registers. This module widens the
read so that story selection has more data.

## Artefacts

| Path | |
|---|---|
| `ane/h17/ane_h17_clk_wide.c` | new source, SHA-256 `6adbadda3e4f7941866126eccfbc21f50b9a4fff1d36bc7851aa5bc5e2c44840` |
| `ane/h17/ane_h17_clk_wide.ko` | build output, SHA-256 `4e72af9b0a9fbb0f5ef939168f999319207514d62f0afacb57262da658ba8623` |
| `ane/h17/Makefile` | one added line in the `ifneq ($(KERNELRELEASE),)` branch: `obj-m += ane_h17_clk_wide.o` |

Module vermagic: `7.2.11-14-1-ARCH SMP preempt mod_unload aarch64` —
matches `uname -r` `7.2.11-14-1-ARCH` on this host.

## Build

```
make -C /home/admin/omarchy-ane/ane/h17 KERNELDIR=/lib/modules/7.2.11-14-1-ARCH/build
```

Result: **success, exit 0**, `LD [M] ane_h17_clk_wide.ko` and
`BTF [M] ane_h17_clk_wide.ko` both produced. `grep -ci '\berror\b'` over the
full build log returns `0`. The pre-existing kernel-header
`-Wattribute-alias`/`-Wmissing-prototypes` noise appears as before and is not
from this file; the compiler emits no diagnostic naming
`ane_h17_clk_wide.c` beyond those headers.

## Exactly what it reads

192 `readl` accesses inside the declared `0x4000`-byte window at
`0x3082c8000`, and nothing else. No writes to the window at all.

- **Range A — 64 reads:** `0x0000, 0x0004, 0x0008, … 0x00fc` (every 4 bytes
  of `0x0..0xff`).
- **Range B — 128 reads:** `0x1000, 0x1004, 0x1008, … 0x11fc` (every 4 bytes
  of `0x1000..0x11ff`).

The table in the source was checked mechanically against both ranges: 192
entries, no duplicates, order-exact, min `0x0`, max `0x11fc`, 4-byte aligned,
none beyond `CLK_SIZE - 4` (`0x3ffc`). `scan()` re-validates alignment and
bound at runtime and refuses with `-EINVAL` before any mapping if either
fails; it also refuses unless `ARRAY_SIZE(offsets) == 192`.

The log line format is byte-identical to `ane_h17_clk_scan.c`
(`"%s offset=%#x value=%#x\n"`), so existing log parsers keep working:

```
BEFORE_READ offset=0x0 value=0x0
CLK_READ offset=0x0 value=0x0
...
CLK_READ offset=0x11fc value=0x0
COMPLETE
```

## Safety properties carried over unchanged from `ane_h17_clk_scan.c`

- `optin=1` is required; without it `module_init` returns `-EPERM`.
- A caller-supplied **absolute** `log_path` is required (mode `0400` param),
  opened `O_WRONLY | O_APPEND | O_SYNC | O_NOFOLLOW` and rejected unless it is
  a regular file. Writes are `kernel_write` with short-write checking plus
  `vfs_fsync` return checking; any failure aborts the run closed, no retry.
- Machine gate: `of_machine_is_compatible("apple,j700")`, ANE node
  `/soc/ane@400000000` must exist, be **disabled**
  (`of_device_is_available()` false) and `apple,t8140-ane`, and all three DART
  nodes (`/soc/iommu@401800000`, `@401820000`, `@401840000`) must exist,
  be disabled and `apple,t8140-dart`.
- Resource 0 of the ANE node must be `0x400000000` + `0x2000000`; the clock
  window physical address is the ADT-derived `0x3082c8000`, size `0x4000`.
- Power: three `dev_pm_domain_attach_by_id` + `pm_runtime_resume_and_get`
  calls, each requiring PMGR `0xc000/0xc008/0xc010` ACTUAL nibble `(w & 0xf0)
  == 0xf0`, plus PMGR `0x290` the same, **before** any engine-window access.
  Release is `pm_runtime_put_sync` then `dev_pm_domain_detach(.., true)` for
  ids 2,1,0, with `iounmap` before either.
- One shot per load: `attempted` latches on entry, so a reload-free retry
  inside the same module instance is impossible and the module never keeps a
  retry path.

Differences are confined to the offset table, the driver/module name, the
`ARRAY_SIZE(offsets) != 192` guard, and the header comment.

## Expected run shape (not performed)

A run needs a launcher that creates and fsyncs the log file and its parent
first, then `insmod ane_h17_clk_wide.ko optin=1 log_path=/abs/path`. The
existing `ane_h17_clk_scan` launcher can be reused with the module and log
names swapped. Whether to run is a separate decision; this receipt only
records that the artefact is buildable and safe by construction.

## Why it was not executed

Two reasons, in order of weight:

1. **This boot has already had one probe run.** The current boot
   (`522531d1-…`) carried the 24-word `ane_h17_clk_scan` run recorded in
   `receipts/2026-11-04-j700-h17-clock-window-zero/`. A second, larger probe
   in the same boot cannot be read as independent evidence: any residue the
   first run left in the ANE power/task state is inside the measurement. A
   run requires a **fresh cold boot**, a new launcher manifest, and the
   preflight identity capture that this session was not authorised to
   perform (no load, no reboot).
2. **Scope.** The task was kernel-module preparation only: no module load,
   no hardware access, no reboot. Both of those are prerequisites for a
   valid run and neither is available here.

The SHA-256 above identifies the exact artefact a future session must record
alongside its boot UUID, uptime and log path.

## Honest limitation

**Uniform zeros across all 192 words would still not distinguish "window
powered down" from "wrong base address".** Both produce a decodable,
non-faulting, all-zero read — which is exactly what the narrow probe already
saw. The widening only helps in one direction: **any** non-zero word in
either range proves the mapping reaches live registers, and its offset then
identifies real register content. All zeros leaves the ambiguity intact and
should be treated as "abandon this mapping", per the next-steps note in the
clock-window-zero receipt, rather than as evidence that the clock block is
empty. Proving the base independently (locating `initRegMap` for map `0x71`
and `_regMaps[0x71][die].memMap`) remains the only route that settles it.

This module also does not, and cannot, prove anything about device behaviour
of the ANE engine. It reads a register window and logs values.
