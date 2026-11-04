# J700/H17: state of the ANE bring-up after the clock and firmware-load decode

> **Superseded safety conclusions:** see
> `../2026-11-04-j700-h17-boot-safety-review/README.md`.
> A latched RVBAR does not prove firmware residency or translation validity,
> and the generic H16 boot path is not qualified for H17. Its T8140 boot
> stage is now fail-closed pending ordering and resource-lifetime fixes.

Consolidated summary of the receipts written on 2026-11-04. No hardware touched
in this receipt; it records conclusions from the offline decode and the two
hardware runs.

## The arc

1. **Clock-generator address corrected.** The ANE clock generator is RegMap 0x71
   at physical `0x401878000` (offset `0x1878000` in ANE resource 0), *not*
   `0x3082c8000` (map 0x6c, the CSR aperture). `2026-11-04-j700-h17-clkgen-map71-correction`.
2. **The clkgen window is live and boot-chain programmed** (251 nonzero words:
   two bank headers, PLL/dividers, mirrored config). `2026-11-04-j700-h17-clkgen-live`.
3. **The functional clock enable is a PMP mailbox command**, not an MMIO write.
   `AppleARMIODevice::enableDeviceClock` → `ApplePMGRFunctionClockGate::callFunction`
   → `_enableDeviceEmmmm` → `_sendPMPCommand`. The only MMIO clock-path write
   (`setPSAutoMinLevel`, bits `[19:16]`) is the *auto-gating* function, which the
   ANE's clock enable does not call. macOS never writes map 0x71 (its accessors
   are dead code). `2026-11-04-j700-h17-clkgen-enable-path`,
   `2026-11-04-j700-h17-clock-pmp-command`.
4. **The firmware is iBoot-pre-loaded via the ANE DART**, not loaded by the OS.
   Text at DRAM phys `0x10000774000`, DVA `0x1000000000`, `dart-ane`
   (`dart,t8110`, vm-base `0x1000000000`). The readiness signal is
   `SCRATCH7 == 0x08042006`. `2026-11-04-j700-h17-firmware-load-path`.
5. **Three probe bugs fixed** (missing `SCRATCH7 := 1` warm-boot write, wrong
   RVBAR mask + unconditional RVBAR write, and polling the mailbox instead of
   `SCRATCH7`), and a corrected firmware-boot probe ran.
6. **Clean negative**: with the corrected sequence, `RVBAR` reads
   `0x10000000001` (latched to DVA `0x1000000000`, confirming the pre-load), the
   CPU is released (`CPU_STATUS 0x6a → 0x68`), but `SCRATCH7` stays `0x0` for
   1000 ms. The firmware does not wake. `2026-11-04-j700-h17-fwboot-clean-negative`.

## The conclusion

The H17 ANE bring-up is blocked, and the blocker is now precisely located:

- The firmware is **pre-loaded** and the reset vector is **latched correctly**,
  so "firmware was never loaded" is ruled out.
- The **functional clock enable is a PMP command** that Linux cannot issue
  (no PMP driver). This is the prime suspect for "released but never fetches".
- A secondary, testable suspect is the **DART mapping**: the firmware lives
  behind the `dart-ane` IOMMU, and the three `apple,t8140-dart` nodes are
  `disabled` in the Linux DT. If iBoot's page tables do not survive into Linux,
  the CPU fetches an unmapped address.

These two are not yet separated by measurement; `SCRATCH7 == 0` is consistent
with both.

## The one measurement that separates them

Wire the H16 driver's `stage=boot` (`ane/h16/ane_h16_main.c`) for T8140. That
stage performs a Linux-side `iommu_map` of the firmware at the ADT IOVAs and
then releases the CPU:

- If `SCRATCH7` reaches `0x08042006` → the DART was the blocker and the clock
  was on all along; the bring-up proceeds to full firmware bring-up.
- If it still stays `0` → the clock is off, and the PMP command is a hard
  blocker (needs a PMP driver, which is a new subsystem).

The wiring is identified but not done: add `ane_h17_soc.o` to the H16 module
build and a `{ .compatible = "apple,t8140-ane", .data = &ane_t8140_soc }`
of_match row (the soc row and the firmware pin already exist in
`ane/h17/ane_h17_soc.c` / `ane_h17_fw.h`; the firmware is installed at
`/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho`), then an opt-in DT
overlay that sets the three `apple,t8140-dart` nodes and the `ane@400000000`
node (with `iommus`) to `status = "okay"`, modelled on
`ane/h16/t8132-ane-experimental.dts`.

## Honest limits

- Whether the ANE functional clock is on or off is still unmeasured.
- Whether iBoot's DART page tables survive into Linux is unmeasured.
- Whether the full CE handshake (`0x18a4010`/`0x18a4018`) is additionally
  required is unmeasured; the probes use only the `0x18a4000` toggle.
- Nothing here is device qualification; register maps and code reading are not
  device behaviour.
