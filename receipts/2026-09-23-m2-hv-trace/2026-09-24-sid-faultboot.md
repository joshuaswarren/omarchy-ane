# SID enable + fault-boot provenance — 2026-09-24 ~10:55 UTC

## (2) SID path configured, no fault pending
- TCR0=0x9 (TRANSLATE + FOUR_LEVEL), TTBR0=0x1001248d valid, TCR1=0.
- ERROR_STREAMS=0 is the fault-latch word (nothing latched), not config.
- ENABLE=0xffff. Stale ERROR_ADDR cleared (W1C): err was 0, now 0.
- Conclusion: SID0 translate path is up; the silent fetch is not a
  disabled stream.

## (1) 0xdca10 fault boot: 2026-09-20 pre-module userspace release
- Netconsole 17:48: NO PGD stream 0 at 0x100000dca10, nothing mapped.
- Inversion: unmapped fetch faults visibly; mapped fetch goes silent.
  The mapped fetch never issues.

## State left: ctl=0 (quiesced), status 0x28 latched, outbox 0x20001/0xa0001.
