# Changelog

## Unreleased

- Data-only records for twelve SoCs no driver binds: H15 (T8122, T6030, T6031, T6034), H16 (T8132, T6040, T6041), H17 (T8140, T8142, T6050) and H18 (T8150, T8152) in `data/ane-soc/`, with data-only overlays for T8132, T6040, T6041, T8140 and T8152 and a receipt per generation. The README data-only table now lists SoC, internal name, board count and whether an overlay exists; the file paths moved to the line above it, so the README stays above the reading-ease fail line. The README chip-coverage note names the four post-M2 ANE shapes.

- `omarchy-ane-probe` (`tools/omarchy-ane-probe`) prints the ANE state of any Apple Silicon Linux machine as one JSON document. It reports the ANE, DART, power-domain and mailbox device-tree nodes (also unknown generations), the `ane*` modules and interrupts, runtime-PM state, the pacman versions, the DKMS module files, and the `omarchy-ane-check` and `omarchy-ane-firmware-fetch --check` results. It also compares the device tree with `data/ane-soc/<soc>.json`. It is read-only, needs no root and no network, exits 0 and keeps the document within 8 KiB. The community collector embeds the document as `ane_linux.ane_probe`. See `docs/ane-probe.md`.

- The `promotion` workflow runs the promotion flow with no person in the loop: a daily cron judges the community rows, opens or updates one auto-promotion PR per PROMOTE or REVERT verdict, runs the host tests plus a fresh-verdict comparison in the gate, squash-merges, and cuts the patch release (CHANGELOG move, tag on the merge commit, GitHub release with the row shas and the tarball sha256 in the summary). `dry_run` prints the plan. Synthetic verdict fixtures are refused in this repo.

- `tools/promote_chip.py` flips a chip between opt-in and on-by-default exactly like the PR that did it by hand: the overlays row and tested header, the overlay's opt-in marker, the omarchy-ane-check case lists, the omarchy-ane-dt opt-in enumeration, firmware-fetch DEFAULT_ON, the README State/Opt-in key cells, one CHANGELOG line. `--check` prints the diff without writing; `--apply` is idempotent and its revert restores the tree byte for byte except that CHANGELOG line. `test_ane_dt` now derives its enabled/opt-in expectations from the overlays table, so a flip needs no test edits.
- `omarchy-ane-check` prints the bring-up steps after its `UNTESTED SoC` line: add the opt-in key, on T6020/T6022/T8112 run `omarchy-ane-firmware-fetch` first, then `omarchy-ane-dt apply`, `update-m1n1`, reboot, and submit a collector row with `--ane-smoke` when the machine is idle. The machine-parsed lines are unchanged.
- Data-only SoCs: `data/ane-soc/SOC.json` keeps cited ANE data for a chip that no driver binds; `tools/validate_ane_soc.py` is its schema. `packaging/build-dtbo` installs the data as `/usr/share/omarchy-ane/soc/SOC.json` and compiles, but never installs, a data-only overlay (`packaging/dt/SOC-ane-dataonly.dts`, root property `omarchy,data-only`). `omarchy-ane-dt` never applies one, opt-in key or not. Its `status` adds `data-only (no driver): SOC`, and `omarchy-ane-check` prints `DATA-ONLY SoC: SOC (no driver yet)`. `tools/gen_coverage_table.py` writes the README data-only table. CI: the `ane-soc-data` job, the data-only overlays in `tools/test_ane_overlays.py`, and the separate `aurora-dtbs` workflow for the M3 and later board device trees. See `docs/ane-soc-data.md`.
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