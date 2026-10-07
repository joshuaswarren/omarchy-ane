# ane/common: DART t8110 page-table builder

A small, header-only builder (`dart_t8110.h`) for dart,t8110 page tables:
16 KiB pages, 2048 entries per 16 KiB table, 2 or 3 walk levels, sid
allowlist and vm window as parameters. Host check:

    make -C ane/common check

Not part of `make all`, not a kernel module, not autoloaded. The header
builds in-kernel (`__KERNEL__`) and in userspace.

## What is measured

- Leaf PTE format: `(pa >> 4) & GENMASK_ULL(37,10) | prot | subpage |
  VALID`, prot bits NO_CACHE=1, NO_WRITE=2, NO_READ=3, leaf-only subpage
  fields `0xfff << 40` and `0 << 52`. This is the Linux APPLE_DART2
  format (drivers/iommu/io-pgtable-dart.c), used by both
  "apple,t8110-dart" and the t6020-era "apple,t6000-dart" binding: the
  formats do not differ. The check reproduces the measured T6021
  ANE-DART leaf PTE `0x000fff1000084801` (receipts
  2026-09-24-t6021-coresight-dart) byte-exactly, and the same format was
  walked by working hardware when the T6021 firmware ran inference
  (receipts/2026-09-29-t6021-installed-path).
- Register words: t8110 TTBR `(root >> 14) << 2 | VALID(bit0)` and TCR
  `TRANSLATE(bit0) | FOUR_LEVEL(bit3)`; the working macOS dart-ane sid0
  TCR `0x9` and TTBR `0x1004102d` are on record (docs/t6021-ane-bringup
  -findings.md section 17); TCR `0x2` is BYPASS_DART.
- Walk shape: index shifts leaf 14, level2 25, level1 36, absolute IOVA
  bits, no vm-base rebase (REMAP_EN clear in the measured TCR). A 2^40
  window cannot be indexed by 2 walk levels (index 2048 overflows the
  2048-entry table), so windows above 2^36 select 3 walk levels and the
  FOUR_LEVEL TCR bit. The proven T6021 map at IOVA 0x10000000000 is a
  3-walk-level table.

## What is not proven

No H15/H16 DART has run any of this. A sim is not silicon: the sid
sets, vm window and register words for H16 (t8132: sids 0,10,11,15;
vm-base 0x10000000000; vm-size 0x30000000000; page 16384; second iommu
parent mapper-ane-mpm on stream 11, modeled as its own stream set) are
device-tree parameters, not validated hardware behavior. The 2-level
mode is kept as a parameter for windows up to 2^36; it cannot address
the H16 window and has no silicon check.

Mapped and table physical addresses must fit the DART PA field
(pa < 2^42), like kernel dma memory does.
