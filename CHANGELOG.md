# Changelog

## Unreleased

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