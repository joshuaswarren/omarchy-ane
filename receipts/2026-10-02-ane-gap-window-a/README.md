# 2026-10-02 — GapWinA: T6021 ANE gap window A (Linux leg)

M2 Max (jw14m2-linux, T6021/j414c), stock 7.1.13-3-1-ARCH, boot bin 62ba3010 untouched.
Module under test: ane_t6021.ko 584e7ac2 (agent/ane-dsid-probe @ 76670f6, default params off,
srcversion 4FE12EFABEDAACF4DF38AA8), installed in updates/ for the window; hand module af2cee6c
restored after. Boot 9bb646d3 for all timed work.

Pre-registration: ~/.local/share/apple-silicon-lab/entries/GapWinA/20261002T093312Z-jw14m2-linux-gap-window-a.md
(thresholds from artifacts/GapRank/report.json E1/E2). Companion notebook entry holds the dated
observations; this README is the repo-side summary. Notebook is not committed.

## Result summary (details + evidence in the notebook entry and runs/)

- Baseline (20 blocks x 16 calls, whole encoder, all bit-exact fca96f13...): min-of-min
  254.26 ms, median-of-medians 254.4115 ms raw (253.21/253.36 settle-corrected). Anchor PASS.
- E1 Linux leg (inputs for the later macOS ratio): P6 conv64 5.468 ms raw / 4.418 corrected =
  1.944 TMAC/s (output overflows fp16 — 257,748/262,144 inf lanes; timing valid, accuracy flagged
  probe-invalid); P7 add32m 4.120 ms raw / 3.070 corrected = 32.8 GB/s (relL2 0.000269, valid).
  trace_td not writable at runtime -> skipped per protocol.
- cpufreq sweep (7 pinned points, readback-verified, 20 ms sampler): encoder flat 254.25-254.47 ms
  across P-cluster 702 -> 3264 MHz; lowest/highest ratio 1.0001 (median), Pearson r = -0.019.
  CPU p-state coupling REJECTED on T6021 across the full OPP range. ane_boost is not in this
  module (strings 0 hits; H13 ane.ko not loaded) — no QoS floor, nothing to toggle. Originals
  restored with readback.
- E2 contention (ABABAB, memcpy hog 46.3 GB/s median <= 15% of spec): encoder B/A 1.0001 per pair,
  matvec 2048x5120 B/A 1.0006 -> DMA-arbitration loss REJECTED on both (GapRank lines: >= 1.15
  support, <= 1.03 reject). A-arm drift 0.000%.
- Probes (dcs-ps 0x28e20c000/0x28e20c400, dsid-tm 0x285c2046c) + restore: see the notebook entry
  post-gap sections (probes ran after the released gap; restore reboot returns af2cee6c).

## Invalid attempts kept as evidence

- s0-20261002T100240Z, s0-20261002T100800Z: psi() parse bug (idle_gate false NOT-IDLE).
- s1-20261002T102429Z, 103205Z: set -u script crashes (no device state touched).
- s2-20261002T103542Z: pin path bug — sweep ran unpinned (INVALID; writes never landed).
- s3-20261002T105134Z/105203Z/105328Z/110929Z: matvec gate staging failures (fixture/args/weights
  form). Root cause of the last: the ANEC's weight blob is weights.bin verbatim (128-byte header
  included; ane-run --check matvec reads w[col*K+k] from byte 0), the M2Qualify-passed form.

## Layout

- scripts/ — common.sh, s0_baseline.sh, s1_e1.sh, s2_sweep.sh, s3_e2.sh, hog.c, s4_probes.sh,
  s5_restore.sh, labstate.sh, prereboot.sh, reboot.sh (bytes pushed to M2 /var/tmp/gapwin/bin/)
- runs/ — per-step run dirs: console.log, blocks.tsv, state-*, dmesg-new.txt, freq samples,
  originals/restored, hog logs, gate logs, labstate-pre; probes + restore dirs appended post-gap
- analysis.py + analysis-output.txt — deterministic re-parse of runs/ (python3 analysis.py runs/)
