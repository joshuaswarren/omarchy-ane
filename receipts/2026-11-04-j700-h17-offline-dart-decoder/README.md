# Independent offline corroboration of the T8140 DART fault decoder

2026-11-05. Retained Apple kernelcache SHA-256:
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
Input `/var/tmp/neo/kernelcache.bin`. No device/register access or deployment.

## Result

Apple's `AppleT8110DART::_collectInterruptState` independently implements
the same per-SID address layout used by Linux:

- Record base `0x8000 + (SID << 6)`.
- Status at +0; fault address low at +8, high at +12.
- Address assembled as `u32(low) | (u64(u32(high)) << 32)`.

Apple's fatal decoder independently extracts status code bits 3:0 and
level bits 13:12 and selects **CTE invalid** for code 0, level 1 in the
collected-record path. Thus the recorded `0x1100` -> code 0, level 1,
access bits `0x100` is source-corroborated. The bits 10:8 access field
has richer cases than Linux's simple read/write flag; no claim is made
that every access encoding has been audited.

**The theory that Linux simply reads the wrong address-high offset is no
longer supported by the retained Apple driver.** This remains source
corroboration, not a measurement of every hardware revision's semantics.
In particular, it cannot prove the complete CPU-to-DART translation path
or live root state. It does establish that no speculative 16-word MMIO
scan is needed merely to find the address-half offsets.

## Exact evidence locations

All VAs below belong to the kernelcache hash above. Its validated file
mapping is `file_offset = VA - 0xfffffe0007004000`.

- `_collectInterruptState` at `0xfffffe000978aed8`:
  - `0x978af34..0x978af40` constructs `0x8000 + sid*0x40`, reads status;
    `0x978af4c` saves it to collected state +0x10.
  - `0x978af68/0x978af6c` construct `0x800c + sid*0x40`;
    `0x978afa8` reads its 32-bit word; `0x978afac` shifts left 32.
  - `0x978afcc/0x978afd0` construct `0x8008 + sid*0x40`;
    `0x978b00c` reads its low word; `0x978b018` ORs in the shifted high
    word; `0x978b01c` saves the assembled address to state +8.
  - The memory accessor object selects/checks the register window before
    dereferencing the computed register address; the intervening checks
    are retained in `collect.txt`, not elided as if these were direct
    fixed-address loads.
- `_fatalException` at `0xfffffe0009789854`:
  - `0x97899f4`: `and w8,w22,#0xf`.
  - `0x9789a18`: `ubfx w9,w22,#0xc,#2`.
  - `0x9789a50..0x9789a70`: level 1 case reaches the string
    `CTE invalid` at VA `0xfffffe00076199a8`.
  - `0x9789adc..0x9789afc` examines masks `0x500`/`0x700` for access
    classification; this is not evidence that bit 8 alone is the whole
    Apple access-type definition.

Compare with `drivers/iommu/apple-dart.c` at installed-package source pin
`4fdd0fc5c931ef9f14474942b37c3d451e97fc04`, retained excerpts in
`linux-source-excerpts.txt`. The Linux introduction commit
`f57d9004043387ed6c437ddf0afcfcc1bb49f2fd` is recorded in `linux-introduction.txt`,
not used as the independent Apple evidence.

## Reproduction and limits

```
python3 receipts/2026-11-04-j700-h17-offline-dart-decoder/analyze.py
python3 receipts/2026-11-04-j700-h17-offline-dart-decoder/verify.py
```

The extractor validates the input hash and mapping, emits symbols and
bounded relevant function windows. The verifier checks 17 exact instruction
anchors plus the CTE label and writes `decoder-verification.json`.
These are offline source/dataflow checks, not device tests. Earlier
subagents stopped on their usage limit; the completed analysis was done
in the parent session. Subsequent independent review session
`ses_e53a66e71ffeOBhrfy4uxfzHzh` **APPROVED** these conclusions and the
companion address-chain analysis, subject to the stated path/version and
runtime-state limitations.

Together with the companion firmware-channel trace, the evidence favors
fixing the proven channel-address layout incompatibility **before**
considering the already-built diagnostic kernel for a hardware measurement.
