# Post-RUN snapshot split — 2026-09-24 ~10:50 UTC

## Snapshot (ane_obs "snap", RUN+3 s and RUN+63 s, identical)
- ctl=0x10 status=0x28 rvbar=0x10000000001 out=0x20001/0x20001.
- VENC_SYS=0x1f0003ff, leaves +8008/+8010/+8018/+8020=0x3ff, +8028=0.
- ps_cpu=0x1f0003ff. I2A lone 0xa word, SCRATCH zero.
- DART inst0: err=0 (no FLAG), addr=0x100000dca10 STALE (earlier boot),
  streams=0, TCR=0x9 (translate, four-level), ENABLE=0xffff.

## Decision
- No fault latched + stale addr + streams=0: the ASC fetch never
  reaches the DART. Clocks/islands all on. Branch: clocks-or-reset,
  not mapping/SID. Next lever: ane_cpu reset controller + quiesce,
  then RUN.
