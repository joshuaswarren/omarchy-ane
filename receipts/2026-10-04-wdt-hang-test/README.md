# wdt-hang-test: unattended reset-to-stock prototype receipt

Status: EXPERIMENTAL and HARDWARE-GATED. The RESET HALF is proven on the
T6021 rig (window A, 2026-10-04: clean arm/pet/restore, then an unattended
SoC reset back to the stock default entry in 2 min 05 s). This PR stays
UNMERGED: the module is still experimental, it never autoloads, and it is
not packaged; the T8103 Limine-rig hang procedure is NOT RUN. It runs only
when a window operator explicitly `insmod`s it.

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
  and revert, where a one-shot is needed at all. The rigs differ,
  MEASURED:
  - GRUB rig (T6021-class): `grub-reboot` (`next_entry` in `grubenv`) is
    proven consumed on a stock-kernel reboot, and window A proved the full
    chain with it: reset -> default entry, one-shot consumed.
    One-shot entries must copy the DEFAULT SUBMENU CHILD, not the first
    top-level entry (`GRUB_DEFAULT` resolves a submenu path to the plain
    pair; the first top-level entry is the no-wireless m2mbox pair).
  - Limine rig (T8103-class, Limine 12.9.1-1 over U-Boot): a
    `LoaderEntryOneShot` variable written FROM LINUX DOES NOT WORK.
    Measured (H262): written NV|BS|RT, efivarfs readback identical, but
    the on-disk U-Boot store `ubootefi.var` kept its old bytes and the
    next boot selected the default entry. Cause: U-Boot's runtime
    SetVariable only updates the in-memory table
    (`CONFIG_EFI_RT_VOLATILE_STORE`); `ubootefi.var` is written only by
    boot-time services, so Linux-written variables never survive the
    reset; Limine additionally erases the one-shot variable at start by
    design. Consequences: a hang test on the STOCK default boot needs no
    one-shot (the reset lands in the same stock entry); experimental
    entries need a `limine.conf` `default_entry` edit with a guarded
    early revert plus a boot counter, or a boot-time `ubootefi.var` edit
    (boot-chain file write; needs the owner's go; not tried).
- Reset half: under a one-shot boot, `insmod`, hang, watchdog reset, and
  the machine must return to the stock default entry unattended, inside a
  6-minute no-return bound (marker-to-first-boot-message, not
  marker-to-WDT-fire), with the one-shot state consumed and the boot
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
  (`RuntimeWatchdogSec=120`). The module's own bound is `hang_delay_sec`
  plus up to `wdt_timeout_sec + ping_interval_sec` (31-45 s at defaults),
  valid when the module is the only keepalive producer; every keepalive
  write resets WD1, so another producer defers the reset by up to its own
  ping interval (M2 rig: 60 s systemd ping + 30 s module timeout -> up to
  90 s after the pings stop).
- iBoot handover watchdog state is not known (recorded unknown; no sysfs
  status on these kernels).

## Hardware results (measured, 2026-10-04)

- T6021 rig, window A (pre-registered, hard stop 12:45Z, 12-minute reboot
  notices): stage 1 — `insmod hang_delay_sec=300`: WD1 armed at 30 s in
  core mode (identity `"Apple SoC Watchdog"`), petter kept the box alive,
  `rmmod` restored the previous timeout and the box survived well beyond
  one ping interval afterwards (a stuck 30 s hardware timeout with 60 s
  systemd pings would have bitten). Stage 2 — defaults: emerg marker at
  the wedge 12:13:43Z, ssh loss, SoC reset, first message of the new boot
  12:15:48Z — 2 min 05 s is marker-to-first-boot-message; the WDT fire
  itself has no direct timestamp (the pre-reset log ends at the marker;
  with the 60 s systemd producer the bite can land up to 90 s after it -
  30 s timeout + one 60 s ping interval - inside the ssh-loss
  window) — then an UNATTENDED return on the stock DEFAULT entry
  with the one-shot consumed and the ESP `boot.bin` sha unchanged.
  Post-reset end state: ane_t6021 bound, failed units 0, smoke 20/20
  bit-exact. Full record: lab notebook entries/M2WdtRecovery/
  20261004T105500Z-jw14m2-linux-wdt-recovery-window-a.md (+ artifacts).
- T8103 Limine rig (H262): the boot-once check FAILED as described above;
  config restored in-window (`limine.conf` sha back to pre, no OneShot
  variable, `boot.bin`/`ubootefi.var` unchanged). The stock-default hang
  procedure on this rig is NOT RUN.

## Pending hardware retest (required after the review-fix commit)

The review-fix commit changes unload and arm behavior; the 2026-10-04
window A run predates it and does not cover the new paths. Retest on one
M2 stock-kernel boot with the one-shot armed (grub-reboot to a stock+
marker entry), in this order:

1. Warm unload path (core/running): `insmod hang_delay_sec=300`, expect
   the armed line and the owner warning (systemd holds the device), then
   `rmmod` at +30 s; expect a clean unload line, prior timeout restored,
   box alive 140 s, no reset.
2. Unload-during-hang: `insmod` (defaults); at ~T0+17 s, just AFTER the
   emerg marker (T0+15 s), `rmmod`; expect the spin itself to abort via
   `kthread_stop` (the pet loop has already ended), a clean unload with
   the pre-insmod state restored, no reset, box alive 140 s.
3. The real reset: `insmod` (defaults); expect the emerg marker at about
   T0+15 s, unattended reset within 90 s of the marker (30 s timeout +
   up to the 60 s systemd ping interval), return on the stock default entry,
   one-shot consumed, ESP sha unchanged.
4. DT-mode path (`mode=dt`), separate scheduled window (it drives the WDT
   window directly while the built-in apple_wdt also binds on the M2):
   arm, log the clock source and tick, `rmmod`, verify the box stays
   healthy; the pre/post register readback needs an operator-decided
   policy because raw reads are out of scope for this module.

Items 1-3 fit one window (notice, camera, 6-minute watch, stop rules as
in window A).

## Protocol lessons (measured)

- On the GRUB rig, a one-shot entry must copy the DEFAULT SUBMENU CHILD:
  `GRUB_DEFAULT` resolves a submenu path; the first top-level entry is the
  no-wireless m2mbox pair.
- Every reboot step needs an explicit UTC gate checked against the notice
  (a timezone-aware `date` arithmetic slip has already burned a window).
- Return probes must compare `boot_id`: the DERP relay makes TCP connect
  succeed during an outage, so "ssh connected" alone is not evidence of a
  new boot.

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
