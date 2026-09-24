# T6021 CoreSight read and DART stream state (2026-09-24)

Host: jw14m2-linux, kernel 7.1.13-3-1-ARCH. All eight ANE islands were on
(pmgr ane_cpu `0x1f0003ff`). The core was released and stalled, with
CPU_STATUS `0x28`. Vehicle: `ane_ascdbg_t6021.c`, the T6001 ane_ascdbg
from f90f6c1 with three changes:

- It finds `284000000.ane` by name.
- It gates on pmgr ACTUAL instead of the runtime-PM raise. The raise hung
  a bind on this box.
- It refuses `0x1010080-0x101009c` and `itr` while EDPRSR.OSLK is set.

Every access was pre-logged and synced to `/var/tmp/ascdbg/ascdbg.log`.

## CoreSight (read-only, 17:18 CDT)

| register | T6021 | T6001 |
|---|---|---|
| EDPRSR engine+0x1010314 | `0x00000000` (read twice) | `0x2ab` |
| EDDEVARCH engine+0x1010fbc | `0x00000000` | `0x09108a15` |

There was no hang and no SError. The block reads zero, so the PC was not
read. No EDLAR, OSLAR, DBGWRAP, or DTR access was made.

## DART streams (read-only, then one restore)

| instance | PARAMS_C | ENABLE | ERROR | TCR0 | TTBR0 | TCR15 | TCR1-14 |
|---|---|---|---|---|---|---|---|
| 0x285800000 | 0x00010010 | 0xfffe | 0x00a00000 | 0x9 | 0x100124d1 | 0x2 | 0 |
| 0x285810000 | 0x00100010 | 0xffff | 0x00f00000 | 0x9 | 0x100124d1 | 0 | 0 |
| 0x285820000 | 0x00100010 | 0xffff | 0x10700000 | 0x9 | 0x100124d1 | 0 | 0 |

ERROR has no FLAG. Its SID field is 27:20 in the m1n1 dart8110 layout.
TTBR1-15 are 0. dart0 TCR15 = 2 was written by hand in the IOMMU_CACHE
test. macOS writes 2 on all three instances. dart0 ENABLE was `0xfffe`
after the SID-0 cleanup. It was restored with a write of `0xffff` and read
back as `0xffff`.

## Earlier today, same boot family

- DATA before/after release byte-identical, 0x430000 bytes: no stores.
- IOMMU_CACHE map: leaf PTE `0x000fff1000084801`, NO_CACHE clear, PA
  `0x10000848000`. SCRATCH7 0 and outbox empty for 60 s.
- ENABLE bit 0 already set: stall unchanged over 60 s.
