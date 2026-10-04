# wdt-hang-test

Kernel test module for the "unattended reset to stock" recovery path:
arm the Apple SoC watchdog (WD1), pet it from a kernel thread while the
system is healthy, then deliberately hang the CPU with interrupts off so
the watchdog fires and firmware resets the machine.

After the reset the bootloader must pick the stock default entry. That
requires the experimental boot to be one-shot:

- Limine rigs: one-shot via the Boot Loader Interface variable
  `LoaderEntryOneShot` (Limine reads and consumes it; entry named by its
  config ID or path).
- GRUB rigs: `grub-reboot <entry-id>` (one-shot `next_entry` in
  `grubenv`, consumed by GRUB on the next boot).

Run both halves separately before combining them: first prove the one-shot
boot with a harmless cmdline marker, then run this module under a one-shot
boot.

## Parameters

| param | default | meaning |
|---|---|---|
| `wdt_timeout_sec` | 30 | hardware timeout (clamped 5-120) |
| `ping_interval_sec` | 7 | kernel-thread pet interval (max timeout/2) |
| `hang_delay_sec` | 15 | seconds of healthy petting before the hang |
| `hang_cpu` | -1 | pin the hang thread to a CPU (-1 = no pin) |
| `disarm_on_unload` | false | stop the WDT on clean unload instead of restoring the previous timeout |
| `mode` | auto | `core` (registered apple_wdt device) or `dt` (map the DT "apple,wdt" window directly) |

Reset latency from `insmod`: `hang_delay_sec` + up to `wdt_timeout_sec +
ping_interval_sec` (defaults: about 31-45 s).

## Register access policy

The module drives the watchdog only through the device the in-kernel
`apple_wdt` driver registered (`ops->set_timeout/start/ping` on the core
`watchdog_device`, found via the `apple-watchdog` platform driver and
verified by the `"Apple SoC Watchdog"` identity string). If that device
is not registered, it maps ONLY the DT-described `apple,wdt` window
(reg[0]) and uses the same WD1 layout as `drivers/watchdog/apple_wdt.c`.
It never touches an address outside that window.

On a clean unload the previous hardware timeout is restored (not stopped)
so the owner of `/dev/watchdog0` (systemd) keeps its watchdog healthy.
`disarm_on_unload=1` stops the hardware instead; use it only when no
other watchdog user exists.

After the hang the thread is unkillable and `rmmod` blocks; only the
watchdog reset (or a hard reset) ends it. Run `sync` and record all
pre-state before `insmod`.

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
