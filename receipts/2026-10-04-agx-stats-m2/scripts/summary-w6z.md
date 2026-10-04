# Handoff summary for the M2 GPU lane (w6Z) — agx_stats M2 validation

The drm/asahi agx_stats series (replacement for draft PR #157: branch
`agent/jw16-agx-stats4`, 8 commits, every commit Signed-off-by, checkpatch
--strict 0 errors 0 checks) exports the AGX firmware stats the driver already
polls as `/sys/class/drm/card*/device/agx_stats` (`busy_ns jobs pstate
power_mw util1..4 temperature_raw temperature_scale`, mode 0444, producer
contract for coreglass GPU busy). `jobs` is now a real counter: bumped at
submission completion (`JobFence::command_complete`), so the M2 window checks
it rises through the decode cells instead of expecting 0. The original PR
#157 did not compile and its commits lacked Signed-off-by (not fixable without
a force-push, which we do not do) — hence the replacement. Offline prep is
done: module-only gating is impossible (stock M2 kernel has
`CONFIG_DRM_ASAHI=y`), so the window is a ONE-SHOT KERNEL BOOT of the stats4
build (own release `7.1.12-ARCH-agxstats+`, own modules dir, stock GRUB default
untouched). Window: about 100-120 minutes, 3 reboots (export-on boot,
`asahi.stats_export=0` boot — the param has no sysfs file, the command line is
the only switch — and the stock return), 12-minute notices, 6-minute no-return
rule, ESP writes only behind Main's GO file, rollback = reboot + the bundled
`remove-test-kernel.sh`. Gates, thresholds and every command:
`receipts/2026-10-04-agx-stats-m2/README.md`. Both configs build W=1 clean
with `asahi.ko` receipts and the host unit tests pass (see `builds.md`).
Known open item for the window: the T6021 `FwBusy` timestamp unit is
unvalidated — S1/S2 measure exactly that, and a refuted unit is a finding,
not a tuning target. The decode A/B gate is 0.5% between export-on,
reader-loaded, and export-off arms at identical digests.
