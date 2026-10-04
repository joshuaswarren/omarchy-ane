# wdt-hang-test: unattended reset-to-stock prototype receipt

Status: EXPERIMENTAL and HARDWARE-GATED. This PR is not merged until the
hardware windows below pass. The module never autoloads: nothing in this
change installs a modules-load file, a DKMS package, or a modprobe
configuration, and it is not packaged. It runs only when a window operator
explicitly `insmod`s it.

## What it is

`tools/wdt-hang-test/` is an out-of-tree kernel test module for the
"unattended reset to stock" recovery path used before running experimental
probes that may wedge the SoC:

1. It arms the Apple SoC watchdog (WD1: `CUR_TIME` 0x10, `BITE_TIME` 0x14,
   `CTRL` 0x1c, `RESET_EN` bit 2, layout per `drivers/watchdog/apple_wdt.c`)
   through the watchdog device the kernel already owns: it finds the
   `apple-watchdog` platform device, validates the `"Apple SoC Watchdog"`
   identity string, and calls `ops->set_timeout()` (30 s default) and
   `ops->start()`. If no `apple_wdt` device is registered it maps ONLY the
   DT-described `apple,wdt` window (`reg[0]`) and uses the same layout. It
   never touches an address outside that window.
2. A kernel thread pets the watchdog every 7 s while the system is healthy.
3. After `hang_delay_sec` (15 s default) the thread prints an emergency
   marker and spins with interrupts and preemption off. The pings stop and
   the watchdog resets the machine within `wdt_timeout_sec +
   ping_interval_sec` (31-45 s at defaults).
4. A clean unload restores the previous hardware timeout instead of stopping
   the watchdog, so the `/dev/watchdog0` owner (systemd) keeps the machine
   healthy afterwards. After the hang the thread is unkillable and `rmmod`
   blocks; only the reset ends it.

Reset to the STOCK entry requires the experimental boot to be one-shot. The
two halves are proven separately, boot-once first:

- Boot-once half: a marked, otherwise-stock entry must boot exactly once
  and revert. On the Limine rig (T8103-class, Limine 12.x) the one-shot is
  the Boot Loader Interface variable `LoaderEntryOneShot`
  (vendor GUID 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f; `LoaderFeatures` bit 3
  advertises oneshot entry control; Limine consumes the variable). On the
  GRUB rig (T6021-class) the one-shot is `grub-reboot` (`next_entry` in
  `grubenv`, already proven consumed on a stock-kernel reboot).
- Reset half: under a one-shot boot, `insmod`, hang, watchdog reset, and
  the machine must return to the stock default entry unattended, inside a
  6-minute no-return bound, with the one-shot state consumed and the boot
  store (ESP `boot.bin`) unchanged.

The full host-specific protocol (exact commands, timings, stop rules
S1-S4, rollback, success definition) lives in the private lab notebook;
this receipt carries the rig-independent procedure.

## Measured baseline (sources: lab notebook receipts, kernel sources)

- The SoC's own watchdog already fires when a system hangs hard: measured
  reset classes of about 59 s, 63 s (the W10 fabric-fatal read:
  engine+0x1854000..0x1c04000 on the T6021, pre-log pinned by netconsole),
  67 s, and about 2 minutes.
- Boot chain watchdog state, from sources: m1n1 v1.6.1 disables WD1 at
  startup (`src/main.c:161` -> `wdt_disable()`); the lab U-Boot re-arms it
  for 60 s (`CONFIG_WDT_APPLE=y`, `CONFIG_WATCHDOG_AUTOSTART=y`,
  `CONFIG_WATCHDOG_TIMEOUT_MSECS=60000`); the Linux core
  (`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`, `CONFIG_WATCHDOG_OPEN_TIMEOUT=0`)
  pings a firmware-armed watchdog until userspace takes over; the root
  systemd then arms a 120 s hardware timeout with 60 s pings
  (`RuntimeWatchdogSec=120`). The module makes the reset latency explicit
  (31-45 s) and independent of that chain.
- iBoot handover watchdog state is not known (recorded unknown; no sysfs
  status on these kernels).

## Build receipts (W=1, offline, no hardware run)

| tree | result |
|---|---|
| 7.1.13-3-1-ARCH (macstudio ALARM chroot, `~/src/m2-headers/7.1.13-3-1-ARCH`, docker `dg-alarm-py314:sep23`) | PASS, zero warnings; `VERMAGIC: 7.1.13-3-1-ARCH SMP preempt mod_unload aarch64`; `wdt_hang_test.ko` sha256 `ab5b1db06c130698ed97e68f04b818907593b0bbfdb56907e0e2d66302b50f22` |
| local 7.1.13 built tree (Module.symvers present) | PASS, W=1 + full modpost |
| `ane-driver-aurora` efe6e359 (modules_prepare worktree, AuroraAnePr config) | W=1 compile PASS, zero warnings; modpost unresolved symbols are a tree-state limit (no built kernel, no `Module.symvers`), not a module defect — the same source modposts clean on the built 7.1.13 tree; rig loadability is gated on the vermagic check at window time |

Build command:

```sh
make -C <prepared-arm64-kernel-tree> ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- M=$PWD W=1 modules
```

The tree must match the target kernel or `insmod` refuses the vermagic.

## Safety properties

- Register access is restricted to the kernel-registered watchdog device or
  the DT `apple,wdt` window; no ANE, pmgr, DART, or CoreSight access; no
  `/dev/mem`.
- Nothing autoloads: no modules-load drop-in, no DKMS, no package.
- The hang is deliberate and total once `hang_delay_sec` elapses; the
  protocol forbids running it outside a scheduled window with the
  one-shot mechanism armed and verified first, a camera + console watch,
  and hard reset reserved to the window owner as the backstop.
