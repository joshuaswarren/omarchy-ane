# T6021: firmware-managed idle (`dyn_pg`), code and gated M2 protocol (2026-10-03)

Status: code and host proof only. No hardware run. Every power, latency and stability
statement about the M2 below is a prediction until the protocol in this file runs.

## What changed

The driver keeps the whole ANE powered while the module is loaded. Probe takes a runtime-PM
reference and never puts it (`ane/t6021/ane_t6021_rtclient_main.c`, `pm_runtime_resume_and_get`
in probe; `held = true` after the CPU release). A genpd power-off would end the firmware instance,
so this change leaves the reference in place and asks the firmware to gate its own compute
islands between jobs. This is design (c) of the offline power study (private notebook,
`AnePmT6021Study/report.md`, sections 5 and 6(c)).

- New load-time parameter `dyn_pg` (bool, 0444, default 0). With `dyn_pg=1`, probe sends
  `CSNE_CMD_SET_DYNAMIC_POWERGATE` (0x2d) once, after CONFIG_GET and the ChMan gate and before the
  DRM device registers: channel 1, length 0x0c, the u32 value 1 at +0x08, timeout 3000 ms, under
  `ane_t6021_fw_lock`, through `ane_rtclient_command` (the path that `fw_perf_mode` uses). There
  is no runtime switch: the firmware's off path powers the islands on again.
- Every registered device with `dyn_pg=1` has sent the command. A probe that does not reach that
  point registers no device at all: the RTKit mode (`legacy_only=0`) never validates the ChMan
  table and refuses, and a failed command-buffer allocation fails CONFIG_GET first.
- If the command fails, `ane_rtclient_command` quarantines the device and probe does not register
  the DRM device. The command runs only after this driver started the firmware (`held` is set:
  the ChMan table exists only on that path), so `err_pm_or_hold` keeps the power, rings and IRQ
  until reboot.
- Probe refuses `dyn_pg=1` with -EINVAL before any power access on a SoC whose firmware is not
  selene 13.5 (today: T8112, firmware bia). The command is decoded only for selene.
- `trace_td` takes no PS or TD sample when `dyn_pg=1` (see the audit, row 7).
- New debugfs file `ane_t6021/ane_pg_state` (0400; T602x, created with `stats=1`, the default).
  Each read prints the seven ANE PS words that `trace_td` checks, through the same
  `ioremap_np(pmu_pa + ps_off)`, one `name value` line each: `ane_sys_mpm`, `ane_td`,
  `ane_base`, `ane_set1` .. `ane_set4` (pmgr 0x28e080000 + 0x4000 .. 0x4030). ACTUAL is bits
  7:4. genpd's `pm_genpd_summary` cannot see firmware writes to these words. The file reads no
  other register: not `ane_cpu` (0x2e0), no CoreSight, no other pmgr word.
- `boot_prevent_nap` (existing, `ane/t6021/ane_t6021_boot.c`, default 1) is unchanged. Arm C of
  the protocol sets it to 0.

With `dyn_pg=0` and `boot_prevent_nap=1` (the defaults), probe sends the same firmware commands
and makes the same register accesses as origin/main `16cfa87`. The only new item is the debugfs
file, which reads the seven PS words when someone opens it and at no other time.

### Firmware facts this relies on (selene 13.5, 22G74)

Summary of the private disassembly listings of the selene image (sha256 `a9c4b771…427bc`, pinned in
`ane/t6021/ane_fw_validate.h`); no listing is published here. The public decode receipt is
omarchy-ane `8cae3ad` (`receipts/2026-10-03-t6021-powerdown/README.md`, branch
`agent/ane-t6021-powerdown-study`).

- The 0x2d handler (0x28178) reads the u8 at command +0x08 and calls
  `CAneEngineExeLoopH14::setDynamicPowerGate` (vtable +0xe0).
- `setDynamicPowerGate(1)` (0x51fb8) stores 1 at +0x678. If its flag +0x1a2 is clear, it calls
  `SwitchDynamicPowerGate(1)`, then `powerDownAne` when the engine is idle. If +0x1a2 is set,
  the switch does not happen. Which value +0x1a2 has on Linux is unknown: arm B's PS words
  answer it.
- With the gate on, `SignalProcessor` and `DataProcessor` call `powerDownAne` after each job when
  the engine is idle, and `powerUpAne` turns td, base and set1-4 on for the next job. `PowerUp`
  asserts that these islands are off before it turns them on. A failed firmware ASSERT spins
  forever. Therefore genpd must never turn on an island that the firmware gated: the driver
  keeps its runtime-PM reference, and this design must never be combined with runtime
  autosuspend of the same device.
- Without the command, `PowerDown` returns at once ("Dymanic PowerGating is disabled"): its flag
  +0x96 has one writer, `SwitchDynamicPowerGate`. The default "off" is INFERENCE from the
  absence of other writers.

## Host-access audit (every register access that can run after probe)

Hard rule: a TM read while the compute islands are off hangs the SoC. "Compute island" here
means ane_td, ane_base and ane_set1-4 (the islands that the firmware gates). Line numbers are in
`ane/t6021/ane_t6021_rtclient_main.c` on this branch.

| # | Site | Block | Compute island | Guard |
|---|---|---|---|---|
| 1 | `:518` writel doorbell (legacy exchange) | IPI, engine +0x1844000 (ASC area, ane_cpu) | no | runtime-PM ref held; fw lock |
| 2 | `:521` readl IPI pending | IPI | no | same |
| 3 | `:524` writel IPI ack | IPI | no | same |
| 4 | `:559` writel IPI bit 5 (MALLOC ring) | IPI | no | same |
| 5 | `:812` writel doorbell (T2H drain) | IPI | no | same |
| 6 | `:841` readl PS words (traced CALL wait) | pmgr page `pmu_pa` | no | runs only with `trace_td=1` and `dyn_pg=0` |
| 7 | `:844` readl TD word (traced CALL wait) | TM, engine +0x1c20458 | **yes** | `dyn_pg=0` (no PS map otherwise, so the loop never reaches it) AND all seven PS words read 0x3ff in the same iteration. With `dyn_pg=0` the firmware does not gate (INFERENCE above), so the check cannot go stale. With `dyn_pg=1` a check-then-read can race a firmware gate, so the read is removed. |
| 8 | `:1970` readl PS words (`ane_pg_state`) | pmgr page `pmu_pa` | no | T602x only; reads on open only |
| 9 | `:1825` `apple_rtkit_poll` | ASC mailbox | no | scheduled only with an RTKit instance (`hello_wait_ms` > 0 or `legacy_only=0`); never in the default legacy mode |
| 10 | `ane_stats_begin/complete` `:922-963`, `ane_stats_emit` `:1991`, `ane_timeline_fops` | host memory | no | no MMIO: `ane/include/ane_stats.h` and `ane/ane_stats_show.c` contain no readl/writel/readq/writeq/ioread/iowrite; tmst is 0 on T6021 (`:963`) |

Probe-only accesses, all before the 0x2d command: `:2081` (ane_cpu PS, G1 gate), `:2094` (PWGATE,
T8112), `:2105-2106` and `:2184` (ASC CPU_STATUS, RVBAR), `:2229` (SCRATCH3 host ack), and the
boot and staging units (`ane_t6021_boot.c` and `ane_t6021_fwload.c` MMIO), which run only from
`ane_t6021_fwload_probe` and `ane_t6021_boot_start` inside probe. `remove()` cancels the poll
work and touches no register. DART TLB maintenance on BO map and unmap goes to dart0 (always-on
pmp) and dart1/dart2 (ane_cpu, held), not to a compute island.

Open risk for arm C (`boot_prevent_nap=0`): Linux sets AUTO_ENABLE on every ANE PS word when genpd
powers it on (omarchy-linux `drivers/pmdomain/apple/pmgr-pwrstate.c`, `apple_pmgr_ps_set`). If the
firmware CPU naps, ane_cpu ACTUAL can fall to 0 between jobs, as macOS shows (`0x1000030f`,
`docs/t6021-ane-bringup-findings.md`, macOS PS table). Rows 1-5 then reach the IPI block while
the ASC is auto-gated. macOS rings the same doorbell in that state, so the auto-enable wakes the
block (INFERENCE). Linux has no evidence. Arm C runs only after arm B passes.

## Gated M2 protocol (hardware lane, NOT RUN)

Owner and window: Main schedules the M2 window with the GPU lead. One owner for the M2 during the
window. Pre-register a private notebook entry before the first change. Never suspend the M2.

Module: the branch build of `ane_t6021.ko` (sha256 recorded before install), on the existing lab
hand-module path `/lib/modules/7.1.13-3-1-ARCH/updates/ane_t6021.ko`, `depmod -a`. Never `rmmod`
or reload `ane_t6021`; each arm is its own boot. A test arm uses a one-shot option, as
`receipts/2026-10-01-t6021-default-on-gate/scripts/oneshot-b.sh` does: the modprobe.d `install`
line deletes its own file and runs `sync` before it loads the module with the option, so a reset
gives a default load (arm A) on the next boot, not a reset loop.

    # arm B (arm C: add boot_prevent_nap=0), on the M2
    printf '%s\n' '# dynpg one-shot (removes itself before the load).' \
      "install ane_t6021 /usr/bin/rm -f /etc/modprobe.d/ane_t6021-dynpg.conf && /usr/bin/sync && /usr/bin/modprobe --ignore-install ane_t6021 dyn_pg=1 \$CMDLINE_OPTS" |
      sudo -n tee /etc/modprobe.d/ane_t6021-dynpg.conf
    sync

### Arms (one boot each)

| Arm | Parameters | Purpose |
|---|---|---|
| A | none (`dyn_pg=0 boot_prevent_nap=1`) | baseline, loaded and held |
| B | `dyn_pg=1` | firmware gates the compute islands; the ASC never naps |
| C | `dyn_pg=1 boot_prevent_nap=0` | B plus firmware CPU nap |

Order A B C A B C, so drift cancels. Run C only after B passed in the same series.

### Per boot

1. State: boot ID, `uname -r`, `/proc/cmdline`, module sha256, `srcversion` and `version`,
   `/sys/module/ane_t6021/parameters/{dyn_pg,boot_prevent_nap,stats,trace_td}`, the
   `dyn_pg:` and `LAB init resource[0x84]` kernel lines (the second one prints only when
   `boot_prevent_nap=1`), `ane_pg_state`, `/sys/kernel/debug/pm_genpd/pm_genpd_summary`,
   `/sys/bus/platform/devices/284000000.ane/power/runtime_status`, `ane_stats`.
2. Wait 120 s after boot.
3. Correctness: the 16 gates and one encoder process, with the method of
   `receipts/2026-10-01-t6021-default-on-gate/scripts/window.sh` (gates bit-exact; encoder fp16
   sha256 `fca96f1355485ec3…`, golden `max_abs=0`).
4. Wait 30 s. Read `ane_pg_state` three times, 1 s apart. Expected (prediction): A shows ACTUAL
   0xf in all seven words. B and C show ACTUAL 0 in ane_td, ane_base and ane_set1-4 while no
   job runs. If B shows 0xf in all seven, the firmware did not switch (its flag +0x1a2, or a
   cause not known): record it, B equals A for power, and C still measures the nap alone.
5. Idle power, 5 min at 1 Hz: every `power*_input` of the macsmc hwmon with its label, as
   coreglass `sampler.hwmon()` reads it (select the device by `name`, not by `hwmonN`); the
   battery `power_now`, `status` and `capacity` under `/sys/class/power_supply/`;
   `ane_pg_state`; load1; `/proc/pressure/{cpu,io,memory}`; fans; temperatures.
   Quiet-box gate per window: load1 < 0.5, PSI some avg10 = 0, fixed screen state, no GPU job,
   AC with a full battery (or battery with `power_now` recorded). Discard a window that fails;
   never edit samples.
6. Wake latency: 20 cycles of `sleep 5` then one PROCEDURE_CALL of the add gate program
   (`ane-run --time --repeat 1`), against 20 back-to-back calls (`--repeat 20`). Report min and
   median exec ms of the first call after idle and of the steady calls. Read the per-call
   `submit_ns`/`end_ns` from `ane_t6021/ane_timeline` as the second view. Do not switch
   `trace_td` on: with `dyn_pg=1` it records no TD word, and its debugfs directory collides with
   the stats directory (see Findings).
7. Encoder min-of-min: 20 processes of 16 calls each (the `enc` step of
   `receipts/2026-10-01-t6021-af-bridge-run/scripts/ab-turn.sh`); minmin = minimum of the
   process minimums, medmed = median of the process medians.
8. `ane_stats` before and after steps 3, 6 and 7: `jobs` grows by exactly the number of calls
   issued; `busy_ns` grows.

Every ANE call runs as `flock /var/tmp/ane-run.lock timeout 120 ...`.

### Pass criteria

- Correctness in every arm: 16/16 gates bit-exact, encoder sha256 equal to the golden digest,
  `ane_stats` jobs equal to calls, 0 `EXCH ... failed`, 0 completion-wait failures, 0
  quarantine, 0 DART faults, 0 SError or external abort lines.
- B passes when B−A idle power < 0 with a bootstrap 95% CI (over the per-window medians) that
  excludes 0, the encoder minmin regression is at most +1% against A, and the first-call
  latency after idle is reported.
- C passes when C−B < 0 with the same CI rule, and the same latency and regression rules hold.

### Stop rules

- No answer within 6 minutes after any step or reboot (`ssh -o ConnectTimeout=8 <M2-SSH-alias>
  'uptime; cat /proc/sys/kernel/random/boot_id'`; ping is not a liveness test) = STOP. Report to
  Main. No retry and no next arm.
- Any quarantine, firmware timeout, completion-wait failure, DART fault or abort ends the arm:
  record it, reboot, report.
- Before any hard reset (`p.reboot()` over the m1n1 USB proxy, or `macvdmtool reboot`): `sync`,
  then wait 40 s.
- Never `rmmod` or reload `ane_t6021`; reboot only.

## Findings outside the change (not fixed here)

- `trace_td` creates debugfs `ane_t6021/` when it is first switched on, and probe creates the
  same directory for `ane_timeline` when `stats=1`. `debugfs_create_dir` returns -EEXIST for the
  second creator. So with the default `stats=1`, switching `trace_td` on after probe gives no
  `trace_td` file, and `trace_td=1` at load leaves no `ane_timeline` (and no `ane_pg_state`).
- On a kernel tree that carries the in-tree `include/uapi/drm/ane_accel.h` (omarchy-linux
  `josh/ane-driver-aurora` f227145f50e4, `CONFIG_DRM_ACCEL_ANE=m`), the quoted include
  `"uapi/drm/ane_accel.h"` resolves through the kernel's `-I include` before the module's
  `-I ane/src`, and the in-tree header lacks `ANE_M2_MAX_BINDS` and `ANE_ABI_M2_MAJOR`. Main
  already handles this for installs: DKMS skips such a kernel (`BUILD_EXCLUSIVE_CONFIG`, #94).
  The aurora build below is therefore a compile check only, with `KCFLAGS=-iquote <src>/ane/src`
  on both the base and the branch builds.

## Host proof (MEASURED, 2026-10-03 UTC)

Base = origin/main `16cfa87` (after #94), branch = `14196ef` (the driver commit). Same tree and
flags for both builds of each tree; `ANE_VERSION` was `dynpg-base` and `dynpg-fix`.

| Tree | Build | rc | W=1 diagnostics | sha256 | srcversion |
|---|---|---|---|---|---|
| M2 3-1 headers `7.1.13-3-1-ARCH`, ALARM chroot, gcc 16.1.1 | base | 0 | 6 | `73854fa9d6aba20730a9373fca3a2418cd2ac80d26bad5aad0e47b7ebeebc8a0` | `421C74FF4D6BC3A3660B61C` |
| same | branch | 0 | 6, none new | `921998e678afc392da273f9467e947222423299f268ac65c987dd99c9eac4ec6` | `DA73BC8411DFADE4E7E0FA1` |
| omarchy-linux `josh/ane-driver-aurora` `f227145f50e4` (`7.1.12-ARCH+`), aarch64-linux-gnu-gcc 12.2.0, `modules_prepare`, `-iquote` | base | 0 | 5 | `509836df6b77b96c20b4c6a976ba81fcc90ef81a0946e2de5ca411c0d8523014` | `421C74FF4D6BC3A3660B61C` |
| same | branch | 0 | 5, none new | `40dd6ea1b8c9cc99acf561f6cc5db0a9ab1e55cc4e7d16d610cd71c94de27365` | `DA73BC8411DFADE4E7E0FA1` |

- vermagic: `7.1.13-3-1-ARCH SMP preempt mod_unload aarch64` (M2 3-1) and
  `7.1.12-ARCH+ SMP preempt mod_unload aarch64` (aurora).
- The diagnostics are the same lines in base and branch (compared with line numbers removed):
  `ane_t6021_load_sec_name` defined but not used (3), `t2h_buf` and `t2h_ioq` set but not used,
  and on the M2 3-1 tree the pahole version note.
- `modinfo -F parm` of the branch module lists `dyn_pg` (bool) and `boot_prevent_nap` (bool).
- Without the `-iquote` flag, base `160b209` fails on the aurora tree with the three UAPI errors
  above (rc 2).
- The same builds before the rebase onto #94 (base `160b209`, branch `585af4b`) were also rc 0
  with no new diagnostic on either tree.
- Host tests on the rebased branch (driver code of `14196ef`): `make -C tools check` rc 0;
  `pytest -q tests tools` after `make -C tools ane-run` (the CI host suite builds it too):
  51 passed, 1 skipped (`tests/test_hwx_ports.py`: staged Qwen inputs not on this host).
