# Handoff summary for the M2 GPU lane (w6Z) — agx_stats M2 validation

PR #157 (`agent/jw16-agx-stats2`) exports the AGX firmware stats the driver
already polls as `/sys/class/drm/card*/device/agx_stats` (`busy_ns jobs pstate
power_mw util1..4 temperature_*`, mode 0444, producer contract for coreglass
GPU busy). Offline prep is done: the series does NOT compile as shipped —
`DEVICE_ATTR_RO(agx_stats)` needs `agx_stats_show`, the code says
`asahi_agx_stats_show`; fix branch `agent/jw16-agx-stats3` (commit `19291a93f142`,
normal push on joshuaswarren/aurorasilicon-linux) renames it, adds prototypes
and the missing trailing newlines, and cross-builds W=1 clean with `asahi.ko`
receipts on both the M2 `7.1.13-3-1` config and the aurora config (see
`builds.md`). Module-only gating is impossible — the stock M2 kernel has
`CONFIG_DRM_ASAHI=y` — so the window is a ONE-SHOT KERNEL BOOT of the fix-branch
build (own release `7.1.12-ARCH-agxstats`, own modules dir, stock GRUB default
untouched). Window: about 100-120 minutes, 3 reboots (export-on boot, 
`asahi.stats_export=0` boot — the param has no sysfs file, the command line is
the only switch — and the stock return), 12-minute notices, 6-minute no-return
rule, ESP writes only behind Main's GO file, rollback = reboot + the bundled
`remove-test-kernel.sh`. Gates, thresholds and every command:
`receipts/2026-10-04-agx-stats-m2/README.md`. Known deviations to expect, not
tune: `jobs` stays 0 (no call site in the series), T6021 `FwBusy` timestamp
units are unvalidated (S1/S2 measure exactly that), and the decode A/B gate is
0.5% between export-on, reader-loaded, and export-off arms at identical
digests. Remaining pre-merge work for the branch owner: all six commits need
Signed-off-by, and the ABI doc overpromises (`temperature_tmin`/`tmax` keys the
code never prints; a saturation claim the code does not implement).
