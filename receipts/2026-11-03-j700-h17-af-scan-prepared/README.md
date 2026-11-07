# Corrected cold-boot AF sample: preparation only

Files: `ane/h17/ane_h17_af_scan.c`, `run-af-scan.py`,
`prepare-af-scan.md`. The latter records exact descriptor bytes,
instruction addresses and the retained artifact SHA-256.

The previous full probe confused descriptor references with register
offsets. `0x11378b8` is a descriptor file offset; its first word
`0x40000000` encodes width 4 and engine offset 0. The two-register sample
uses offsets 0 and 0x338. Prior writes at engine+0x1137xxx do not establish
anything about the actual AF configuration registers or clock state.

Checks performed without hardware access:

- `make -C ane/h17 KERNELDIR=/lib/modules/7.2.11-14-1-ARCH/build`:
  built `ane_h17_af_scan.ko`; kernel headers emitted attribute/compiler
  option diagnostics. Build log: `/var/tmp/opencode/af-scan-build.log`.
- `python3 ane/h17/run-af-scan.py`: preparation-only output, no load.
- `python3 -m py_compile ane/h17/run-af-scan.py`: passed.
- `git diff --check`: passed.
- Module vermagic: `7.2.11-14-1-ARCH SMP preempt mod_unload aarch64`.
- Module SHA-256:
  `3aacc5c33b5406c2cff1f6b63ec798d45b7fd5b2e17b2b9833053e5a9f61ec10`.

No claim of device qualification. Source-backed configuration fields may
still fault on this Linux power sequence. The experiment reads the
genpd-only state, does not initialize the engine, and cannot prove a
clock fault from zero values. Power votes and release still change
hardware. No module was inserted during preparation.
