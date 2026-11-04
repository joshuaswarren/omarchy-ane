# J700/H17: firmware-boot smoke runs clean but the firmware does not wake

Boot `105a8645-ecce-48a8-9e55-23962582f07e`, warm reboot, run at uptime ~33 s.
Commit `46efa4b`. Module `ane_h17_fwboot_probe.ko` — the corrected Apple-order
bring-up (receipt `2026-11-04-j700-h17-firmware-load-path/`) plus the SCRATCH7
wake-word poll. No fault, no freeze, clean teardown (CPU re-held, module
unloaded).

## The three probe fixes that this run exercises

1. `SCRATCH7 := 1` (warm-boot request) — the earlier `hal_probe` only *read*
   it where it should have written `1`.
2. RVBAR write **skipped** — on J700 the ADT sets `pre-loaded = 1`, so macOS
   skips both the RVBAR read and write; the earlier mask `0xff7d…` was also one
   bit off (`0xff7e…` preserves address bit 49).
3. Poll `SCRATCH7 == 0x08042006` (1 ms × 1000) — the signal the firmware raises
   when it runs, which the earlier probes never checked.

## Result

```
G13 RVBAR=0x10000000001 (pre-loaded; write skipped)
G16 CPU_CONTROL := 0        -> status 0x6a
G17 CPU_CONTROL := 0x10     -> status 0x68
G18 SCRATCH7=0x0 ready=0 polled=1000 ms
mailbox done rx=0 i2a_ctrl=0x20001 a2i_ctrl=0x20001
```

`RVBAR = 0x10000000001` is **latched** (bit 0) to reset-vector DVA
`0x1000000000` — exactly the firmware `__TEXT` DVA the firmware-load receipt
derived, confirming iBoot pre-loaded the firmware and latched the vector. Yet
after the CPU was released (`CPU_CONTROL := 0x10`, `CPU_STATUS 0x6a → 0x68` =
"released from stop"), `SCRATCH7` stayed `0x0` for the full 1000 ms and the
ASC mailbox stayed empty.

Also observed: `D2 0xc000 = 0xf0003ff` after the PS write — bits `[19:16]` (the
`setPSAutoMinLevel` auto-gating floor) read `0xF` ("pinned"), ack `[7:4]` = `0xF`.
That is the *power* floor, not the functional clock.

## What this establishes

- The corrected bring-up is a **clean negative**: the firmware still does not
  wake. This is no longer a probe-ordering artifact.
- The firmware *is* pre-loaded and the reset vector is latched correctly, so
  "firmware was never loaded" is **not** the cause.
- Two candidate blockers remain, and this run separates neither:
  1. **Functional clock off** — the ANE's `enableDeviceClock` is a PMP mailbox
     command (`2026-11-04-j700-h17-clkgen-enable-path/`), which Linux cannot
     issue. If the clock is gated, the CPU is released but never fetches.
  2. **DART mapping not present** — the firmware lives in DRAM at DVA
     `0x1000000000` behind the `dart-ane` IOMMU. If iBoot's DART page tables do
     not survive into Linux, the CPU fetches an unmapped address. The three
     `apple,t8140-dart` nodes are `disabled` in the Linux DT.

## Next step

The one thing that separates (1) from (2) is the H16 driver's `stage=boot`
(`ane/h16/ane_h16_main.c`), which performs a Linux-side `iommu_map` of the
firmware at the ADT IOVAs and then releases the CPU. If the firmware wakes
*with* the DART mapped, the blocker was the DART (and the clock was on all
along); if it still does not wake, the blocker is the clock (PMP command). That
driver's of_match currently lists only t8132/t6040/t6041 — wiring t8140 and
enabling the DART nodes is the next investigation.

## Not established

- Whether the ANE functional clock is on or off.
- Whether iBoot's DART page tables survive into Linux.
- Whether the full CE handshake (the `0x18a4010`/`0x18a4018` reads this probe
  omits, using only the `0x18a4000` toggle) is additionally required for the
  firmware to start.
