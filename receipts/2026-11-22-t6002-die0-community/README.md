# T6002 (M1 Ultra) die-0 qualification — community pass

Source: mijuny, omarchy-ane#304 comment, 2026-11-22. Machine: Mac Studio 2022,
M1 Ultra (Mac13,2, j375d), 64 GB, Asahi Alarm Minimal (BTRFS) + Omarchy
4.5.0-3, no omarchy-mac-boot, kernel 7.2.12-3-1-ARCH, omarchy-ane-dkms 0.5.13-1.
This is the second die-0 pass; row 0846cbcc5337 promoted the chip earlier the
same day (80c46b2).

- `omarchy-ane-check`: ready — node `apple,t6000-ane` from the overlay,
  ane.ko 0.5.13 bound to `285c04000.ane`, `/dev/accel/accel0` open.
- `omarchy-ane-smoke` x11: 220/220 calls bit-exact against golden sha256
  `5ad7eccd2977a88a375f123fe42c6780855953a1e9df4ac442cae383bc240dd6`,
  medians 0.057-0.069 ms.
- `collect_deep.py --ane-smoke` (omarchy-mlx c15a0d4): 20/20, errors 0.
- Stress, packaged H13 add fixture with fresh random inputs per call:
  5,000 calls exact, then 171,023 calls in 180 s exact
  (median 0.059 ms, p99 0.080 ms); a corrupted reference was flagged.
- Kernel log this boot: `loaded ane`, no ANE or DART fault lines.
- Runbook stages 0 and 1 pass (`docs/ultra-test-runbook.md`). Die 1 not
  enabled — no die-1 qualification command exists yet.
- Scope: one boot on one machine.

## Reporter's opt-in note (resolved)

mijuny observed that the installed `t6002/omarchy-ane.dtbo` (0.5.13-1)
carries no `omarchy,opt-in` key while #304 and the README called die 0
opt-in. Resolved: the promotion to on-by-default (80c46b2, row
0846cbcc5337) predates the install, so the missing key is correct; the
stale piece was the "Opt in, untested" bullet and the issue table, not the
overlay.
