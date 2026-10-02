# T6021 ANE window 2: where macOS holds the ANE clock operating point (prepared, not run)

Window 1 (receipts/2026-10-02-ane-macos-gap-window/result) measured the compute-bound P6' at
1.180 ms on macOS 27 and 5.465 ms under Linux, a ratio of 3.74. The ADT's ANE ladder (`voltage-states8`)
runs from 600 to 2100 MHz, and the Linux number sits at or below its floor. No capture names the
register that selects the operating point. This window reads every ADT-named candidate word,
read-only, on macOS idle, across a P6' ramp-up and ramp-down, and mid-load. It adds the live
IOService plane, powermetrics, a log stream and, as the last and optional pass, a dtrace of the
ANE perf path. `ane/t6021/probes/ane_clk_twin_probe.c` reads the same words on Linux.

Window 1 already shows that the operating point ramps. `analyze_w2.py --runner-json` on its P6'
blocks gives these values. In 12 of 20 blocks, the first timed call took 2.61-2.81 ms, which is
2.08-2.18x the 1.26 ms steady state. Steady state followed after a median of 4.5 ms and at most
26.5 ms of ANE time. The ramp is millisecond-scale, so this bundle samples with a new CLI loop
(about 2 ms per dump) instead of one process per dump. The runner here does no warm-up, so its call
1 is the cold start.

## Words (ranges-w2.txt; same order = the Linux twin's mask bits)

Every address comes from the live macOS 27 ADT decode (AneClockHunt `t6021-macos27-decode.txt`).

| bit | name | PA | why | read risk |
|---|---|---|---|---|
| 0-1 | fabric-ps, dcs-ps | 0x28e20c000, 0x28e20c400 | pmgr reg[40] CLKGEN words (DT pmgr-misc); macOS 0x666/0x999 | proven: read on macOS (window 1) and on Linux (GapWinA/B, FabricDsidAB) |
| 2-5 | set-000/030/034/038 | 0x28e08c000 +0/+0x30/+0x34/+0x38 | ane0 reg[2] SET window; the 13.5 kext writes 0x80000000 to these four before bring-up | low-medium: Linux read +0 and +0x8..+0x38 once (2026-09-21, values 0/0x80000000). Never read on macOS. On T6001 a read past SET+0x38 hard-reset the box, so these reads stop at +0x38. SET writes external-abort; nothing here writes |
| 6-9 | clk62-33c..clk65-348 | 0x29e24033c..0x29e240348 | ane0 `clock-ids` 0x13e-0x141 = boot clocks 62-65, type 0x33, pmgr reg[5] | unknown: never read on either OS. m1n1 does not read type 0x33. No Linux DT window covers it |
| 10-11 | pll-ane0, pll-ane1 | 0x28e0e03b0, 0x28e0e03c0 | clocks PLL_ANE0_OFF/PLL_ANE1_OFF, perf-regs[1] idx 0x2b/0x2c | unknown, forbidden-class by analogy (FIND §19: ANE perf regions on T6001/T8103). Slot stride (base+0x100+idx*0x10) is INFERENCE |
| 12-13 | dev-anesys, dev-anecpu | 0x28e0e0360, 0x28e0e0370 | devices ANE_SYS (flags perf+notify_pmp) and ANE_CPU perf slots 1:0x26/0x27 | same as bits 10-11 |
| 14 | ev-clvr-ane | 0x28e0e0b80 | event CLVR_EXT_ANE 1:0xa8 | same; an event slot, so read side effects are unknown |
| 15-24 | ane0-adclk/dithr/ext0-2 (-0/-1) | 0x28e0e8100..0x28e0e8190 | perf-regs[2] (pmgr reg[0]+0x68000, size 0xa), events ANE0_ADCLK/DITHR/EXT_TRG0-2 ids 120-124 | unknown, forbidden-class by analogy; trigger slots, so read side effects are unknown |

Excluded: pmgr reg[41] 0x285868000 and the ANE DPE 0x285910108 (inside the ANE fatal-read window
0x285854000..0x285c04000, `ane/t6021/ane_t6021.h`), anything past SET+0x38, any block dump, and the
PMP SRAM.

Kext rules (frozen 932d3b9b; source unchanged since 14620db): `ane_req_acceptable` has no address
allowlist. It requires word alignment, length up to 16 MiB, at most 40 ranges, and no CoreSight
overlap. Engine-window ranges are forced gated. None of the 25 words is in the engine window
0x284000000-0x286000000, so all 25 are allowed as plain runtime data, with no rebuild and no Allow
click. Tier 2 (bits 2-24) is flagged `gated` anyway, so its first reads happen only while all eight
islands read ACTUAL=0xf. The live `pmap-io-ranges` lists none of these words. It also lists none of
the words window 1 read without trouble (fabric-ps, pmgr-ps), so that is no barrier.

## Phases (macos-bundle/macos_window2.sh)

Pass A (default, ~4 min on-box):

1. `base`: window 1's `env inputs` (identity, csrutil, sudo probe, stage manifest).
2. `w2env`: sysctl hw/machdep/kern, pmset (-g, therm, assertions), `powermetrics -h`, the kext sha
   and loaded state, and the CLI sha. A series self-test runs 3 dumps of the two proven words. On
   failure the bundle falls back to single dumps of the window-1 CLI (~/ane-cap3, 7ee61c0e).
3. `ioservice-idle`: `ioreg -l -p IOService -w0`, the class list, and `ioreg -r -a -l -c <class>`
   for every live class matching ANE|PMGR|CLPC|PerformanceController|PerfControl|ApplePMP. The
   25G83 kernelcache strings name H11ANEIn, AppleCLPC, AppleARMPerformanceControllerCLPCNub,
   ApplePMGR, IOPerfControlClient and the CLPC property `ANEFrequencies`.
4. `tier1-idle`: 25 dumps of the proven words plus island PS, 200 ms apart.
5. `first-set`, `first-clk`, `first-perf1`, `first-dvfm`: the first read of each tier-2 block, one
   gated dump under a P6' runner. Each step is printed to the console and synced first, so a reset
   names its block. A block joins later phases only if every word read status ok.
6. `logstream-start`: `log stream --level debug` (info if refused) as ndjson, with predicate
   `subsystem CONTAINS[c] "ane" OR process == "aned"` plus kernel senders H11ANE/PMGR/CLPC. No log
   configuration change.
7. `ramp`: three P6' runs. Each is 2 s idle, 3000 calls and 4 s ramp-down, with dumps every ~2 ms.
   It records runner spawn/exit and per-call ms.
8. `load`: window 1's `loopload.sh` (P6 2000 / P7 500 alternating) with 20 dumps 0.5 s apart,
   `ioservice-load`, and `powermetrics --samplers ane_power,cpu_power,thermal,smc -i 200 -n 20`. If
   that sampler set is refused, every sampler that runs alone is used.
9. `logstream-stop`, `sums`.

Pass B (`PHASES="idle-raw sums"`, ~15 s): each cleared tier-2 block read once with no gate while the
ANE idles. This is the riskiest read in the window and is printed and synced first.

Pass C (`PHASES="dtrace sums"`, ~2 min, optional and fail-soft): `dtrace -l -P fbt`, then entry
clauses generated from that listing (PMGR writeReg32/PerfState/DVFS, CLPC ane*, H11ANE
perf/clock/freq/power), then one P6' run. As on the T6001 macOS windows 9-10, generating from the box's own
listing avoids a missing probe name stopping dtrace. Any failure leaves `w2/dtrace/SKIPPED` and the
pass exits 0.

## Staging list (ct/stage_w2.sh)

`p6/`, `p7/` (E1 manifests), `p6prime/` (P6Fix e1-p6overfix, manifest-checked), `ane_inmem_run`
(Studio 2bbc237b, pinned), `src/` (ane_inmem_run.m, macos-regdump incl. aneregdump.c with the series
mode), `regdump-build/aneregdump` (Studio build of this branch), `macos_window.sh`, `loopload.sh`,
`ranges-gapwin.txt`, `prepare_stage.py` (window 1), `macos_window2.sh`, `w2sample.py`,
`ranges-w2.txt`. Every executed file is in `stage_manifest.sha256`, which the on-box `inputs` phase
checks. No encoder is staged.

## Runbook (entry and return as window 1; ~60 min including two reboots)

Aliases are the operator's ssh config: LINUX = the M2 Linux, MACS = the macOS Tailscale and LAN
aliases, and the macOS wired route as the third option. Liveness is checked by ssh identity only,
never by ping. Every macOS ssh runs under `timeout`.

1. T-15: Linux pre-flight, read-only: boot id, uptime, module sha/srcversion, ESP boot.bin hash,
   locks, gpu-turn queue, dmesg BAD, and `labstate.sh` (window-1 notebook copy). Stage on the
   operator host with `ct/stage_w2.sh`. Copy `m2/prereboot-mac.sh` to the M2 `$M/bin/`.
2. T-10: notice to Main and the M2 queue owner ("macOS reboot not before T0").
3. T0: `ct/reboot-mac.sh` queues `prereboot-mac.sh` in its own gpu-turn ticket. That script checks
   the ESP hash, takes the locks, runs `asahi-bless -n --set-boot-macos -y` (BootNext only), reads
   back default Omarchy / next Macintosh HD, then sync, 40 s, sync, and `systemctl reboot`. **6-minute
   rule:** if no alias answers 6 min after T0, STOP and tell Main. A camera frame is read-only
   evidence. No hard reset.
4. macOS checks: identity (`sw_vers` 27.0 26A428, Mac14,5), disk > 20 GB, `pmset -g therm` clean.
   `staging.sh push` (window 1, both-end hashes).
5. Pass A under `caffeinate -dimsu`, console teed to the operator, then `ct/fetch.sh` (non-destructive,
   verified). Pass B, fetch. Pass C, fetch. Then `ct/fetch.sh clean` (python rmtree of the scratch).
6. Return: notice ~10 min ahead, then `ct/return-linux.sh <macos alias>` with OLD_BOOT = the T-15
   boot id. It runs `sudo reboot` from macOS (BootNext is consumed; the default Linux volume boots).
   The 6-minute rule applies again.
7. Linux post-verify: labstate diff against the pre-flight (identity only), ESP hash, module bound,
   dmesg BAD 0, and one 16-call encoder smoke block bit-exact (fca96f13). Then "M2 FREE".

Later, the Linux twin: load `ane_clk_twin_probe.ko mask=<analysis linux_mask>` (cleared words only)
in its own ticket. Sample idle with `echo 20 > .../start`, then again during one encoder block. Feed
its dmesg lines to `analyze_w2.py --linux`. The module has no exit; a reboot unloads it.

## Analysis

    python3 analyze_w2.py --synthetic
    python3 analyze_w2.py --macos <fetched>/out [--linux dmesg.txt] [--pmgr-regs <DEC>] [--json r.json]

For each word the output gives the macOS values with the islands down and up, every value change
inside a ramp (ms after the first islands-up dump), the Linux values, and the flags LOAD-CHANGE /
RAMP-CHANGE / OS-DIFF. It also gives the ramp timing (cold call 1 to steady, spawn to power-on, exit
to power-off, dump cadence), IOService idle vs load diffs (freq/perf/state/volt/clock/power keys
first), powermetrics ANE lines, the log stream summary, and dtrace writeReg32 groups. The groups
decode RegMap to PA on the assumption, taken from T6001, that RegMap is the ADT pmgr reg index:
INFERENCE. The output ends with the Linux twin `mask=`.

## Risks

- A tier-2 first read can raise an SError. macOS then panics and reboots into the default Linux
  volume, and the pass's unfetched tail is lost. The console names the block. Every first read
  happens under load and gated, block by block, after the proven captures. The ungated idle reads
  are pass B.
- Window 1's mid-load engine-window reads coincided with one 104 s loopload round, against ~3.5 s
  rounds elsewhere. This bundle reads no engine-window range.
- The series mode is new, compile-checked but never run against the kext. The on-box self-test
  falls back to the proven CLI.
- dtrace probe names on 27.0, `smc` on Apple silicon, and `log stream --level debug` are each
  unverified. Every one has a fallback or a skip.
- The macOS load stays high after boot (18-27 in window 1). Every artifact carries its load stamp.
