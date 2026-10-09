# T6021 section-release device check (ticket plan; not executed yet)

Status: PLAN. This ticket has not run on any machine. The M2 slot and the
build belong to the gpu-turn owner (w7J); the code change is reviewed in
PR agent/t6021-section-release (w7K reviews the libane userspace contract;
no kernel module change is involved, so no module-install review is
needed — the installed `ane_t6021` (37ffb57) already has the dedup
semantics this fix relies on).

## Why

Measured on boot 2105990f (2026-10-09, receipts
`/var/tmp/qwen-decode/ab-20261009T134205Z/`): with `bo_total_bytes` at 0,
the resident arm's first configure loaded all 38 programs (their sections
became the held `fw_ref` floor, 2,754,388,032 B), and the second
configure (per-prompt) failed at the 7th load:
`REFUSE: prog_006: session refused LOAD: ERR LOAD prog_006
device-open-failed`, `DRM_IOCTL_ANE_BO_INIT failed for 222980416 bytes`,
`bo_total_bytes` after: 2,757,607,424. The per-call arm passed on the
same boot. Cause and fix: libane kept a duplicate of every loaded
program's section BOs alive for the life of each nn; the fix releases
them after `PROG_LOAD` + `PROC_CREATE` (libane/ane_m2.c, this branch).

## Preconditions

- M2, kernel 7.1.12-2-12.6-sep-ARCH, module `ane_t6021` loaded, no
  reboot, no module reload at any point.
- gpu-turn slot held (`flock /var/tmp/ane-run.lock`); the script refuses
  to run when the lock is busy.
- Tools built from this branch into `/var/tmp` (userspace only:
  `cc`, no package installs): `ane-session`, `ane-run`,
  `tools/qwen_m2_decode.py` (the branch's decode is the same as
  `agent/qwen-resident` 62972b0 for the resident path).
- The staged assets the A/B used: `/var/tmp/qwen-decode/manifest.json`,
  `/var/tmp/qwen-real-anec-h14/`, `/var/tmp/qwen-conform-0e2c3743-r2/`.
- Read `bo_total_bytes` only from sysfs; no dmesg loops.

## Run

    sudo -n sh receipts/2026-10-09-t6021-section-release/m2-section-release-check.sh

(adjust env overrides as needed; the script prints every
`bo_total_bytes` reading and tees them to its output dir). `sudo -n` is
needed only if the sysfs parameter and the lock file are root-owned; run
without sudo when the session user can read the parameter.

## Pass criteria (with the fix)

1. Stage 1 (K loads of prog_006 in one session, FREE between loads):
   `T1 - T0` equals prog_006's section set (the `fw_ref` floor, about
   230 MB), not K times that.
2. Stage 2 (resident decode, two prompts -> three configure passes):
   the decode completes (rc 0); no `BO_INIT failed` line; `T2` sits at
   the 38-program floor (2,754,388,032 B) plus a small parked-io
   residue, matching the A/B's per-call end state (2,760,491,008 B on
   boot 2105990f).
3. A second back-to-back stage-2 run completes with no reboot and
   `bo_total_bytes` returns to the same floor.

## Fail signatures (without the fix)

- Stage 2's second configure: `REFUSE: prog_006 ... device-open-failed`
  with `DRM_IOCTL_ANE_BO_INIT failed for 222980416 bytes`, decode rc 2.
- The host regression already proves the same discrimination:
  `tools/test_libane_ioctl` "resident pattern" passes on this branch and
  fails on 76d620b (client 0 holds 6 section BOs; held 540,672 vs floor
  196,608).

## Receipts

Attach the script's output dir (bo_total.tsv, session.out/err,
resident.log) and the `SHA256SUMS` of the built tools to the notebook
entry `entries/SectionRelease/20261009T134552Z-ct-section-release.md`.
