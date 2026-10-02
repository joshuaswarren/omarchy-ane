# Changelog

## Unreleased

- `omarchy-ane-smoke` now routes T8103/T6000/T6001/T6002 to the packaged H13 add fixture and T6020/T6021/T6022/T8112 to H14. H13 checks 64 valid little-endian fp16 planes and zero padding across the full 16 KiB tile. Its runner uses positional `0/1/0` input/output indices, not ANEC channel IDs. See `receipts/2026-10-02-h13-smoke/README.md`.

- ane_stats: producer-side counters and ring for the coreglass ANE
  dashboard. Adds sysfs `/sys/class/accel/accel*/device/ane_stats`
  (busy_ns, jobs) and debugfs `ane_timeline` (last N submissions,
  seqlock-protected), plus a `stats` module parameter (default 1;
  0 = single predictable branch in the hot path, no files).
  Shared header at `ane/include/ane_stats.h`; the show callbacks
  (sysfs/debugfs) live in `ane/ane_stats_show.c` and are linked
  into both `ane.ko` (H13, ane_drv.c ane_submit) and `ane_t6021.ko`
  (T6021, ane_t6021_rtclient_main.c ane_rtclient_command).
  No new ioctls; do not change DKMS ABI-1/ABI-2 behaviour.
  Hot path: atomic counters only, no allocation/locking/formatting;
  the union rule (busy_ns = union of busy intervals) handles
  overlapping submissions on T6021. Verified by
  `tools/test_ane_stats` (host-compiled shared header, seven
  scenarios including concurrent producers and torn-read
  detection). See `receipts/2026-10-02-ane-stats/` for the
  hardware acceptance protocol on M2 (T6021) and jw16 (T6001).