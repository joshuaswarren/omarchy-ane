# agx_stats M2 (T6021) validation protocol: AGX firmware stats export (aurora draft PR, replacement for #157)

Branch `agent/jw16-agx-stats4` (replacement for the original PR head, see
`builds.md`), 8 commits on the same `aurora-wip` base `3bb0a6104a11`. The
series exports the AGX firmware statistics (`StatsMsg`: `Utilization`,
`PowerState`, `PowerOn/Off`, `FwBusy`, `AvgPower`, `Temperature`) as
`/sys/class/drm/card*/device/agx_stats` (`key value` lines, mode 0444), with a
cumulative `busy_ns` integrated from `FwBusy` timestamp deltas, a host-side
completed-submission counter `jobs` (bumped at fence signal in
`JobFence::command_complete`), and an opt-out `stats_export` module
parameter (the kernel command line takes the module-prefixed form; default 1). Producer contract: `coreglass` reads it at
10 Hz as the GPU busy source.

Prepared 2026-10-04 by AgxStatsM2Prep (offline). Executor: Main with the M2 GPU
lane (w6Z). The executor pre-registers its own notebook entry before the first
reboot. Private record: `apple-silicon-lab entries/AgxStatsM2Prep/`.

## Fact base (measured offline)

- The M2's running stock kernel `7.1.13-3-1-ARCH` has the AGX driver built in
  (`CONFIG` key =y; macstudio headers `.config`, the config of every proven ane.ko build). The
  driver is built-in on jw16 `3-2-ARCH` too (Jw16AgxStats entry). A module-only
  swap gate is therefore INFEASIBLE: a built-in driver cannot be unloaded and a
  second same-name module cannot bind the claimed device.
- The distro headers tree has no `rust/` directory, so no OOT Rust module build
  against `7.1.13-3-1-ARCH` headers exists either (macstudio ls, 2026-10-04).
- The M2 window is a ONE-SHOT KERNEL BOOT of an aurora build, stock GRUB entry
  untouched as the fallback. This is the proven pattern from the in-tree #155
  M2 boot and the H253 boot.bin arms (about 95 s boot, stock return works).
- aurora-wip is a 7.1.12 tree; the M2 runs a 7.1.13 stock kernel. The test
  kernel gets its own release string `7.1.12-ARCH-agxstats`, its own
  `/lib/modules` directory and its own initramfs, so the stock install is never
  touched.
- GPU is bound and busy on the M2 stock kernel (H253: qwen38-2B decode
  109.59 tok/s d64, GPU OPP table live). The agx series must not change that.
- `jobs` is counted on the host: `JobFence::command_complete` bumps the
  snapshot counter when the last command of a submission completes. The exact
  "jobs matches submissions ±1" contract check is NOT computable from
  userspace (the number of internal driver submissions per cell is
  Mesa/driver-defined), so the M2 gate checks: `jobs` stable while idle,
  strictly increasing through the decode cells, and `jobs/s` from the
  coreglass phases output in a plausible range (H253-class decode: tens of
  submissions/s).
- The original series' doc/code mismatches (unprinted `temperature_tmin/tmax`
  keys; a saturation claim versus wrap semantics) are fixed in this series:
  the ABI document now names exactly the printed keys and states the wrap.

## What ships to the window (build proof products)

From `builds.md` and the private artifacts
`apple-silicon-lab artifacts/AgxStatsM2Prep/20261005T0933Z-busy-ticks/`
(the round-6 tick-conversion product, Image sha `c7f4dabe...`):

| file | use |
|---|---|
| `Image-m2` | the test kernel (AGX driver built in, like stock) |
| `System.map` | offline symbol verification (driver probe and agx_stats sysfs symbols) |
| `kernel.config` | the exact .config the Image was built from |
| `modules-m2.tar.zst` | `/lib/modules/<release>` (no AGX module in it — the driver is built-in) |
| `RELEASE` (contains `7.1.12-ARCH-agxstats+`) | exact release string, read by the scripts |
| `SHA256SUMS-stage` | copy to `SHA256SUMS` in the device stage dir (verified pre-install) |
| `verify-boot-product.txt` | offline verification receipt; install REFUSES without it (see below) |

HARD GATE (added after window B boot 1 shipped a kernel without a working
driver; EXTENDED after the retry found the repr(Rust) readout skew):
`receipts/2026-10-04-agx-stats-m2/scripts/verify-boot-product.sh <stage-dir> <kernel-tree>` runs on
the CT before the window and proves, offline, that the Image carries the
built-in AGX driver (its probe banner / `MMU:` strings), the agx_stats
symbols, matching vermagic across the tarball, and the checksums. It writes
`verify-boot-product.txt` (`AGX_VERIFY_OK` + Image sha). The device-side
`install-test-kernel.sh` REFUSES to install unless that receipt is staged and
the staged Image's sha256 matches it bit-for-bit.

## Window plan (about 100-120 min wall, 3 reboots)

| step | what | time |
|---|---|---|
| 0 | stage files, preflight (read-only) | 10 min |
| 1 | install test kernel + one-shot boot (12-min notice) | 8 min |
| 2 | T1 block: export on — identity, S1 idle, S2 matmul, S3 coreglass, S4 cells E1 (2 with 10 Hz reader), S5 dmesg | 40 min |
| 3 | one-shot boot with the module-prefixed off parameter appended (12-min notice) | 8 min |
| 4 | T0 block: off-arm identity, unsupported file, S4 cells E0, S5 dmesg | 20 min |
| 5 | reboot to stock (12-min notice), smoke, remove test kernel, records | 15 min |

Reboot rules (standing): 12-minute notice before every reboot; if no ssh
return within 6 minutes, stop and hand to Main (hard reset via jwm1 tuxvdmtool
is Main-only). Every boot ends on the stock default unless step 5 says
otherwise. Never rmmod anything; `ane_t6021` is never touched.

## Step 0 — preflight (read-only, on the M2)

    ssh jw14m2-linux 'bash -s' < receipts/2026-10-04-agx-stats-m2/scripts/preflight.sh

The script saves a baseline to `/var/tmp/agx-window/baseline/` (dmesg -x,
journalctl -k -p warning, uname, cmdline, boot id, GPU card + of_node,
the driver's module parameters directory listing, `findmnt /boot`, `grub-editenv list`,
load1, PSI) and prints the GO/NO-GO lines:

- the AGX driver config is `=y` in `/proc/config.gz` (exact key in
  preflight.sh) — expected (built-in).
- A DRM card whose `device/of_node` ends in `gpu@406400000` — the AGX device.
- `findmnt /boot`: if FSTYPE is `vfat` (ESP), the install step in step 1 is an
  ESP write and needs Main's explicit GO recorded at `/var/tmp/agx-window/ESP_GO`
  before `install-test-kernel.sh` will proceed. If `/boot` is on the rootfs
  (ext4), no GO is needed.
- `grub-editenv list` must work and `/etc/default/grub` must have
  `GRUB_DEFAULT=saved` for the one-shot pattern. If not, stop and ask Main —
  do not edit the default boot order.
- Quiet gate at each timed step: uptime >= 360 s, load1 < 0.5, cpu PSI
  some avg10 = 0.00 (same convention as the ane gates and H253).

## Step 1 — install and one-shot boot (test kernel, export on)

Copy the staged files to the M2 (`/var/tmp/agx-window/stage/`: `Image-m2`,
`modules-m2.tar.zst`, `RELEASE`, and `SHA256SUMS-stage` renamed to
`SHA256SUMS`, plus this receipt's `scripts/` directory as
`stage/scripts/`), then:

    ssh jw14m2-linux 'sudo bash -s' < receipts/2026-10-04-agx-stats-m2/scripts/install-test-kernel.sh

The script: verifies sha256 of the staged Image and modules tarball, installs
`/boot/vmlinuz-7.1.12-ARCH-agxstats`, runs `mkinitcpio -k 7.1.12-ARCH-agxstats`
with the stock `/etc/mkinitcpio.conf`, extracts modules, `depmod -a`, appends
an idempotent `40_custom` block with one entry named `AgxStats M2 validation`,
runs `grub-mkconfig`, then `grub-reboot "AgxStats M2 validation"` and prints
`grub-editenv list`. Announce the 12-minute reboot notice, then `sudo reboot`.

Post-boot identity (all recorded):

    uname -r                      # 7.1.12-ARCH-agxstats
    cat /proc/cmdline             # records whether stats_export was set
    cat /proc/sys/kernel/random/boot_id
    ls -l /sys/class/drm/card*/device/agx_stats

PASS: the test kernel is up, ssh returned within 6 minutes, the AGX card
exists, `agx_stats` exists. FAIL (any): reboot to stock, record, stop.

## Step 2 — T1 block (export on)

### S1 — file shape, permissions, idle counters

    F=$(ls /sys/class/drm/card*/device/agx_stats)
    stat -c '%a' "$F"                         # PASS: 444
    sudo -u nobody cat "$F" >/dev/null        # PASS: readable without root
    bash /var/tmp/agx-window/stage/scripts/capture-stats.sh   # sample 1
    sleep 60                                  # quiet idle
    bash /var/tmp/agx-window/stage/scripts/capture-stats.sh   # sample 2

PASS (all): key set is exactly `busy_ns jobs pstate power_mw util1 util2
util3 util4 temperature_raw temperature_scale` (one `key value` line each,
ASCII integers); `jobs` unchanged across the idle window (no submissions);
`busy_ns` monotonic non-decreasing; `busy_ns(s2)-busy_ns(s1) <= 60 s in ns`
with idle fraction <= 0.05. A `busy_ns` fraction > 1 or stuck at 0 under load
in S2 refutes the T6021 nanoseconds-unit hypothesis — record it, change
nothing in-window.

### S2 — matmul saturation

Run the coreglass built-in probe matmul step (or a pinned MLX 4096x4096 fp16
loop, 30 s) under the GPU lock, captures before/after:

    bash /var/tmp/agx-window/stage/scripts/capture-stats.sh && <matmul 30 s> && bash /var/tmp/agx-window/stage/scripts/capture-stats.sh

PASS: busy fraction (delta busy_ns / delta wall) >= 0.9; `util1..util4`
and `power_mw` rise >= 10x over the S1 idle sample (observation — the raw
utilization semantics on T6021 are exactly what this window validates; only
`busy_ns` is a pass/fail gate).

### S3 — coreglass reads it as GPU busy

From the CT (same hosts file as the 20261003T2250Z coreglass M2 probe):

    coreglass hosts && coreglass run <m2-host> && coreglass phases captures/<file>.jsonl

PASS: `coreglass hosts` lists `agx_stats` under the host's `stats`;
`gpu_busy` >= 0.9 in the matmul phase and <= 0.05 in the idle phase.

### S4 — decode cells, E1 arm (export on)

    ssh jw14m2-linux 'CELL_CMD="<lane d64 cell>" bash -s' < receipts/2026-10-04-agx-stats-m2/scripts/decode-ab.sh E1 5 2,4

`CELL_CMD` is the lane's existing d64 decode cell (qwen38-2B, uclampset 1024,
gpu-turn ticket, digest printed) — w6Z supplies it; H253 is the shape
reference (stock d64 109.59 tok/s, digest `eee1cf96..`). The script runs 5
reps, adds a 10 Hz `agx_stats` reader loop during reps 2 and 4, and writes
`<tok_s> <digest> <reader>` lines.

PASS: all 5 digests identical; median(E1 reps 1,3,5) vs median(E1 reps 2,4)
within 0.5%; `jobs` strictly increased across the cells (capture before/after
with capture-stats.sh).

### S5 — dmesg clean

    dmesg -x > /var/tmp/agx-window/t1-dmesg.txt

PASS: zero lines at emerg/alert/crit/err on the test boot; new warning lines
vs the step-0 baseline are recorded and named (the driver probe lines are
info-level and expected). No ANE line changes: `ane_t6021` behavior on this
kernel is out of scope but any ANE error line is a FAIL.

## Step 3-4 — T0 block (export off)

Announce 12 minutes, then arm the off command line: the install script's
`OFF=1` option appends the module-prefixed off parameter (exact form in the
script's `OFFARG`) to the 40_custom
entry, reboot, repeat identity checks. The `stats_export` parameter has no
sysfs file (the driver's parameters all omit `permissions`), so the command line is the
only switch — this boot is why the window has a second reboot.

PASS additions: `cat agx_stats` prints exactly `unsupported`; S4 cells E0
(5 reps, no reader) all digests identical to E1.

    ssh jw14m2-linux 'sudo env OFF=1 bash -s' \
      < receipts/2026-10-04-agx-stats-m2/scripts/install-test-kernel.sh   # idempotent; rewrites the block with the off cmdline + grub-reboot

## Overhead verdict (the 0.5% gate)

    PASS if |median(E0) - median(E1 no-reader)| / median(E0) <= 0.005
    PASS if |median(E1 reader) - median(E1 no-reader)| / median(E1 no-reader) <= 0.005

Both must hold with identical digests. Soft reference (not a gate, different
kernel base): E medians within about 1% of the H253 stock d64 values.

## Step 5 — restore

Announce 12 minutes, `grub-editenv unset default` (or `grub-reboot 0`), reboot:
the stock `7.1.13-3-1-ARCH` default returns. Then:

    ssh jw14m2-linux 'sudo bash -s' < receipts/2026-10-04-agx-stats-m2/scripts/remove-test-kernel.sh
    ssh jw14m2-linux 'bash -s' < receipts/2026-10-04-agx-stats-m2/scripts/preflight.sh   # re-run: stock identity + GPU bound

PASS: stock kernel back, GPU card bound, one 1-cell decode smoke inside the
session noise, `/lib/modules/7.1.12-ARCH-agxstats` gone, `40_custom` block
gone, `grub.cfg` regenerated, boot.bin untouched (`62ba3010` on the ESP if the
executor reads it read-only).

## Rollback

- Test kernel fails to boot or hangs: wait 6 minutes, stop, Main hard-resets
  via jwm1 tuxvdmtool; the one-shot is consumed and the stock default boots.
- Any mid-window failure: `grub-editenv unset default; sudo reboot` — stock
  returns; then `remove-test-kernel.sh`.
- The install script is idempotent and every write is `set -euo pipefail` with
  sha256 verification; the stock kernel, initramfs, modules and the lab
  boot.bin (`62ba3010`) are never modified. ESP writes only behind Main's GO.
- Never rmmod anything; the ANE module state is untouched throughout.

## Risks

1. Kernel base 7.1.12 (aurora) under the M2's 7.1.13-era userland and the
   stock boot.bin DT: the one-shot pattern bounds the blast radius, but a
   Vulkan bind failure makes S4 impossible — that is an abort-with-rollback,
   not a tuned workaround. Fallback (Main decision): rebase the commits on
   the 7.1.13-based branch and rebuild.
2. `util1..util4` semantics on T6021 are unvalidated (the raw power-state
   counters were never read on this chip). S1/S2 exist to observe exactly
   this; a surprise is a finding, not a failure of the window. `busy_ns` no
   longer depends on the unit assumption: the round-6 build integrates
   utilization over 24 MHz tick deltas, exact in 64 bits (see `builds.md`).
3. `jobs` counts internal submissions at `JobFence::command_complete`, so
   its rate tracks Mesa's internal submission count, not user cells; the
   window gates only on stable-while-idle and strictly-increasing (S1/S4).
4. ABI doc mismatches (unprinted temperature keys, the old saturation claim)
   are fixed in the current series; if the contract test's doc-only NOTE
   reappears against the staged tree, stop and re-check the branch head.
5. GRUB one-shot prerequisites (saved default) — preflight checks; if absent,
   stop before touching boot config.
6. ESP-resident `/boot` — install gated on Main's GO file.
