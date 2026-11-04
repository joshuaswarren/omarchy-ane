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
