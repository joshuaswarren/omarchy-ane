# T6021 ANE clock, round 2: macOS climbs a 7-state ladder per job; Linux stays at the bottom (2026-10-03)

Offline analysis of the second macOS 27.0 (26A428) window on the M2 Max (T6021). Bundle and probes:
`receipts/2026-10-03-ane-macos-window2/` on `agent/ane-macos-window2` (8d720d5). No hardware was touched for this
receipt. Round 1: `receipts/2026-10-03-ane-clock-hunt/` on `agent/ane-clock-hunt`.

## Result

**macOS P6' cold ramps.** Every cold P6' run on macOS starts at about 4.0 ms per call. It then steps down to
1.22-1.31 ms within 3-37 calls. One run (ramp-2) shows seven plateaus: 3.911, 2.707, 2.107, 1.770, 1.541, 1.345
and 1.220 ms. They fit the device tree's seven-state ANE ladder (`voltage-states8`, 600-2100 MHz) as
t = 0.117 ms + 2250 ms·MHz / f, rms 0.034 ms (`fit_ladder.py`). The one-to-one assignment of plateaus to states is
an inference; neither OS exposes a clock readout.

**Linux.** P6' takes 4.415 ms (after the 1.05 ms call-settle correction). That is 1.13x macOS's own bottom-state
call, an effective 523 MHz. Linux behaves like an ANE left at the bottom of the ladder.

**What does not set the clock:**

- The 21 pmgr words read in window 2 (clock-id words, PLL and device perf slots, CLVR and DVFM trigger slots) are
  constant on macOS, including the first 200 ms after the ANE powers up, while the clock climbs. They read 0 on
  Linux. They look like static configuration (PLL off-mode, CLVR voltage trigger, adaptive-clock and dither setup).
  They could be prerequisites for a clock change; they are not the selector.
- Fabric and DCS power states on Linux (0x555/0x999) sit inside the range macOS cycles through. A fabric 5→6 write
  under Linux moved nothing by more than 0.6% (encoder, P7, P6'). The ANE sped up while DCS stepped down.
- The ANE sys clock assertion in AppleH11ANEInterface 10.19.2 is a reference count on the ANE_SYS clock enable, with
  a power-gating timer behind it. It turns the clock on and off; it does not pick a frequency.
- The kext's "FW perf mode" is one firmware property write at init (0x10aa = 1), which our driver already sends.
  No per-job perf command exists.

**Where the selector most likely is.** The live 26A428 probe list names the path: CLPC `ane::DVFManager::
requestPerformanceChange` → `pmgr::PMCVoterInterface::setANEPerfStateFloor` / ApplePMGR `writeANEClkGenReg`,
`waitANEClkGenReg`, `enableFANE` → `ApplePMC::setPerfStateAgent`. The window-2 dtrace probed none of them.

pmgr maps an ANE-engine page at engine+0x1868000 (ADT pmgr reg[41]). That is the page where the T8103 PMGR writes
its ANE clock-gating word. It is a likely home for the ANE clock generator. It lies inside the engine range that
Linux must not read.

## Next (ranked; details in the lab report)

1. A macOS dtrace of the functions above during cold P6' ramps. Read-only, one macOS window, no kext build.
2. A guarded Linux read of the word or words it names.
3. A one-word write of macOS's first rung above the bottom (852 MHz). Predicted: P6' 4.4 → about 2.8 ms.

Every write is gated, uses only values macOS was seen to write, and has a revert path and a reboot fallback.

## Tools (this directory)

| Tool | What it does |
|---|---|
| `fit_ladder.py` | Fits the plateaus to the ladder and places Linux on it. |
| `ramp_levels.py` | Plateaus of every macOS run. |
| `ramp_words.py` | Window-2 word values right after the ANE powers up. Needs `w2sample.py` from the window-2 bundle. |
| `kext_fmt.py`, `kext_xref.py`, `kext_calls.py` | Read-only LIEF + capstone helpers for os_log strings, string and call cross-references, and per-function call lists in a kext. |
