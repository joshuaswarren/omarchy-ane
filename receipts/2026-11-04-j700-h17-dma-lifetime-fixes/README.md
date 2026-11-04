# Firmware staging: translated DMA backing, reserved aliases, retained lifetime

Parent `680a5d0`. Software changes only; no CPU release or device test.

## Fixed

- Resolve coherent backing pages through the allocation's DMA handle in
  its DMA-API IOMMU domain, never virt_to_phys(cpu_pointer). Verify aligned,
  nonzero translations and fail closed for unsuitable domains.
- Require IOMMU_RESV_RESERVED coverage of every fixed firmware alias
  before coherent allocation. The updated T8140 overlay **source** adds
  a no-reg reserved-memory node with iommu-addresses plus a memory-region
  reference. This protects fixed aliases from the DMA allocator when
  applied at boot. This revision has not been installed on the machine.
- Bound segment size to the pinned DATA virtual size and reject physical
  or IOVA overflow/misalignment before staging.
- After CPU-release attempt, retain successful binding, module reference,
  mappings and runtime power on both success and timeout. Preserve the
  error in boot_error and the FAIL RESULT. Returning an error after CPU
  release would trigger failed-probe devres cleanup; dropping runtime
  power would contradict the promised retained mapping lifetime.

The running kernel headers expose iommu_is_dma_domain and reserved-region
APIs. Kernel source checkout `linux-aurora` at
`b6f070c911934c9c377c4be939b883bf32f37acf` provides the boot-time reservation
mechanism in drivers/iommu/of_iommu.c (memory-region + no-reg
iommu-addresses) and drivers/iommu/dma-iommu.c (reserve_iova). The maintained
ANE T6021 loader uses DMA-IOVA-to-physical translation when creating fixed
aliases; no new device-register or firmware-ABI claim is made here.

## Tests

The simulator now makes coherent CPU pointer, DMA handle and backing PA
different, and models source DMA mappings. The positive boot test checks
the alias points at the translated backing PA. Missing alias reservation
and non-DMA domain tests refuse before allocation/engine writes. Timeout
checks binding success with boot_error=-ETIMEDOUT, retained runtime power,
module pin and unfreed DMA backing. Foreign-map/short-unmap tests still pass.

Full simulator/mutation suite, W=1 kernel build, git diff --check, overlay
compile and two-overlay fdtoverlay dry-run all pass. Mutations substituting
the DMA handle for the PA and dropping runtime power after CPU release are
caught by matching tests. Local complete outputs:
`/var/tmp/opencode/h17-dma-domain-sim.log` and
`/var/tmp/opencode/h17-dma-domain-build.log`.

This is not hardware qualification. T8140 stage=boot remains fail-closed.
Next prerequisite: a checked, durably logged H17 ordering adapter and
explicit acquisition/retention of all required power domains. Physical
preload reservation/content admission also remains to be established.

## Reservation installation after bf21193

The source overlay was compiled, installed in the existing opted-in
T8140 stage-boot overlay slot, and update-m1n1 rebuilt the boot image.
The old overlay is backed up at
`/var/tmp/opencode/h17-pre-alias-overlay-backup.dtbo`, SHA256
99492a071f53965f5fdfa3506289a291ebd86768c988595ca687f6fed8e2156e.
The generated DTB's iommu-addresses tuple is owner-phandle plus
`0x100 0 0 0x380000`, reserving [0x10000000000,0x10000380000).
The old boot's IOMMU-group reserved_regions output is empty, saved at
`/var/tmp/opencode/h17-alias-reservation-before.txt`.

Next boot check: verify the memory-region owner/tuple, actual DART driver
bindings and /sys/kernel/iommu_groups/<ANE group>/reserved_regions. No
CPU release or stage=boot is part of that check. The H17 guard remains.

## Ownership admission, after 6ffbd0e (live boot f62ad8af)

Live check on boot f62ad8af-c34e-417a-b546-f2e2258763c1: IOMMU group 15
reports `0x10000000000-0x1000037ffff reserved`, all three DARTs bound to
apple-dart, ANE node okay and unbound. Recorded in
`/var/tmp/opencode/h17-alias-live-f62ad8af-*/metadata.json`.

Per the read-only preload review (session ses_e593c2d98ffef6e9aneN3903I4):
these preload windows are owned by exclusion from System RAM, not by a
reserved-memory node; "must be reserved" would fail and was T6021's rule,
not this boot's. `ane_h16_admit_preload()` now enforces, before any
mapping or read of preload DRAM: segments non-empty/aligned/non-overlap,
`region_intersects(..., IORESOURCE_SYSTEM_RAM, ...)` == REGION_DISJOINT,
and zero overlap with any reserved-memory reg. It is enforced in the boot
path and logged as metadata verdicts at stage=dt. A boot refuses on a
nonzero SCRATCH7 wake word (firmware not quiescent). Per-segment snapshot
buffers no longer outlive their short demand-window memremap. Preload
diffs are now byte-whitelisted at patchbay/tunables/CE-env positions with
an accepted-diff count and staged-image SHA-256 logged; the old value-
based `live && !file` rule is removed. New negative scenarios:
refusing BSS/IOVA-like diffs, whitelist boundary, snapshot failure and a
System-RAM-overlapping seg. Full suite passes:
`/var/tmp/opencode/h17-ownership-sim.log` (21 scenarios), W=1 build
`/var/tmp/opencode/h17-ownership-build.log`, mutation tests GREP-clean.

Next boot check: verify the memory-region owner/tuple, actual DART driver
bindings and /sys/kernel/iommu_groups/<ANE group>/reserved_regions. No
CPU release or stage=boot is part of that check. The H17 guard remains.
