# ane.ko runtime-PM autosuspend (T8103, T6000, T6001, T6002)

Commit `ae4f177` on branch `agent/ane-autosuspend` lets `ane.ko` power the
ANE off when it is idle. This receipt records the design, the source audit,
the build proofs, and the protocol that the hardware lanes run before the
merge. No hardware result is in this receipt. `ane_t6021.ko` (M2) does not
change; the M2 needs its own study of firmware sleep and wake.

## What changed

Before, probe took a runtime-PM reference and kept it until remove
(`ane_drv.c` at `160b209`, lines 1164-1177 and 1235-1236). The per-ioctl
get/put could not bring the usage count to zero. The ANE power domains and
the three ANE DARTs stayed on for as long as the driver was bound.

Now:

- Probe sets the autosuspend delay, calls `pm_runtime_use_autosuspend`, and
  releases its reference with `pm_runtime_put_autosuspend` at the end.
- Module parameter `autosuspend_ms` (int, 0444, default 1500). A value of 0
  (or less) sets the delay to -1. With a negative delay the PM core holds a
  usage reference and keeps the device powered, which is the old behavior.
- The open, ioctl, close and `reset` paths take a reference and release it
  with `pm_runtime_mark_last_busy` + `pm_runtime_put_autosuspend`.
- Remove resumes the device first, and calls `pm_runtime_dont_use_autosuspend`
  after `pm_runtime_disable`, so the usage count is balanced for both delay
  signs.

Run-time controls on the ANE platform device
(`DEV=$(readlink -f /sys/class/accel/accel0/device)`):

| Action | Command |
|---|---|
| Hold the ANE powered | `echo on > $DEV/power/control` |
| Let it autosuspend again | `echo auto > $DEV/power/control` |
| Change the delay | `echo 3000 > $DEV/power/autosuspend_delay_ms` |
| Old behavior without a reload | `echo -1 > $DEV/power/autosuspend_delay_ms` |
| Old behavior at load | `modprobe ane autosuspend_ms=0` |
| See the state | `cat $DEV/power/runtime_status` |

## Source audit: every path that touches the engine, a DART, or a mapping

Line numbers are `ane/src/ane_drv.c` at `ae4f177` unless a file is named.
"Lifetime" means the probe reference that `160b209` held until remove.

| Path | Where | PM reference before | PM reference after |
|---|---|---|---|
| First engine MMIO (`ane_tm_enable`, `ane_tm_status`) | resume callback 1309-1353, from probe 1222 | probe `resume_and_get`, kept | same `resume_and_get`, released at 1241-1242 |
| Stale DART mapping purge | 1226 | probe reference | probe reference |
| `ane_dart_init` (ioremap only, no MMIO) | 1227 | not needed | not needed |
| File open | 731-736 | `resume_and_get` + `put` (lifetime kept it on) | `resume_and_get` + `put_autosuspend` |
| BO_INIT map, BO_FREE unmap | ioctl wrapper 812-819 | `resume_and_get` + `put` | `resume_and_get` + `put_autosuspend` |
| SUBMIT: enqueue 568, execute 586, DART mask/latch/drain/unmask (`ane_tm.c` 266-341), wedge recovery (`ane_tm.c` 352) | ioctl wrapper 812-819 | same as above | same as above |
| `reset` write: `ane_tm_recover` | 622-638 | none (lifetime only) | `resume_and_get` + `put_autosuspend` (new) |
| File close: `drm_release` frees BOs (`ane_gem_free_object` 343-362 unmaps), then postclose 755-772 | release wrapper 831-840 | none (lifetime only) | `get_sync` + `put_autosuspend` (new) |
| Final GEM put from munmap after the file closed | `ane_gem_free_object` 343-362 | lifetime | none: apple-dart powers the DART for the TLB flush itself |
| Remove: unmap 1282, reclaim 1283, genpd detach | 1266-1291 | lifetime | `get_sync` at 1266, released at 1291 (new) |
| Runtime suspend callback | 1295-1307 | no MMIO | no MMIO, wedge veto kept |
| Recovery power cycle | `ane_tm.c` 411-467 | lifetime + caller | caller (ioctl or `reset`) |
| Engine IRQ | 1162-1169 | never requested | never requested |
| DART fault IRQ | apple-dart | apple-dart takes its own reference | unchanged |
| CPU boost off work | `ane_boost.c` 76-89 | no MMIO, no PM call | unchanged |
| `ane_stats` sysfs and `ane_timeline` debugfs | `ane_stats_show.c` | RAM only | unchanged |
| System sleep | 1358 | `pm_runtime_force_suspend/resume` | unchanged |

Postclose runs `ane_wedge_clear` and then `ane_tm_recover`. The first call
sets `wedged` to 0, so the second returns at `ane_tm.c` 562-563 and never
power-cycles. The comment above postclose says otherwise. This is older than
this change; the release wrapper now covers postclose in any case.

## What survives a power cycle

- DART registers. apple-dart registers runtime PM ops for every DART
  (`drivers/iommu/apple-dart.c` in the linux-aurora 7.1.12 tree, line 2365).
  Suspend saves TCR and every TTBR of every stream (2305-2328). Resume runs
  `apple_dart_hw_reset` and writes them back (2330-2363). Locked DARTs skip
  both; the T8103 and T6000 ANE DARTs are not locked.
- Order. `apple_dart_probe_device` links the ANE to each DART with
  `DL_FLAG_PM_RUNTIME` (1316-1318). The PM core resumes the suppliers before
  the ANE resume callback and releases them after a good suspend callback
  (`drivers/base/power/runtime.c` 392 and 418). The DARTs are up whenever
  `ane_runtime_resume` or a submit runs.
- TLB flushes. Each unmap flush takes `pm_runtime_resume_and_get` on the
  DART (apple-dart.c 901-919), so an unmap while the ANE is suspended is
  still correct.
- Page tables, the IOMMU domain, the `drm_mm` allocator, BOs and per-file
  state are in DRAM. A power cycle does not touch them.
- Task manager. `ane_runtime_resume` re-arms the TM on every resume
  (1336). T8103 loses the tm/tq file in the cycle and the re-arm restores it.
  T6001 keeps the file in retention (`ane.h` 88-96). After a clean idle
  period the retained file is the clean completion state (the completion
  path clears `TQ_STATUS` and the `TQ_NID1` pending bit, `ane_tm.c` 333-335),
  and the re-arm writes the same values again.
- Earlier evidence, not repeated here: on 2026-09-14 the T6001 bring-up
  driver ran with a 1000 ms autosuspend. SET0 read `ACTUAL=0` while the
  device was suspended and `0xf` after `open`, and the add, select, conv and
  linear programs stayed exact (commit `7418ea2`). The garbage results in the
  `ane_tm.c` 377-383 comment came from cycles of a wedged engine that kept
  the dead task's latched errors (commit `df23ca9`).

## Safety analysis

- Suspend against submit. The ioctl wrapper holds a usage reference from
  before `drm_ioctl` until after it returns. The PM core refuses a suspend
  while the usage count is not zero, and a resume waits for a suspend that
  is in progress. No submit can run on a gated engine.
- Wedge and recovery. A wedge is set only inside `ane_tm_execute`, under the
  ioctl reference, so the device is active at that time. While `wedged` is
  set and `recovering` is not, the suspend callback returns `-EBUSY` and the
  device stays powered. Every recovery caller now holds a reference, so the
  only gating during recovery is the forced cycle in `ane_pd_cycle`.
  `recovering` lets the single-domain forced suspend pass the veto, as
  before.
- After a failed recovery, postclose clears the wedge, and the veto stops.
  Autosuspend can then gate that engine. That is the same partition cycle
  that recovery runs on purpose. On T8103 the cycle clears the TM file. On
  T6001 the latched errors stay, as they did before this change.
- IRQ. `ane.ko` never requests the engine IRQ (1162-1169): completion is
  polled inside the submit. No handler can read a gated engine. The DART
  fault handler belongs to apple-dart and takes its own reference.
- `ane_boost` holds a cpufreq minimum-frequency QoS request only
  (`ane_boost.c` 43-74). It has no runtime-PM call and does not keep the
  ANE powered.
- `ane_stats` counts submits in RAM inside the submit path. Its busy time and
  job counts do not depend on the power state. `ane/include/ane_stats.h`
  does not change.
- The old M1 reset class ("autosuspend invalidates DART TLBs and resets
  the SoC") came from eiln's driver, whose runtime suspend wrote DART TLB
  invalidates itself (`upstream/main:ane/src/ane_drv.c` 609-614). This
  driver's suspend callback does no MMIO, and apple-dart owns every DART
  register write in the cycle.

## Build proofs

W=1, `ane.ko` and `ane_t6021.ko`, base `160b209` against fix `ae4f177`, same
tree and flags. No warning in a fix build is absent from its base build.

| Tree | Module | sha256 | vermagic | srcversion |
|---|---|---|---|---|
| Arch Linux ARM 7.1.13-3-1-ARCH headers | ane.ko (fix) | `af17de56d80e6441e7691ba73e9f64eebfe34b3257e840de5d1a513d67d7ed56` | 7.1.13-3-1-ARCH SMP preempt mod_unload aarch64 | `BB59C10AC8149C4473AE244` |
| same | ane.ko (base) | `48db8c81d94781dbc1984ca4b3aede940911cd11bc15e9340bb6776094a21040` | same | `389B601E4710B9AFCFAE58C` |
| same | ane_t6021.ko (fix) | `279fbc1f8cb27181869763c7c131700bc70519c5b4602c1a048162007f8462b5` | same | `C6BEAF73AB255965AC9AA18` |
| same | ane_t6021.ko (base) | `f44666de4635b845c84ffbcdffe93c40f400f42ebfc4d537c68feb73a8671425` | same | `C6BEAF73AB255965AC9AA18` |
| linux-aurora 7.1.12 (`9f99ebd9c6de`) | ane.ko (fix) | `7a54da9ea9fa1749b9e2dceb78e8a1828df490b945b5b3841f3c1793ca5abfd5` | 7.1.12-ARCH+ SMP preempt mod_unload aarch64 | `BB59C10AC8149C4473AE244` |
| same | ane.ko (base) | `4e722cecc069508ba95b688b096e0792e5c7f9d82e8889e843a1d8a2f3e18d14` | same | `389B601E4710B9AFCFAE58C` |
| same | ane_t6021.ko, base and fix built in one path | `6f99ee099ae4c8fd6019a77ac4fefee4fe41204e8e7aea155b6dce3c5f15da1a` (byte-identical) | same | `C6BEAF73AB255965AC9AA18` |
| linux-aurora 7.1.12 (`b5e8553dfb03`) | ane.ko (fix) | `b3f8238d5241f492ce85ad785c46ed5e5ebfc735eaa239ddb5ef22065d46886a` | 7.1.12-ARCH+ SMP preempt mod_unload aarch64 | `BB59C10AC8149C4473AE244` |

- Warnings: 7.1.13 `ane.ko` has no compiler warning (only the pahole
  version notice). 7.1.12 `ane.ko` has one, `ane_tm.c` 530
  `-Wformat-truncation`, in base and fix. `ane_t6021.ko` has the same five
  unused-variable warnings in base and fix on both trees.
- The two `ane_t6021.ko` hashes on 7.1.13 differ only because the base and
  fix sources were built in different directories; their srcversion is equal,
  and the same-path build on 7.1.12 is byte-identical.
- On `b5e8553dfb03`, `ane_t6021.ko` does not compile, in base and fix alike:
  that tree ships its own `include/uapi/drm/ane_accel.h`, and the kernel
  include path wins over the module's quoted include, so `ANE_M2_MAX_BINDS`
  and `ANE_ABI_M2_MAJOR` are undefined. This is not caused by this change.
- The 7.1.12 builds use `KBUILD_MODPOST_WARN=1` without `Module.symvers`
  (`CONFIG_MODVERSIONS` is off in that config). They prove compilation, not
  loadability. The 7.1.13 builds use the full headers.

## Host tests

`make -C tools check` passes on `ae4f177`. After `make -C tools ane-run`
(a build product that `tests/test_qwen_prog_run.py` runs),
`pytest -q tests tools` gives 51 passed, 1 skipped (the only skip marker
in `tests/` is `test_hwx_ports.py`, which needs staged Qwen inputs that
this host does not have). An earlier full run, at a 1-minute load average
near 60, stopped at collection: `tools/test_ane_smoke.py` got exit 1 from
the fake T6002 smoke run although all 20 hashes were golden. That suite
drives `tools/omarchy-ane-smoke` against a fake machine root and does not
read the driver; alone it passes in 25 s, and it passed in both later full
runs.

The change is PM glue with no pure logic to unit-test on a host: the only
computation is `autosuspend_ms > 0 ? autosuspend_ms : -1`. The hardware
protocol below is the test.

## Recipe for the hardware lanes

Build on the lane, against the running kernel's headers:

```sh
git fetch origin agent/ane-autosuspend
git worktree add /var/tmp/ane-autosuspend FETCH_HEAD
make -C /lib/modules/$(uname -r)/build M=/var/tmp/ane-autosuspend/ane \
     ANE_VERSION=0.4.1.r1.gae4f177 modules
modinfo -F version /var/tmp/ane-autosuspend/ane/ane.ko   # 0.4.1.r1.gae4f177
modinfo -F parm /var/tmp/ane-autosuspend/ane/ane.ko | grep autosuspend_ms
```

Load (replace the packaged module for the window only):

```sh
sudo rmmod ane
sudo insmod /var/tmp/ane-autosuspend/ane/ane.ko                      # autosuspend_ms=1500
sudo insmod /var/tmp/ane-autosuspend/ane/ane.ko autosuspend_ms=0     # hold, old behavior
DEV=$(readlink -f /sys/class/accel/accel0/device)
cat /sys/module/ane/parameters/autosuspend_ms $DEV/power/autosuspend_delay_ms
```

With `autosuspend_ms=0`, `autosuspend_delay_ms` reads -1. Restore the
packaged module with `sudo rmmod ane && sudo modprobe ane`.

Warning for the T6001 lane: on one 2026-10-02 boot, a second bind of any
ane-family module after `rmmod ane` failed at probe with `-EACCES` from
`pm_runtime_resume_and_get`, until reboot. Plan each window so that it needs
the fewest reloads. Arms B and C below switch at run time with no reload.

## Measurement protocol

Quiet box for every power arm: `load1 < 0.5`, PSI `some avg10` 0 for cpu,
memory and io, screen off or at one fixed brightness, no GPU work, AC power,
sleep disabled. Record `uname -r`, the boot ID, `modinfo -F version ane`,
and the sha256 of the loaded `ane.ko` at the start.

1. Idle power, three arms, 5 minutes each, in the order A B C A B C,
   sampled at 1 Hz with `idle-sample.sh` (this directory; `HW` is the
   macsmc hwmon whose `power1_input` is total system power, as in the
   earlier `total_uW` runs):
   - A: `ane` not loaded (`rmmod ane`). On the T6001 lane, run A once, at the
     end of the window, if a reload is not safe.
   - B: `ane` loaded, held: `echo -1 > $DEV/power/autosuspend_delay_ms`
     (same as `autosuspend_ms=0`). Check once per lane that
     `insmod ... autosuspend_ms=0` also gives `-1` and `active`.
   - C: `ane` loaded, `echo 1500 > $DEV/power/autosuspend_delay_ms`, and no
     ANE use for at least 10 s before the arm starts.
   - Each arm: `sudo HW=... ./idle-sample.sh C 300 > C1.tsv`. Report the
     mean and standard deviation of `total_uW` for each 5-minute block.
     Also save `pm_genpd_summary` once per block.
   - Pass: in C, `runtime_status` reads `suspended` and the ANE domains
     (T8103: `ane_set1..5`, `ane_base`, `ane_sys_cpu`, `ane_sys`; T6001:
     `ane_set1..4`, `ane_base`, `ane_set0`, `ane_sys_cpu`, `ane_sys`) read
     off in every sample. In B they read `active` and on. Each C block mean
     is lower than each B block mean. A domain that stays on in C is a
     finding: record which one and its other users from
     `pm_genpd_summary`.
2. Wake-up latency, with the delay at 1500:
   - Open latency, 20 cycles of 5 s idle then one `open` of
     `/dev/accel/accel0` (the resume happens inside `open`), each followed
     by an immediate second `open` as the warm control:

     ```sh
     python3 - <<'EOF'
     import os, statistics as st, time
     cold, warm = [], []
     for _ in range(20):
         time.sleep(5)
         for out in (cold, warm):
             t = time.perf_counter()
             fd = os.open('/dev/accel/accel0', os.O_RDWR)
             out.append((time.perf_counter() - t) * 1e6)
             os.close(fd)
     for n, v in (('cold', cold), ('warm', warm)):
         print(n, 'min_us %.0f median_us %.0f' % (min(v), st.median(v)))
     EOF
     ```

   - First whole-encoder call after 5 s idle against a steady-state call:
     min and median over 20 cycles of (idle 5 s, one call, one more call).
3. Regression, autosuspend 1500 against held (-1): whole-encoder 20 blocks x
   16 calls, each block after the idle gate, min-of-min and
   median-of-medians per arm. Pass: min-of-min within 1% and every output
   digest bit-exact in both arms (the lane's own encoder anchor).
4. `omarchy-ane-smoke` 20/20 with the delay at 1500 and at least 2 s idle
   before each run, so each run starts from a suspended device. All hashes
   equal the receipt `2026-10-02-h13-smoke` value.
5. `ane_stats`: read `$DEV/ane_stats` before and after steps 3 and 4. The
   `jobs` delta must equal the number of submits exactly in both arms.
6. Stress, 10 minutes, delay 1500: loop a random idle of 0 to 4 s (both
   sides of the delay) and one smoke run. Pass: every run exact, `wedged`
   reads 0 at the end, and `dmesg` has no new line that matches
   `SError|external abort|Internal error|tm completion failed|recovering|DART.*fault|Unbalanced pm_runtime`.
7. Close-out: `echo on` and `echo auto` on `power/control` each take effect
   (`runtime_status` changes within delay + 1 s). Then rmmod, and confirm
   that the packaged module binds again (`omarchy-ane-check`).

Save the TSV files, `dmesg`, `pm_genpd_summary` captures, and the command
transcript with SHA256SUMS. Merge only after the T8103 and T6001 lanes pass
steps 1 to 6.

## Not verified here

- Nothing ran on an ANE. Every behavior above, including the domains that go
  off and the wake latency, is a prediction until a lane measures it.
- `ane_sys_cpu` and `ane_sys` power off with the driver bound for the first
  time. The parked ANE coprocessor state is lost in that cycle. The host TM
  path does not use the coprocessor on the M1 family, but no run has checked
  this with this driver.
- DART state that apple-dart does not save (for example a tunable that iBoot
  writes) would be lost in the cycle. No such tunable is known for these
  DARTs.
- The mlx-omarchy worker gate (`runtime_worker.cpp`, eligibility check)
  opens and closes `/dev/accel/accel0`, then requires `runtime_status` to
  read `active`. The close now leaves the device active for the delay, so
  the gate passes at 1500 ms if it reads within that time. With a delay of
  0 written at run time the gate would see `suspended` and refuse. The gate
  should accept `suspended` once this change merges.
- A BO whose last reference is an mmap that outlives its file is unmapped
  without an ANE reference. That is correct (apple-dart powers the DART for
  each flush), but each page can cost a DART power cycle.
