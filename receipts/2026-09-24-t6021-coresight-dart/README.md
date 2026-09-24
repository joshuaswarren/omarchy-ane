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

## SID-15 bypass run (17:38 CDT, Main go)

`sid15_run.sh`: TCR15 = 0x2 on dart1 (0x28581103c) and dart2
(0x28582103c), read back 0x2 on both. dart0 was already 0x2. I2A bit 0
was already set. The script wrote CPU_CONTROL 0 then 0x10 and polled every
5 s for 60 s.

    t=0..60  scratch7=00000000 i2a=00020001 status=00000028
             err=00a00000/00f00000/10700000 (unchanged)

The core had already been released at the 16:08 IOMMU_CACHE run on this
boot. A follow-up check wrote CPU_CONTROL = 0 and read STATUS: it stayed
0x28 for 100 ms, so the write does not stop the core. The T6021 image word
at TEXT+0x200 is 0x14000000 (`b .`), and +0x234 is `msr VBAR_EL1, x0`.
A core parked there does not recover when the fetch path changes, so this
run is inconclusive. Full access log: `ascdbg.log`.
