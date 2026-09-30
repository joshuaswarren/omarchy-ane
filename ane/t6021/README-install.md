# ane_t6021 — installed module (T6021 / M2 Max)

This directory ships the installed-path `ane_t6021.ko` for the
Apple Neural Engine on the T6021 (M2 Max). It replaces the
`ane_t6021_rtclient.ko` scratch module that boot 3ab812a3 used to
verify the add path (`y == a + b` on the 512 valid lanes of the add
surface; padding lanes are zero).

## Prerequisites

- Kernel: stock linux-asahi `7.1.13-3-1-ARCH` (proven 2026-09-30 with the
  packaged overlay, receipts/2026-09-30-t6021-stock-mailbox) or
  `7.1.13-ARCH-polltx`. The module builds against
  `/lib/modules/$(uname -r)/build`.
- Device tree: `packaging/dt/t6021-ane.dts` adds the `apple,t6021-ane`
  node at `ane@284000000`, the three ANE DARTs, the ANE mailbox
  (`recv-not-empty` 884, `send-empty` 1833) and the alias IOVA
  reservation. `omarchy-ane-dt apply` puts it in m1n1's device trees; by
  hand, `dtc -@` and `fdtoverlay` from dtc 1.7.1 or newer (older
  `fdtoverlay` renumbers the AIC phandle).
- Firmware: place the pinned selene payload at
  `/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho`
  (sha256 `a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`).
- Module build prerequisites: `make`, a working kernel headers tree,
  `sparse` if you want the warnings the upstream expects.

## Build

```
make KERNELDIR=/lib/modules/$(uname -r)/build \
     ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     -C ane/t6021 modules
```

Out-of-tree build (the default), no header install, no automatic
modprobe. The Makefile keeps `obj-m := ane_t6021.o` (single module)
and links `ane_t6021_rtclient_main.o + ane_t6021_fwload.o +
ane_t6021_boot.o` against the shared boot/fwload contract units.

## Install

```
sudo make -C ane/t6021 modules_install
sudo depmod -a
```

The module is named `ane_t6021`, compatible `of:N*T*Capple,t6021-aneC*`
(see `modinfo`). It carries `suppress_bind_attrs = true` and refuses
`/sys/module/.../uevent` writes, so the device binds via
`modalias` only and cannot be unbound manually.

`depmod -a` registers the alias so the kernel autoloads the module
when the DT node appears. Once loaded the module is permanently
pinned — see "Lifecycle" below.

## Autoload behaviour

`/lib/modules/$(uname -r)/modules.dep` carries
`ane_t6021.ko`. After `depmod -a`, a `uevent` from the platform bus
(matching `apple,t6021-ane`) triggers `request_module("ane_t6021")`
and the kernel loads `ane_t6021.ko`. A bare insmod is also valid and
exercises the same probe path; the compiled-in defaults are the
proven add-path parameter list.

## Lifecycle (wedged-pin rule)

The module is a research driver for a CPU we release into the
firmware. Once the boot sequence (W15 boot contract) writes
`CPU_CONTROL <- 0x10`, the ASC CPU may be fetching from the staged
surface; the firmware owns the IO rings and the boot heap, and
genpd links hold the power domains up.

Therefore `remove()` is gated: `suppress_bind_attrs = true`
blocks manual unbind, and the boot path takes a permanent module
reference before the first write. The only legal reclamation is
reboot. No `rmmod` will succeed — and `rmmod -f` is forbidden
because the boot surface, the `IPC '` ring, the mailbox state,
and the genpd links are all preserved exactly as the firmware
expects to find them.

## BO lifetime

The firmware never frees a program or a process, and it reads
section bytes and io buffers by IOVA at any time after the doorbell
rings. The driver marks a BO `fw_ref` when the firmware receives its
IOVA — under the fw lock, before the exchange goes out:

- section BOs of a `LOAD_PROGRAM` that is sent to the firmware
  (cache misses only: a cache hit reuses the firmware program and
  never touches the caller's BOs);
- the io BOs of every `PROCEDURE_CALL`, unless `free_io_bos=1`.

A `fw_ref` BO is held, with its bytes counted against
`ANE_T6021_BO_TOTAL_MAX` (2 GiB), until reboot. Every other BO frees
when its last reference drops. Each BO holds two reference kinds:
its handle (dropped by `BO_FREE` or fd close) and each live user
mapping (`mmap`; fork takes one more, unmap drops one). A BO freed
while mapped stays allocated until the mapping is torn down.

The 2 GiB cap therefore bounds the memory the firmware may still be
reading, not every allocation a short-lived context makes.

## Module parameters (compiled defaults = proven add-path config)

The defaults reproduce the `load-run.sh` parameter list exactly so a
bare `insmod ane_t6021.ko` is the proven configuration. Parameters
remain overridable from sysfs for bisection only.

| Parameter | Default | Lives in | Purpose |
|---|---|---|---|
| `fw_load` | `1` | fwload.c | Validate + DART-map the selene PRELOAD payload |
| `fw_extra_ram` | `0x200000` (2 MiB) | fwload.c | Page-aligned owned RAM after the 5 MiB firmware surface |
| `fw_alias_reserved` | `1` | fwload.c | Map the iBoot-reserved SEG0/SEGi phys at the latched RVBAR entry |
| `fw_start` | `1` | rtclient | Fenced Linux-context firmware start (boot contract) |
| `fw_start_table_mode` | `2` (skip) | rtclient | Pre-CPU engine table block: 0 abort, 1 write, 2 skip |
| `fw_start_rtb_mode` | `0` | rtclient | RTBuddy/RTKit-app-endpoint select; off = legacy ChMan transport |
| `boot_prevent_nap` | `1` | boot.c | Retain firmware nap-prevention counter via init resource bit |
| `scratch3_ack` | `1` | rtclient | After DONE, write SCRATCH3 = 0x08042006 |
| `legacy_only` | `1` | rtclient | Use the pinned 13.5 legacy ChMan transport |
| `legacy_query` | `1` | rtclient | Service bounded startup allocations and CONFIG_GET |
| `hello_wait_ms` | `1000` | rtclient | Upper bound for the RTKit HELLO wait (lab proven value) |
| `poll_rx` | `1` | rtclient | Drive RX by `apple_rtkit_poll` from the workqueue |
| `start_app_eps` | `1` | rtclient | STARTEP fw-announced app endpoints after the handshake |
| `free_io_bos` | `0` | rtclient | Free PROCEDURE_CALL io BOs at BO_FREE even though the fw saw their IOVAs (soak-test knob) |

Lab knobs that stayed at their inert values in every proven run are
deleted outright, not kept at 0: `fw_diag_marker`, `fw_load_stamp_base`,
`fw_diag_retention` (execution-marker/bisect diagnostics; at 0 they
were unreachable no-ops), plus the `fw_start_dapf`, `fw_start_venc_gates`,
`fw_start_mpm_off`, `fw_start_state_report`, `fw_start_dart_single_stream`,
`fw_start_mbox_ctrl_bit19`, `fw_start_core1_run`,
`fw_start_wrapper_b80_unmask`, `patch_timer_freq`, `fw_start_stop_after`,
`csne_ping`, `legacy_load`, `legacy_seq`, `legacy_resource`,
`legacy_silent` gates (all 0/off in the proven run — the behaviours they
gated are simply absent) and `legacy_notify_ack`/`legacy_fast_poll`
(both 1: the port always acks pending IPIs and polls at 50 us).
`fw_start_skip_genpd` (lab default 0) is likewise absent: the genpd
attach + `pm_runtime_resume_and_get` always run, which IS the lab
default path.

## DRM device

A bare `modinfo ane_t6021.ko` reports
`alias: of:N*T*Capple,t6021-ane`. The DRM device, when bound,
exposes the ABI 2 ioctl set per `ane/src/uapi/drm/ane_accel.h`:

- `DRM_IOCTL_ANE_BO_INIT` — allocate a coherent BO and return its
  handle.
- `DRM_IOCTL_ANE_BO_FREE` — drop the handle. The memory frees when
  its last reference drops, except a BO the firmware received: those
  are held until reboot (see "BO lifetime" below).
- `DRM_IOCTL_ANE_SUBMIT` — rejected with `-ENOTTY` on T6021.
- `DRM_IOCTL_ANE_PROG_LOAD` (0x200) — build the LOAD_PROGRAM message
  (1..9 section records; each record carries the caller's
  `drm_ane_section.id` identity; unsupplied slots stay zero) and
  send it; the firmware-assigned ProgramId comes back in
  `prog_id_out`.
- `DRM_IOCTL_ANE_PROC_CREATE` (0x202) — CREATE_PROCESS for a
  ProgramId; the assigned ProcessId comes back in `proc_id_out`.
- `DRM_IOCTL_ANE_EXEC` (0x204) — PROCEDURE_CALL with priority 2..7
  and 1..64 buffer records at +0x60 + i*0x30; waits for the fw ack
  (slot reply), polls the eight TQ status words idle (0x81) gated on
  the pmgr PS words 0x28e084000..0x30 == 0x3ff, drains the fw's
  target-to-host slots, and only then returns — output buffers are
  coherent, so CPU visibility needs no explicit sync.

The driver reports `major = ANE_ABI_M2_MAJOR = 2`. libane
`is_ane_device()` (in `libane/ane.c`) checks `major == 1` today; a
M2 backend requires `major == 2`, which `ane_m2.c` will provide
separately.
