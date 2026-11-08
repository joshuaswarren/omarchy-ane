# Staged IOMMU mapping cleanup preserves foreign mappings

Parent `1d5c004`. Source-only safety fix; no device access.

The old unwind unmapped entire requested segment ranges, including pages
that predated the transaction. Track the contiguous successfully mapped
prefix of each segment instead, and unmap only those prefixes. Check the
unmap return length: if translations may remain, retain the backing
buffer rather than freeing memory that a device can still address.

The existing neg-dart-map scenario now asserts that its colliding foreign
page remains mapped after refusal, as well as checking the owned prefix
was unmapped. New neg-short-unmap injects a failed unmap and verifies the
buffer stays allocated, foreign mapping survives, and CPU release never
occurs. The simulator shim now returns actual unmapped lengths like the
kernel API instead of returning void.

Commands passed:

```
make -C ane/h16 sim
make -C ane/h16 KERNELDIR=/lib/modules/$(uname -r)/build W=1
git diff --check
```

Private full logs: `/var/tmp/opencode/h17-owned-map-sim.log` and
`/var/tmp/opencode/h17-owned-map-build.log`. This is simulator evidence,
not DART hardware qualification. T8140 stage=boot is still refused.

## Remaining blockers

- Coherent DMA backing must not be converted with virt_to_phys. The
  maintained T6021 loader instead resolves DMA IOVAs page-by-page through
  its IOMMU domain; adapting that requires validating the domain and
  preventing the DMA allocator from reusing fixed alias IOVAs.
- H17 CE/HAL/PWGATE sequencing with checked polls and durable logging
  must precede any CPU release.
- Explicitly vote all required domains and retain driver/resource/power
  ownership after both CPU start and an uncertain timeout. A failed probe
  must not trigger cleanup of memory/register mappings a released CPU
  may still use.
- Live ADT ranges are now decoded, not content-verified. Validate backing
  reservations and firmware identity before copying or exposing pages.
