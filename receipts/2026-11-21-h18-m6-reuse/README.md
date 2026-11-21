# M6 (T8152) ANE: program loaded once, and the module released without removing the overlay (2026-11-21)

The M6 runs the out-of-tree H18 bring-up source, not this repository's main. `m6-bringup-tree.diff` is the patch applied to that tree (base ane_h18_main.c sha256 4c23481673c57ed40388972c856b433d2bfdc196aaf5bfa7696c5aee9d334dca, ane_h18_dt.c sha256 6311f60260863cb8d9a41aea2378ee9fcb44c97712909a638e0b6ad65b8c4d85). It contains:
- `hs_prog_reuse` and one output digest per call (same change as this PR's main commits).
- t8152 overlays take no module reference and refuse `remove` (-EPERM). The PSD power-domain driver has no `.remove`, so removing its nodes leaves a live genpd whose power-off work Oopses (`apple_psd_request`). The overlay stays applied until reboot, and the next load binds the existing ANE node.

Test, red first: `test_dt_reuse.c` (RU1-RU7; RU3, RU4 and RU6 failed before the dt change). `reuse-check.py` reads a run's kernel log (tests: `tests/test_reuse_check.py`, 6 cases).

| run | result |
|---|---|
| load once, 1000 calls (first boot) | 1000/1000, median 548.3 us, p99 582.8 us, digests equal, lanes exact |
| cycle 1 on a fresh boot: insmod, apply, 1000 calls, rmmod (no overlay remove) | 1000/1000, median 549.2 us, p99 715.8 us; rmmod unloaded the module; no Oops |
| cycle 2: insmod again, binds the existing node | bind OK, no Oops, but the second firmware start on the same boot failed: `handshake result -19`, `rtkit-shutdown-incomplete`, lifecycle retained (module pinned until reboot) |

Open: a second ANE firmware boot within one OS boot. The reload path is safe now (no Oops), but it is not usable yet.
