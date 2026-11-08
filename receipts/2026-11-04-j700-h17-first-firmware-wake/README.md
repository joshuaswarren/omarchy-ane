# J700 / T8140 (H17): first firmware wake on Linux

**RESULT PASS boot-wake.** On boot `638bf8a4-6198-43f9-a45a-3435663b0dec`
(kernel 7.2.11-14-1-ARCH, module `b7ec8f6`), `ane_h16.ko optin=t8140
stage=boot` released the ANE CPU and the preloaded H17 firmware
published the wake word:

```
SCRATCH7 wake word 0x8042006: firmware is running (CPU_STATUS 0x6c)
ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-wake
CPU release attempted: boot_error=0; retaining binding, mappings and power;
do NOT unload; reboot required
```

Full dmesg: `/var/tmp/opencode/h17-order-rung-638bf8a4-*/dmesg-boot-rung5.log`.
This is the first ANE firmware execution observed under Linux on this
machine. It is a firmware-boot observation, not task qualification: no
mailbox traffic, no task submission, no model ran.

## What the run executed (all durably logged, 89 ordered accesses)

1. Four power domains attached and held per-index
   (`dev_pm_domain_attach_by_id` + `pm_runtime_get_sync` each). Root
   cause of every earlier stuck-gate refusal: with more than one
   `power-domains` entry, `genpd_dev_pm_attach()` attaches NOTHING, so
   the sub-islands sat at TARGET=3/ACTUAL=0 until the driver voted.
2. Ordering helper steps 1–8 (kext recipe §5.4): CE handshake
   (0x18a4010/18, request 0x18a4000=3, ack 0x18a4008, 64-bit ack), 37
   HAL masked writes, PS 0xc000=0xf→poll, PWGATE 0x268c=0→poll `&3==0`,
   PS 0xc000=0x1000000f→poll, PS 0xc008=0xf→poll. After the run the
   words read 0x1f0003ff/0x3ff/0x3ff with ACTUAL=0xf.
3. Preload ownership admission passed; iBoot preload snapshotted:
   TEXT phys 0x10000774000, DATA phys 0x100019ac000 at the ADT IOVAs;
   2065 accepted diff bytes (patchbay/tunables/CE-env whitelist),
   staged-image SHA256 63edf11ba87202146fe44482e0c81bc49978b9172d95d0d
   344213806c005838f; 3504 KiB mapped at 0x10000000000/0x100000c0000.
4. RVBAR read 0x10000000001 — latched by iBoot at the staged TEXT
   entry; skipped the write (preloaded path, as decoded).
5. Cold-branch scratch init (steps 9/10/12), CPU_CONTROL 0 → 0x10,
   wake word within the poll window, post-wake steps 19–21.

## Fixes made during the rung (commits f6cec7a, 1fe5ddf, 24f4ac4, b7ec8f6)

- Map windows WITHOUT request_region: the T8140 ANE DARTs sit inside
  the engine window; the first insmod failed -EBUSY at the first
  ioremap (no MMIO had happened).
- Attach every power domain per-index (the core skips multi-domain
  devices); the ps gate is back to full enforcement now that genpd
  raises all four words.

## State and rules

The module is pinned, power held, firmware running. **Do not unload;
reboot before any further rung.** The wedged-firmware parking rules and
the one-write-budget-per-boot discipline are unchanged.

## Next rung (fresh boot)

`stage=boot hello_wait_ms=1000`: after the wake word, poll the ASC
mailbox for the RTKit HELLO and answer EPMAP — the first firmware
handshake beyond the boot token. The boot token proves the CPU ran;
RTKit HELLO proves the firmware's command path works.
