# wdt-hang-test

Kernel test module for the "unattended reset to stock" recovery path:
arm the Apple SoC watchdog (WD1), pet it from a kernel thread while the
system is healthy, then deliberately hang the CPU with interrupts off so
the watchdog fires and firmware resets the machine.

Reset to the STOCK entry requires the bootloader to pick the stock default
on the next boot. The two rigs differ, and the boot-once half is proven
per rig:

- GRUB rigs (T6021-class): `grub-reboot <entry-id>` (one-shot `next_entry`
  in `grubenv`, consumed by GRUB on the next boot). MEASURED: consumed on
  a stock-kernel reboot, and the full reset half is proven end-to-end
  (see Hardware results).
- Limine rigs (T8103-class, Limine over U-Boot): a `LoaderEntryOneShot`
  variable written FROM LINUX DOES NOT WORK, measured: U-Boot's runtime
  variable store is RAM-volatile (`CONFIG_EFI_RT_VOLATILE_STORE`: the
  write is accepted in memory and readback succeeds, but the on-disk
  `ubootefi.var` store is written only by boot-time services), so the
  variable never survives the reset and never reaches Limine; Limine also
  erases the one-shot variable at start by design. Consequences:
  - a `wdt_hang_test` run on the STOCK default boot needs no one-shot at
    all (the reset lands in the same stock default entry);
  - experimental ENTRIES need a `limine.conf` `default_entry` edit with a
    guarded early revert plus a boot counter, or a boot-time `ubootefi.var`
    edit (boot-chain file write; needs the owner's go, not tried).

Run both halves separately before combining them: first prove the one-shot
boot with a harmless cmdline marker where a one-shot is needed at all.

## Hardware results (measured, 2026-10-04)

- T6021 rig, window A: stage 1 (arm/pet/restore, `hang_delay_sec=300`) -
  WD1 armed at 30 s through the registered `apple_wdt` device, the petter
  kept the box alive, `rmmod` restored the previous timeout and the box
  survived well beyond one ping interval afterwards. Stage 2 (defaults) -
  emerg marker at the wedge (12:13:43Z), SoC reset, first message of the
  new boot at 12:15:48Z (2 min 05 s marker-to-first-boot-message),
  UNATTENDED return on the stock default entry, one-shot consumed, ESP
  `boot.bin` unchanged.
- T8103 Limine rig: `LoaderEntryOneShot` from Linux FAILS as described
  above (written NV|BS|RT, efivarfs readback ok, `ubootefi.var` bytes
  unchanged, next boot selected the default). The stock-default hang
  procedure on this rig is NOT RUN.

## Parameters

| param | default | meaning |
|---|---|---|
| `wdt_timeout_sec` | 30 | hardware timeout (clamped 5-120) |
| `ping_interval_sec` | 7 | kernel-thread pet interval (max timeout/2) |
| `hang_delay_sec` | 15 | seconds of healthy petting before the hang |
| `hang_cpu` | -1 | pin the hang thread to a CPU (-1 = no pin) |
| `disarm_on_unload` | false | stop the WDT on clean unload instead of restoring the pre-insmod hardware state |
| `mode` | auto | `core` (registered apple_wdt device) or `dt` (map the DT "apple,wdt" window directly) |

Reset latency from `insmod`: `hang_delay_sec` + up to `wdt_timeout_sec +
ping_interval_sec` (defaults: about 31-45 s). This bound holds only while
no other keepalive producer pings the WDT faster than `wdt_timeout_sec`
(measured on the M2 rig: systemd pings every 60 s against the 30 s module
timeout, and the reset fired inside the bound).

## Register access policy

The module drives the watchdog only through the device the in-kernel
`apple_wdt` driver registered (`ops->set_timeout/start/ping` on the core
`watchdog_device`, found via the `apple-watchdog` platform driver and
verified by the `"Apple SoC Watchdog"` identity string). If that device
is not registered, it maps ONLY the DT-described `apple,wdt` window
(reg[0]) and uses the same WD1 layout as `drivers/watchdog/apple_wdt.c`
(pinned: omarchy-linux `josh/ane-driver-aurora` commit `efe6e359`,
lines 42-44 and 52). It never touches an address outside that window.

A clean unload restores the pre-insmod hardware state. Core mode: if the
watchdog was running before `insmod` (the normal case - systemd owns
`/dev/watchdog0`), the previous timeout is written back and the watchdog
keeps running; if it was stopped, it is stopped again. DT mode: the
`WD1_BITE_TIME` and `WD1_CTRL` values read at arm time are written back.
`disarm_on_unload=1` stops the hardware instead (core: `ops->stop`;
DT: `WD1_CTRL` = 0) regardless of the prior state; use it only when no
other watchdog user exists.

Second keepalive producers: on an SMP host the hang wedges one CPU, so a
userspace owner of `/dev/watchdog0` (systemd with `RuntimeWatchdogSec`)
keeps pinging from the other CPUs. The reset bound above holds only while
its ping interval stays above `wdt_timeout_sec`; the module detects an
existing owner at load and logs a warning naming this condition. It does
not quiesce the owner (that would change machine state outside the test);
check `RuntimeWatchdogSec` before a window.

The hang spin polls `kthread_stop`, so `rmmod` ends the test cleanly at
any time (mid-hang included) and restores the pre-insmod state; during a
window the protocol forbids unloading, and the WDT reset is the expected
end. Run `sync` and record all pre-state before `insmod`.

## Build

```sh
make -C <prepared-arm64-kernel-tree> ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- M=$PWD W=1 modules
```

The tree must match the target machine's kernel or `insmod` refuses the
vermagic. W=1 clean on: the aurora `ane-driver-aurora` tree (compile;
modpost needs a fully built tree) and the 7.1.13-3-1-ARCH tree.

## Safety

- The hang is a real SoC-wide wedge once `hang_delay_sec` elapses; never
  run it on a machine you cannot let reset.
- One-shot selection must be armed BEFORE `insmod`; if the reset boots the
  test entry again, stop after one repeat and hard-reset by hand.
- The module touches only the WDT block; it does not read or write the
  ANE, pmgr, or any other device window.
