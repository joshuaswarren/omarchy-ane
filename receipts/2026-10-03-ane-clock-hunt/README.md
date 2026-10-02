# T6021 ANE clock hunt: the ladder is known, the selector is not (2026-10-03)

Offline analysis of the 2026-10-02 macOS gap-window capture on the M2 Max (T6021, macOS 27.0 26A428).
No hardware was touched for this receipt. The capture method and timings are in
`receipts/2026-10-02-ane-macos-gap-window/` on `agent/ane-macos-gap-window-result`.

That window measured the compute-bound probe P6' at 1.180 ms on macOS and 5.465 ms raw / 4.415 ms
corrected on Linux, a ratio of 3.74. It also measured fabric-ps at 0x666 on macOS and 0x555 on Linux.
This receipt asks what in the live device tree defines and selects the ANE clock.

Decoder: `ioreg_adt.py` (this directory). It parses an `ioreg -l -w0` device-tree text dump, using the
m1n1 `proxyclient/m1n1/adt.py` pmgr record layouts. Its `--selfcheck` reproduces three independent
anchors:

- The ANE power-state word offsets that the capture tool already gates on.
- The E-cluster frequency ladder that `powermetrics` printed on the same boot.
- The ANE ladder.

## Measured (live T6021 device tree)

- `pmgr/perf-domains` record 4 is `ANE`, with byte 0 = 8. Byte 0 is the `voltage-statesN` index: for
  ECPU it gives 912…2424 MHz and for PCPU 702…3504 MHz, the ladders powermetrics prints. So the ANE
  ladder is `voltage-states8`: 600/593, 852/618, 1104/684, 1356/740, 1596/793, 1848/840, 2100/906
  (MHz/mV), 7 states. The T6001 tree has the same record, with 300…1500 MHz in 6 states.
- The ANE has two PLLs (`PLL_ANE0_OFF`, `PLL_ANE1_OFF`, pmgr perf block 1). The ane0 `clock-ids` point
  at boot-clock words 0x29e24033c…0x29e240348: type 0x33, frequency field 0, the same form as the GPU's
  DVFS clock.
- pmgr events `ANE0_ADCLK_TRG`, `ANE0_DITHR_TRG` and `ANE0_EXT_TRG0..2` live in `perf-regs[2]` =
  0x28e0e8000. T6001 has the same block at 0x28e0f8000.
- CLPC's `function-ane_perf_ctr` targets the ane0 node (phandle 0x169, 'ANCR').
- The PMP `dvfs-domain` table lists DCS, FAB, AFR, SOC0, DISP0, AVEMSR0 and DISP1. It has **no ANE
  domain**, unlike T6001, which lists SOC0_ANE_SYS. The pmgr also has no T6001-style DVFS_CMD page (T6001
  reg[113] 0x400004000).
- The live ANE perf state was not captured on either OS. powermetrics gives ANE power only
  (9.0-9.1 W under P6). The 16 KiB pmgr power-state window changes only in ANE power words between idle
  and load, and Linux holds the same ANE power words (0x1f0003ff / 0x3ff).

## Result

- **The selecting register is not identified.** The 13.5 static read says the T6020 pmgr rejects ANE
  domain 8 (`docs/t6021-ane-bringup-findings.md`, section 19). The equivalent T6001 static read was
  overturned by a live capture, so the macOS 27 behaviour on T6021 is open.
- The decisive capture is a macOS 27 dtrace of `ApplePMGR::_setPerfState` / `writeReg32` during a P6'
  loop. This is the method that decoded the T6001 write set. It costs one macOS window, needs no kext
  rebuild, and SIP is already off. If it shows no AP write for domain 8, the op point is owned by a
  coprocessor, and the PMP route stays parked.
- Arithmetic only: a pure-clock reading of r(P6') = 3.74 puts the Linux ANE at no more than 561 MHz
  (2100/3.741), below the 600 MHz bottom state. A boot clock under the managed ladder fits every number.
- **fabric-ps.** macOS holds 0x666 at idle and under load, and Linux holds 0x555.
  - Sources: Linux's `apple-pmgr-misc` keeps the probe-time DESIRED[3:0] and writes it only on
    suspend/resume. m1n1 never touches the word. The PMP owns a FAB DVFS domain on macOS, and the PMP is
    disabled under Linux.
  - Inference: iBoot leaves 5 and the macOS PMP raises it. Not traced.
  - Gated write: DESIRED-only RMW to 6 (the kernel's own op), with a whole-word readback. Never write
    [11:4]. The A/B covers P7, the encoder and P6'.
  - Falsifier: readback 0x666 with all three probes within ±1%.
- Excluded from every probe: the pmgr window at 0x285868000 and the ANE0 DPE word at 0x285910108. Both
  are inside the ANE fatal-read window (`ane/t6021/ane_t6021.h`).

## Not verified

- The slot stride inside perf blocks (base + 0x100 + idx·0x10 is an inference).
- The meaning of boot-clock type 0x33.
- The function of the ane0 SET window on T6021.
- The fabric-ps [7:4]/[11:8] fields.
- Who writes fabric-ps = 6 on macOS.
- Any live value of the ANE perf block or the clock words.
