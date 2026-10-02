# Changelog

## Unreleased

- `tools/promote_chip.py` flips a chip between opt-in and on-by-default exactly like the PR that did it by hand: the overlays row and tested header, the overlay's opt-in marker, the omarchy-ane-check case lists, the omarchy-ane-dt opt-in enumeration, firmware-fetch DEFAULT_ON, the README State/Opt-in key cells, one CHANGELOG line. `--check` prints the diff without writing; `--apply` is idempotent and its revert restores the tree byte for byte except that CHANGELOG line. `test_ane_dt` now derives its enabled/opt-in expectations from the overlays table, so a flip needs no test edits.
- `omarchy-ane-check` prints the bring-up steps after its `UNTESTED SoC` line: add the opt-in key, on T6020/T6022/T8112 run `omarchy-ane-firmware-fetch` first, then `omarchy-ane-dt apply`, `update-m1n1`, reboot, and submit a collector row with `--ane-smoke` when the machine is idle. The machine-parsed lines are unchanged.
- `omarchy-ane-smoke` now routes T8103/T6000/T6001/T6002 to the packaged H13 add fixture and T6020/T6021/T6022/T8112 to H14. H13 checks 64 valid little-endian fp16 planes and zero padding across the full 16 KiB tile. See `receipts/2026-10-02-h13-smoke/README.md`.
- Fix promotion verdicts: scope fault lines to the ANE device, its DARTs and mailbox; report uninstalled rows and clean no-smoke rows as not judged. One passing row promotes an opt-in chip; pass/fail conflicts block promotion. A default-on chip reverts when its latest judged row is not clean. Remove machine, owner, board, kernel and uptime thresholds.
- `omarchy-ane-smoke` uses positional `0/1/0` runner input/output indices for H13, not ANEC channel IDs.

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