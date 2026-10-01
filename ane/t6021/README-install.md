# ane_t6021 — installed module (T6021 / M2 Max)

This directory ships the installed-path `ane_t6021.ko` for the
Apple Neural Engine on the T6021 (M2 Max). This is a research
driver. The package builds it with DKMS and turns it on by default:
the T6021 overlay applies, the firmware hook fetches the firmware, and
the module autoloads at the next boot (top-level README, "M2 Max
(T6021)"). Once the firmware starts, the only reclamation is reboot.

Boot path status: the module works on the stock linux-asahi kernel
`7.1.13-3-1-ARCH` (three boots; the third used the complete overlay
`packaging/dt/t6021-ane.dts`, the first two a lab overlay with the
same ANE nodes; receipt
[2026-09-30-t6021-stock-mailbox](../../receipts/2026-09-30-t6021-stock-mailbox/README.md))
and on `7.1.13-ARCH-polltx`. Up to 2026-09-30 every boot, on both kernels,
was a USB proxy chain load (kernel, DTB and initramfs staged over the m1n1
proxy by the M1 host), with the module and the DTB installed by hand. On
2026-10-01 one M2 Max booted from the internal disk (m1n1, U-Boot, GRUB)
with `omarchy-ane-dt apply` and `update-m1n1`, and `ane_t6021` loaded at
boot (receipt
[2026-10-01-t6021-disk-boot](../../receipts/2026-10-01-t6021-disk-boot/README.md)).
That boot used a lab m1n1 stage 2 that adds the two `ane-firmware`
reserved-memory nodes; the packaged m1n1 1.6.1 does not add them. The
default `fw_alias_reserved=1` maps those physical windows, so probe refuses
it, before any power access, unless no-map `/reserved-memory` nodes cover
both windows: with the packaged m1n1 the module stays unbound and the ANE
stays off. `fw_alias_reserved=0` needs no reservation: the driver runs its
own copy of the firmware with iBoot's runtime patches replayed (receipt
[2026-10-01-t602x-independent](../../receipts/2026-10-01-t602x-independent/README.md));
no T6021 has run it yet. It becomes the default after its T6021 device test.

On that laptop the disk boot also needs the opt-in overlay
`packaging/dt/t6021-uboot-serial-stdin.dts` (opt-in key
`uboot-serial-stdin-t6021`): without it, input from the U-Boot internal
keyboard driver (`mtpkbd`) stops the boot at the U-Boot prompt. With it, the
internal keyboard does not work at the U-Boot prompt or in the GRUB menu;
only serial gives input there. This input may be specific to that laptop.
The overlay goes away when uboot-asahi passes only keyboard reports from
`mtpkbd` to stdin.

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
- Firmware: the pinned selene payload at
  `/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho`
  (sha256 `a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`).
  The package's `90-omarchy-ane-firmware.hook` installs it with
  `omarchy-ane-firmware-fetch`; by hand, `sudo omarchy-ane-firmware-fetch`.
- Memory: with the default `fw_alias_reserved=1`, no-map
  `/reserved-memory` nodes that cover `0x10000848000`+`0xc4000` and
  `0x10001400000`+`0x438000` (the lab m1n1 adds them).
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
proven parameter list.

## Proven on hardware

All results are on T6021 hardware through the autoloaded module and
libane ABI 2, on `7.1.13-ARCH-polltx` unless noted. On the stock
kernel, each of three boots passed 20 of 20 gate loads (`add`, `mul`,
2048x2048 matvec and 17 more `add` runs; receipt
2026-09-30-t6021-stock-mailbox). Receipts:
[2026-09-29-t6021-installed-path](../../receipts/2026-09-29-t6021-installed-path/README.md)
and the 2026-09-30 receipts named below.

Elementwise and clip ops on [1,512,1,1] (512 valid fp16 lanes,
padding zero), exact in every trial: `add`, `mul`, `relu`,
add/mul/real-div scalar (0.5), clip low and clip high. Matvec
[1,256] x [256,256]: 256 of 256 lanes within 2 ulp.

Qwen-size matvec, dense random inputs, fp16 weights, device error
against the exact fp64 product in condition units (the fp16 output
rounding bound is 1.0): 1536x1536 (M=1) 0.217, 2048x2048 (M=8)
0.218, 2048x5120 (M=1, 20 MiB weights) 0.187. All trials PASS.

Parakeet attention islands. On random U(-1, 1) inputs, the three
batched-matmul islands (island-c-pv, island-a-kt, island-a-attn-p1)
pass on 3 of 3 seeds: 99.77 to 99.80 percent of lanes are within
max(3 ulp, 4 cond-units) of the fp64 reference
([2026-09-30-t6021-island-bmm](../../receipts/2026-09-30-t6021-island-bmm/README.md)).
On real-model operands (layers 0, 11 and 23), island-a-kt and
island-a-attn-p1 stay within 1.0 cond-unit of the exact fp64 product
on every lane (worst ratio 0.91). island-c-pv fails that strict bound
on 1.3, 47.6 and 52.4 percent of lanes (worst ratio 14.5, 104.1 and
65.3), and its accumulator model is not identified
([2026-09-30-t6021-island-golden](../../receipts/2026-09-30-t6021-island-golden/README.md),
[2026-09-30-t6021-accumulator](../../receipts/2026-09-30-t6021-accumulator/README.md)).
The two select islands are bit-exact on all 1,125,000 valid lanes on
3 seeds. In the loop-closed encoder test with all four island
families on the ANE
([2026-09-30-t6021-parakeet-encoder-islands](../../receipts/2026-09-30-t6021-parakeet-encoder-islands/README.md)):
120 device submissions, zero failures, and the decoded transcript is
byte-identical to the golden transcript from the pinned macOS ANE
capture.

Lifecycle: 160 of 160 gate trials across four parallel `ane-run`
workers; 150 loads of the 2048x5120 matvec (20 MiB weights, about
3 GiB of BO traffic) with no `BO_INIT` failure and no IOMMU fault
([2026-09-29-t6021-installed-path](../../receipts/2026-09-29-t6021-installed-path/README.md),
"BO lifetime").

Not proven:

- The full Parakeet encoder on the ANE. Only the four island sites
  run on the device; the convolutions, LayerNorms, feed-forward
  blocks, softmaxes and the decoder run on the CPU.
- Qwen on the M2. Program 20 of the 38 staged programs (20 tasks,
  84 MB of constants) matches the M1 golden on the device with the
  named-port binding: relative L2 0.00117, 50 of 50 runs identical
  ([2026-09-30-t6021-call-wait](../../receipts/2026-09-30-t6021-call-wait/README.md)).
  The other 37 programs have not run: their port tables give some
  surfaces a channel smaller than the surface, and the loader takes
  one output only.
- The rms_norm chain. The device writes only channels 64 to 2047 of
  2048, so the program does not qualify for a model run (receipt
  [2026-09-30-t6021-island-select-rms](../../receipts/2026-09-30-t6021-island-select-rms/README.md)).
- An explanation for the intermittent all-zero output seen on three
  boots (about 1 call in 5 on those boots). The 1 ms post-call
  settle is the mitigation. On the finish-event wait, 5000 single add
  processes gave 0 all-zero outputs; the failing boots have not recurred.
- A bit-exact model of the C pv accumulator. It is fp32-class, but
  no tested model reproduces it; the strict per-lane bound fails on
  1.3 to 52.4 percent of lanes per layer and the deviation does not
  propagate to the transcript
  ([2026-09-30-t6021-accumulator](../../receipts/2026-09-30-t6021-accumulator/README.md)).

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
- the io BOs of every `PROCEDURE_CALL`.

A `fw_ref` BO never goes back to the kernel, and its bytes stay counted
against the `bo_total_max_mb` cap (12 GiB by default):

- a section BO of a sent `LOAD_PROGRAM` is held until reboot, because the
  cached firmware program keeps reading it;
- an io BO is parked in a pool when its last reference drops. Its IOVA
  stays mapped. The next `BO_INIT` of the same page-aligned size takes it,
  zeroes it and gives it to the new owner. After a quarantine, io BOs are
  held instead, because the firmware may still write them.

Every other BO frees when its last reference drops. Each BO holds two
reference kinds: its handle (dropped by `BO_FREE` or fd close) and each
live user mapping (`mmap`; fork takes one more, unmap drops one). A BO
freed while mapped stays allocated until the mapping is torn down.

The held memory is therefore the loaded program sections plus the peak
number of io BOs in use at the same time, not the sum over every
process. Before the pool, every process held its io BOs until reboot, and
a boot ran out of the old 2 GiB cap after about 14,500 add processes.
The 38 Qwen programs hold about 2.6 GiB of sections, so the cap is now a
parameter with a 12 GiB default. Every BO also needs IOVA below 4 GiB
(the module sets a 32-bit DMA mask), so `BO_INIT` fails with `ENOMEM`
near 4 GiB of BOs whatever the cap. `bo_total_bytes` shows the bytes
counted now; the probe line `loaded ane_t6021 ...` names the cap.

## Module parameters (compiled defaults = proven configuration)

The defaults reproduce the `load-run.sh` parameter list, except
`hello_wait_ms` (1000 there, 0 here), so a bare `insmod ane_t6021.ko` is
the proven configuration. Parameters remain overridable from sysfs for
bisection only.

| Parameter | Default | Lives in | Purpose |
|---|---|---|---|
| `fw_load` | `1` | fwload.c | Validate + DART-map the selene PRELOAD payload |
| `fw_extra_ram` | `0x200000` (2 MiB) | fwload.c | Page-aligned owned RAM after the 5 MiB firmware surface |
| `fw_alias_reserved` | `1` | fwload.c | T6021: map the iBoot-reserved SEG0/SEGi phys at the latched RVBAR entry; probe refuses unless no-map `/reserved-memory` nodes cover both windows; `0` = own memory (staged copy + iBoot patches), the only mode on T6020/T6022 |
| `fw_start` | `1` | rtclient | Fenced Linux-context firmware start (boot contract) |
| `fw_start_table_mode` | `2` (skip) | rtclient | Pre-CPU engine table block: 0 abort, 1 write, 2 skip |
| `fw_start_rtb_mode` | `0` | rtclient | RTBuddy/RTKit-app-endpoint select; off = legacy ChMan transport |
| `boot_prevent_nap` | `1` | boot.c | Retain firmware nap-prevention counter via init resource bit |
| `scratch3_ack` | `1` | rtclient | After DONE, write SCRATCH3 = 0x08042006 |
| `legacy_only` | `1` | rtclient | Use the pinned 13.5 legacy ChMan transport |
| `legacy_query` | `1` | rtclient | Service bounded startup allocations and CONFIG_GET |
| `hello_wait_ms` | `0` | rtclient | RTKit HELLO wait in legacy mode. 0 skips RTKit, so the ANE mailbox never starts: the 13.5 firmware sent no HELLO on any recorded boot (-ETIME after 1000 ms), and starting the mailbox enables AIC2 884, which then fired ~700,000 times/s (receipts/2026-09-30-t6021-stock-mailbox). A firmware that speaks RTKit needs `1000` (the lab value). |
| `poll_rx` | `1` | rtclient | Drive RX by `apple_rtkit_poll` from the workqueue (only with an RTKit instance: `legacy_only=0`, or legacy mode with `hello_wait_ms` > 0) |
| `start_app_eps` | `1` | rtclient | STARTEP fw-announced app endpoints after a successful handshake |
| `bo_total_max_mb` | `12288` | rtclient | Cap on the BO bytes held at one time, in MiB; `BO_INIT` returns `ENOSPC` above it. `0` refuses every `BO_INIT`; there is no unlimited value. Read at load (0444). |
| `bo_total_bytes` | read only | rtclient | The BO bytes counted against `bo_total_max_mb` now. |
| `trace_td` | `0` | rtclient | Runtime switch (0644). 1 records a read-only timeline of each CALL (ack, each new value of the last-taken-TD word, the IO_T2H events, the finish) in debugfs `ane_t6021/trace_td`; each switch to 1 empties it. The TD word is read only while the ANE pmgr PS words read 0x3ff. 0 leaves the CALL path unchanged. |

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
default path. `af_bridge_macos` (#33) is absent too: the 26 macOS AXI2AF
bridge values did not change the encoder time
(receipts/2026-10-01-t6021-af-bridge-run).

## DRM device

A bare `modinfo ane_t6021.ko` reports
`alias: of:N*T*Capple,t6021-ane`. The DRM device, when bound,
exposes the ABI 2 ioctl set per `ane/src/uapi/drm/ane_accel.h`
(ABI 2 added in commit 23b8eef, merged to main in 1df4412):

- `DRM_IOCTL_ANE_BO_INIT` — allocate a coherent BO and return its
  handle.
- `DRM_IOCTL_ANE_BO_FREE` — drop the handle. The memory frees when
  its last reference drops, except a BO the firmware received: a
  program section is held until reboot, an io BO is parked for reuse
  (see "BO lifetime").
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

The driver reports `major = ANE_ABI_M2_MAJOR = 2`
(`ane/t6021/ane_t6021_rtclient_main.c`, commit 27e996a). libane
selects its ABI-2 backend in `libane/ane.c` (commit 8a4379e):
`is_ane_device()` accepts `ANE_ABI_MAJOR` and `ANE_ABI_M2_MAJOR`, and
`__ane_init_shift()` calls `ane_m2_open()` (`libane/ane_m2.c`) when
the major is `ANE_ABI_M2_MAJOR`. `tools/ane-run` drives it (the
`--anec` path of every proven run).
