# T6021 ANE window 3: dtrace of the ANE operating-point write path (round-2 E1; prepared, not run)

Round 2 (`receipts/2026-10-03-ane-clock-round2/`) showed that every cold P6' run on macOS 27 starts at
about 4.0 ms per call and steps down through seven plateaus (3.911, 2.707, 2.107, 1.770, 1.541, 1.345,
1.220 ms) that fit the ADT ladder `voltage-states8` (600..2100 MHz) as t = 0.117 ms + 2250 ms·MHz / f.
Linux sits at the bottom of that ladder. The window-2 words are static configuration, so the selector
is elsewhere. The live 26A428 fbt listing names a path: `clpc::ane::DVFManager::requestPerformanceChange`,
`PMCVoterInterface::setANEPerfStateFloor`, `ApplePMGR::writeANEClkGenReg` / `waitANEClkGenReg` /
`enableFANE`, `ApplePMC::setPerfStateAgent`. Window 2's dtrace probed none of them: its generator read the
probe kind from field 5, and the listing puts a demangled `[...]` name with spaces before the kind, so every
PMGR line was skipped (10 clauses instead of 188; fixed in `w2sample.py` on this branch).

This window traces that path, read-only, during three cold P6' ramps. No register range is read, no
kext is built or loaded, nothing is written.

## What runs (macos-bundle/macos_window3.sh, ~2 min on-box)

1. `base`: window-1 `env inputs` (identity, csrutil, sudo probe, stage manifest).
2. `w3env`: `dtrace -l -P fbt` of this kernel, then `w3gen.py` builds `w3.d` from that listing. Targets
   are matched by regex on the mangled name, `.cold.N` split-offs excluded, so a changed build still
   matches or is listed as absent in `probes.json`. The script is dry-compiled with `dtrace -e`; on failure
   it is regenerated without the `proc` clause, then without `stack()`; if it still fails the window
   records `SKIPPED` and continues.
3. `dtrace`: trace running, 10 s idle, three spawns of the window-2 P6' runner (3000 calls, no warm-up,
   so call 1 is cold) 6 s apart, 10 s idle, SIGINT. The runner spawn/exit times go to `run/events.tsv`
   (w2sample.py, CLOCK_REALTIME ns, the clock dtrace's `walltimestamp` uses), per-call ms to `runner-N.json`.
4. `sums`.

Probes and output format (`w3gen.py`):

| tag | functions | lines |
|---|---|---|
| CLPC | `clpc::ane::DVFManager::{requestPerformanceChange, enable}`, `JobTracking::getActiveJobRequestedOperatingState`, `PMCVoterInterface::{setANEPerfStateFloor, writePerfStateFloorReg, writeRegValue}` | `E` entry with a0..a5 and `stack(3)`, `R` return value |
| PMGR | `ApplePMGR::{readANEClkGenReg, waitANEClkGenReg, enableFANE, _setPerfState, _handlePerfStateRequest, _handleSOCPerfStateRequest, _setDeviceDVFSState, _sendPMPCommand}`, `ApplePMGRv2::{_handlePerfStateRequest, _setDeviceDVFSState, _sendPMPCommand}`, `tracePerfStateChange`, `ApplePMC::setPerfStateAgent`, `AppleT6020PMGR::{setPerfState, getFANEIndex}` | same |
| writes | `ApplePMGR::writeANEClkGenReg`, `ApplePMGR::writeReg32`, `AppleT6020PMGR::writeReg32`, `ApplePMC::writeReg32` | `W` per event with a0..a4 plus `stack(3)` when inside an ANE-path gate on the same thread; `writeReg32` per-event lines stop after 50,000 outside a gate; every write is aggregated by (function, ramp, a1, a2, a3) with count, first and last timestamp (`A` lines at END) |
| anchors | `clpc::CLPC::aneWorkBegin/End` | `B` per call |

For a C++ member function `arg0` is `this`. `ApplePMGR::writeReg32(RegMap, uint, uint, uint)` therefore
carries a1 = RegMap, a2 = offset, a3 = value, a4 = die; the T6001 capture of 2026-09-25 (a1=0 a2=0x18014
a3=0xf) confirms that layout. The gate is a per-thread depth counter set by entry/return of the ANE-path
functions that have both probes. On 26A428 six return probes do not exist (`writeANEClkGenReg`,
`readANEClkGenReg`, `waitANEClkGenReg`, and the three `ApplePMGRv2` methods): those functions print on
entry only and never touch the gate, and `readANEClkGenReg`'s return value is not capturable by fbt.
`proc:::exec-success` of `ane_inmem_run` numbers the ramps for the aggregation. Bounds: bufsize 64m,
switchrate 20 Hz, aggsize 16m, `tick-240s` exit.

## Analysis (analyze_w3.py)

    python3 analyze_w3.py --synthetic
    python3 analyze_w3.py --trace out/w3/trace.out --events out/w3/run/events.tsv \
        --runner-json out/w3/run/runner-1.json out/w3/run/runner-2.json out/w3/run/runner-3.json [--json r.json]

Per ramp (spawn to exit): every write in time order with t relative to the ramp's first `aneWorkBegin`,
its call index (the `aneWorkBegin` count so far), the plateau level of that call and of the next call
(a write during call k shows in call k+1), the PA from the RegMap table, and the top stack frames.
Per (function, RegMap, offset): the distinct values in first-seen order and the plateau levels at their
first writes. The decision rule is the round-2 one:

- PASS: exactly one (function, RegMap, offset) wrote >= 5 distinct values in monotonic order inside a ramp.
- STOP: no AP-side write inside any ramp (the op point is coprocessor- or firmware-owned).
- UNRESOLVED: writes exist but no single family qualifies, or more than one does.
- INVALID: no runner spawn/exit pair, or no `aneWorkBegin` anchor inside any ramp.

`pmgr-regmap-26A428.json` is the pmgr `IODeviceMemory` list from window 2's IOService capture (73
entries). It equals the ADT pmgr reg list except entries 59-62 (bridge-translated). RegMap = reg index is
the T6001 precedent (map113) and is INFERENCE here until a fabric-ps write at RegMap 40 confirms it.

The self-test (`--synthetic`) covers PASS (RegMap 41 offset 0xa04 -> PA 0x285868a04, six values, levels
1..6), STOP, UNRESOLVED (two families), a write outside the ramps, stack attachment and the aggregates.

## Staging and the window

    E1=<e1-probes> P6FIX=<P6Fix e1-p6overfix> INMEM_BIN=<ane_inmem_run 2bbc237b> OUT=<stage> bash ct/stage_w3.sh

The stage is the window-1 `prepare_stage.py` output (P6, P7, P6', the pinned runner; no encoder) plus
`macos_window3.sh`, `w3gen.py` and the window-2 `w2sample.py`; all are in `stage_manifest.sha256`.
Entry, return, fetch and clean reuse window 2 unchanged:
`receipts/2026-10-03-ane-macos-window2/ct/{reboot-mac.sh, return-linux.sh, fetch.sh}` and
`m2/prereboot-mac.sh`, with the same rules: one owner, BootNext only (default boot stays Omarchy), ssh
identity never ping, the 6-minute no-answer rule on both reboots, every macOS ssh under `timeout`, no hard
reset, scratch removed with python rmtree. On the Mac:

    SCRATCH=<scratch> caffeinate -dimsu bash macos_window3.sh      # one pass, then fetch, then clean

Expected on-box time is about 2 minutes; the window is two reboots plus notices, as window 2.

## Risks

- fbt on the hot `writeReg32` paths adds overhead and may slow the ramp (T6001 saw 913 writes per
  domain-8 gate). It changes no state; the per-call ms in `runner-N.json` show the effect.
- Six return probes are absent on 26A428; the entry arguments still name the register and value.
- `stack(3)` output is multi-line; the parser attaches indented lines to the preceding event.
- The per-event cap (50,000 outside a gate) and the 64 MB buffer bound the output; the aggregates keep
  every write regardless.
- dtrace is not installed on the operator host, so `w3.d` has only been generated and inspected, not
  compiled; the on-box dry compile and its two fallbacks cover that.
