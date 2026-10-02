# T6021 ANE gap window: one macOS boot that decides the E1 clock question

Ready-to-run bundle for a single macOS window on the M2 (T6021) that captures the five missing
items discriminating the 254 ms (Linux) vs 89 ms (macOS) whole-encoder gap. Prepared 2026-10-02;
nothing here reboots or touches the M2 - the window runs when Main schedules it.

## What the window decides

E1 (GapRank H1, prior 0.45): is the ANE left at a low operating point under Linux?
Decision rule (pre-registered in the notebook): P6 (compute-bound, L2-resident weights) Linux/macOS
ratio >= 2.0 supports the low-clock hypothesis; <= 1.15 while the activation-bound arms carry the
ratio falsifies it. P7 (activation stream) separates a clock cause (P6 high, P7 similar) from a
memory-side cause (P7 >> P6, H3/H12 class). The Parakeet encoder is the reference arm (2.83 known).

## The five items and where they are captured

| # | Item | Phase(s) | Artifact |
|---|------|----------|----------|
| 1 | P6/P6'/P7 timings on macOS, same inputs | `timings` | `out/timings/{p6,p6prime,p7}/block-*.json` + `blocks.tsv` |
| 1b | Parakeet encoder, same procedure as the prior native run | `timings` | `out/timings/pk/` |
| 2 | fabric-ps 0x28e20c000 + dcs-ps 0x28e20c400 (4-byte words, DESIRED[3:0]) idle and mid-loop | `regdump-idle`, `regdump-load` | `out/03-regdump-idle/regdump/{fabric-ps,dcs-ps}.bin`, `out/04-regdump-load/a<N>/regdump/...` |
| 2b | ANE SLC DSID at engine 0x285c2046c (4 bytes, SLC data-set id in bits[17:10], per AneDsidRe2); idle and mid-loop, gated with the ANE power state | `regdump-idle`, `regdump-load` | `dsid.bin` + its status in each `index.json` (a gated range is an empty `.bin`) |
| 3 | iBoot-filled ADT items via ioreg (mcc, iop-pmp-nub, ane0, pmgr, dart-ane0) | `ioreg` | `out/01-ioreg-*.txt` |
| 4 | firmware perfStats through `_ANERequest` (NE compute/nominal/throttle cycles) | `timings` | `perf_stats_first/last` fields in every `block-*.json` |
| 5 | ANE power/frequency snapshot tool output, whatever this build prints | `powermetrics` | `out/05-powermetrics/` |

The reg words need the ANERegDump kext: `ranges-gapwin.txt` = the committed runtime request plus
exactly the two ungated non-engine 4-byte ranges (fabric-ps, dcs-ps) and one GATED engine-window
4-byte range (DSID at 0x285c2046c), with the gate set this M2 passed on 2026-09-25 (0x260, 0x2e0,
0x4008-0x4030; macOS never raises ane_sys_mpm 0x4000). The bundle uses only the installed, approved
frozen kext (`/Library/Extensions/ANERegDump.kext`, sha256 932d3b9b) and its CLI
(`~/ane-cap3/aneregdump`, else built from the staged `src/macos-regdump/aneregdump.c`); it never
builds or loads another kext, because a new build needs an Allow click and a reboot. Without root,
the approved kext or its service, the phase records `SKIPPED` with the reason and the rest of the
window proceeds. A dump taken while the islands are down writes a `gated` marker; the ungated words
are still read. Mid-loop, the bundle makes up to 8 attempts and stops after 2 passes.

PerfStats is best-effort by design: `ane_inmem_run` (this branch, `PERFSTATS=1`) passes a mutable
dict as `_ANERequest perfStats`; if this macOS build rejects the type at request creation or at the
first evaluate, the tool logs the refusal and finishes the block with the previous nil behaviour.
Item 1's timings therefore never depend on item 4 succeeding.

## Window order (one boot; ~35 min with the post-boot load gate)

1. `env` `ioreg` `inputs` - untimed setup, ~1 min (inputs verified before anything runs).
2. `timings` - 20 blocks per arm x 4 arms, 3 warm-up + 20 calls per block, ~6 min after the load
   gate (which waits up to 10 min); each block = one in-process compile + 23 calls, stamped with load.
3. `powermetrics` - `cpu_power` (prints "ANE Power" on this box) and `ane_power` under the P6/P7 loop.
4. Fetch the timings, then `regdump-idle` `regdump-load` `sums`: the register phases run last, so a
   kext fault cannot cost the timings.
5. Fetch artifacts, return to Linux (below).

Each phase leaves `out/<phase>.done`; a re-run resumes. A failed probe (for example the macOS
compiler refusing the 32 MiB add) is isolated per block: the failure is recorded and the remaining
arms continue.

## (b) Staging (Linux side, before the reboot)

Build the stage dir (verifies every input against pinned hashes: the E1 manifests, the encoder
MIL 4e3d2e8d + weights 295dccd4, the Linux fixtures 38dce85b/e50598dd):

    python3 macos-bundle/prepare_stage.py \
      --e1 /var/tmp/e1-probes \
      [--e1-prime /var/tmp/e1-probes-prime] \
      --parakeet-mil <repo>/ane-linux-experiments/receipts/2026-09-22-encoder-island-cost/capture/model.mil \
      --parakeet-weights <home>/.local/state/omarchy-private-evidence/encoder-v10/weights/weight.bin \
      --fixtures <dir-with-input_features.npy-and-attention_mask.npy> \
      --inmem-src <worktree>/tools/native-macos/ane_inmem_run.m \
      [--inmem-bin <Studio-built ane_inmem_run>] \
      [--regdump-src <repo>/tools/macos-regdump] \
      --out /var/tmp/gapwin-stage

The `--e1-prime` flag stages the P6' probe (p6prime/) produced by the generator's `--variant p6prime`
on branch `agent/ane-e1-p6-fix` (`tools/e1_probes.py OUT --variant p6prime --check`). The MIL is
byte-identical to P6's MIL by design (the input scale 0.8 lives in p6prime/in/x.npy); the weight
blob is the same `cc0bb784…` and is pinned against the manifest. Omitting `--e1-prime` prints a
PENDING notice and stages the rest of the window without P6' -- the Mac then runs the P6 / P7 /
encoder arms only.

The encoder sources above are the ParakeetFull originals (hash-verified here). The encoder
fixtures (fp16 [1,3000,128] + [1,3000]) are not on this analysis host; pull them from the M2
Linux `/var/tmp/pk-enc/in/` during pre-flight while Linux is still up, or pass whichever copy
hashes to the pinned values. Omitting `--fixtures` (or `--allow-missing-fixtures` on a mismatch)
stages everything else and prints a PENDING notice; the Mac then skips the encoder arm and the
window still decides E1 on P6/P7. `prepare_stage.py` refuses a hard MIL/weights mismatch.

Ship it (both-end hash verification; ~450 MB, ~1 min on LAN):

    TARGET=<m2-macos-ssh-alias> SCRATCH=/Users/<user>/oracle-mint-scratch/gap-window \
      bash macos-bundle/staging.sh

On the Mac (two passes, so the timings are fetched before any kext read):

    SCRATCH=... LOAD_MAX=4 PHASES="env ioreg inputs timings powermetrics" caffeinate -dimsu bash macos_window.sh
    # fetch out/ (tar over ssh + shasum list), then:
    SCRATCH=... PHASES="regdump-idle regdump-load sums" caffeinate -dimsu bash macos_window.sh
    # then: SCRATCH=... TARGET=... OUT=/var/tmp/gapwin-results bash macos-bundle/staging.sh fetch

Fetch verifies the returned `out/SHA256SUMS` locally and removes the remote scratch.

## (c) Linux-side entry and return (the proven procedure)

Pre-flight on the M2 Linux (read-only): boot id + uptime, release module hash, `bo_total_bytes`,
locks free (`flock -n /var/tmp/ane-run.lock true`, gpu queue empty), ESP `boot.bin` hash, disk.
Record load/uptime; announce the window.

Boot notice pattern (recorded rule, before the reboot): 10-minute notice to Main AND the GPU lane
(a `gpu-turn` ticket may be running; reboots wait for it - never stop another lane's ticket), then
`sync`, 40 s, then the boot swap. The M2 boots its default; BootNext selects macOS for ONE boot:

    sudo asahi-bless -n --set-boot-macos -y      # BootNext only; default stays the Linux volume
    sudo systemctl reboot

macOS liveness is SSH over Tailscale first, LAN second, `ConnectTimeout=8` - never ping (the M2
macOS answers no ICMP). Identity check on answer: `hostname; sw_vers; sysctl -n hw.model`.
`sw_vers` must say the expected build before any work; disk free must be > 20 GB and thermal state
clean (`pmset -g therm`), else stop.

Return: `sudo reboot` FROM macOS (BootNext is consumed; the default volume boots back). Liveness
polls every 10 s on the Linux aliases; **6-minute rule**: no SSH answer 6 min after the reboot ->
STOP and report to Main (netconsole receiver on the analysis host is the fallback console; the
camera-on-the-M2 frame is the fallback screen check - both read-only). No retry, no second reboot.

Linux post-verify: boot id changed, default volume still the Linux one, ESP `boot.bin` hash
unchanged, module hash unchanged, no new dmesg bad lines, locks released.

Worst-known recovery path (recorded, not expected): a macOS boot that never answers SSH on any
route for > 6 min -> STOP, report to Main with the three probe results (both SSH routes + camera
frame). The macOS WiFi route can drop mid-session while the wired USB-LAN route keeps answering
(2026-10-02): wrap every macOS ssh in `timeout`, and try the wired route before calling it a failure.

## (d) Analysis (runs now, on this host, no hardware)

    python3 analyze_gap_window.py --synthetic                      # self-check on fake data
    python3 analyze_gap_window.py \
      --macos-capture /var/tmp/gapwin-results/out \
      --linux-e1 /var/tmp/e1-linux/results.json \
      [--linux-ps /var/tmp/e1-linux/ps-words.json] \
      --e1 /var/tmp/e1-probes [--e1-prime /var/tmp/e1-probes-prime] \
      [--linux-out p6prime=<Linux y63.npy>]

`--linux-e1` JSON contract (raw Linux ms; `settle_ms` is subtracted from the Linux min for the
corrected ratio, GapRank's r_p): `{"settle_ms": 1.05, "p6prime": {"blocks": [[block_min_ms,
block_median_ms], ...]}, "p6": {...}, "p7": {...}, "pk": {...}}`. `--linux-ps` is printed as given.

Output: the per-probe ratio table (corrected and raw minmin, medmed), the E1 decision on P6' (P6
when P6' is absent), fabric-ps / dcs-ps / DSID per dump with each range's kext status, perfStats per
probe, golden compares (P6/P6'/P7 rel L2 and non-finite lanes, encoder vs the bit-exact fca96f13,
optional bitwise compare against a Linux output), and the load-stamp caveat.

## Results

The window ran on 2026-10-02: see [result/README.md](result/README.md).

## Known risks, in window-cost order

1. The approved frozen kext is not installed or not loadable: the register phases SKIP with the
   reason (a rebuilt kext needs an Allow click and a reboot). Probe: the kext binary sha256 and
   `kmutil showloaded | grep -i ANERegDump`.
2. macOS in-process compiler refuses P7's 32 MiB add (e5rt refused the encoder MIL on 26.6.2):
   per-block failure recorded, the other arms continue.
3. perfStats dict type wrong for this build: automatic nil fallback; item 4 empty, items 1/2 intact.
4. No passwordless sudo on macOS: reg words + powermetrics skip with recorded refusals; probe is
   `sudo -n true` in `env`.
5. Load on the M2 (Spotlight et al. after boot): the gate waits up to 10 min, then proceeds with
   stamps recording the load (the prior native run's 89 ms was itself taken at load 25).
6. The M2 macOS sometimes answers on LAN only (Tailscale lags after boot), and the WiFi route can
   drop while the wired route answers: try every route.
