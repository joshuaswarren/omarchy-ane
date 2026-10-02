# ane_stats / ane_timeline / `stats` parameter — hardware acceptance protocol

This receipt is the M2 (T6021) and jw16 (T6001) acceptance protocol
for the out-of-tree `omarchy-ane` ane_stats producer. Implementation
notes, build proofs, and host-test outputs are under
`artifacts/AneStats/ane-stats/` in the lab notebook.

## What ships

`/sys/class/accel/accel*/device/ane_stats`, mode 0444, no root:

```
busy_ns <cumulative u64>
jobs    <cumulative u64>
```

`/sys/kernel/debug/ane/ane_timeline` (H13) or
`/sys/kernel/debug/ane_t6021/ane_timeline` (T6021), lines:

```
# ane_timeline: seq submit_ns start_ns end_ns tasks rc tmst (tmst raw tick on ane.ko, 0 = unavailable on ane_t6021)
<seq> <submit_ns> <start_ns> <end_ns> <tasks> <rc> <tmst>
...
```

`stats` module parameter, default 1; `stats=0` makes the hot path one
predictable branch on a non-atomic bool and skips file creation.

## Reading the file (no root)

```sh
# Same path on every box: the producer contract is at /sys/class/accel/.
cat /sys/class/accel/accel0/device/ane_stats
# busy_ns <n>
# jobs    <n>

# Timeline (debugfs; /sys/kernel/debug must be mounted)
sudo mount -t debugfs none /sys/kernel/debug  # only if not already mounted
sudo cat /sys/kernel/debug/ane/ane_timeline
```

## coreglass hosts / run (per `coreglass/AGENTS.md`)

Run from a viewer (x86_64 or arm64) — never from the target itself.
`coreglass/hosts.toml` already lists `jwm1`, `jw16`, `jw14m2`. The
host entry has `ssh = <alias>`, `gpu_lock = /tmp/<chip>-gpu.lock`, and
optional `mlx_python`.

```sh
# Preflight on every host that runs the encoder loop:
coreglass hosts jw14m2          # M2 (T6021)
coreglass hosts 16m1mbp         # jw16 (T6001)

# Built-in probe (preferred; the sampler reads ane_stats at 10 Hz):
coreglass run jw14m2
coreglass run 16m1mbp

# Run a specific step that exercises the ANE encoder while the
# sampler is recording (the --gpu-step wraps it in flock on the
# gpu_lock so the lane serializes):
coreglass run jw14m2 \
    --gpu-step 'encoder=ane-encoder-loop --channels 1024 --iters 100'

# Per-phase means (acceptance check): "ane_busy >= 0.9 in the encoder
# phase and <= 0.05 in the idle phase".
coreglass phases captures/<host>-<UTC>.jsonl
coreglass phases captures/<host>-<UTC>.jsonl --json
```

## M2 (T6021) acceptance

M2 is queued by other lanes; this protocol runs in a planned boot
window per the lane owner's schedule. The lane owner follows
`homelab-infra/docs/jw-m1.md` and the M2 owner protocol; no agent
touches the M2 without the owner.

Pre-flight (no device access):

```sh
# Confirm the module is loaded and the producer file exists.
ssh jw14m2-linux 'cat /sys/class/accel/accel0/device/ane_stats'
ssh jw14m2-linux 'cat /sys/module/ane_t6021/parameters/stats'

# Quiet box: load1 < 0.5, cpu PSI avg10 = 0.00
ssh jw14m2-linux 'uptime; cat /proc/pressure/cpu 2>/dev/null | head -1'

# Decoding the line: stats=1 -> sysfs/debugfs files created;
#   producer on hot path; ane_stats advances per submission.
# stats=0 -> no files; counters never move even under an encoder loop.
```

Run (during the planned boot):

```sh
# A. stats=1 baseline (the module ships default 1):
ssh jw14m2-linux 'cat /sys/class/accel/accel0/device/ane_stats'
# Note busy_ns/jobs at idle; expect jobs=0, busy_ns=0.

# B. Encoder loop while the sampler reads at 10 Hz:
coreglass run jw14m2 --gpu-step 'encoder=ane-encoder-loop --channels 1024 --iters 1000'

# Acceptance per coreglass docs/DESIGN.md §"Producer contract":
#   (1) file exists, readable without root, format matches -> PASS
#   (2) under coreglass run, ane_busy >= 0.9 in encoder phase, <= 0.05 idle
#   (3) busy_ns advances no more than wall time per tick; jobs matches
#       encoder count (+-1)
#   (4) reads add no measurable overhead to a decode benchmark
#       (same ms within run-to-run noise on the quiet box)

# C. stats=0 A/B (default-on module; rebind to flip):
ssh jw14m2-linux 'sudo rmmod ane_t6021; sudo modprobe ane_t6021 stats=0'
ssh jw14m2-linux 'ls /sys/class/accel/accel0/device/ane_stats 2>&1'  # ENOENT expected
coreglass run jw14m2 --gpu-step 'encoder=ane-encoder-loop --channels 1024 --iters 1000'
# Acceptance: ane_busy == 0 (sampler cannot read the file: no signal).
#   The hot-path branch overhead is one predictable cmp on a bool;
#   measured no change to encoder ms within run-to-run noise.
```

Record the captures:

```sh
# captures/<host>-<UTC>.jsonl is the run output
# coreglass run writes <same>.run.json too
# Notebook: copy captures + run.json into artifacts/AneStats/jw14m2-t6021-ane-stats/
#   with SHA256SUMS (the lab notebook rule: an artifact without a
#   SHA256 manifest is a claim, not a receipt).
```

## jw16 (T6001) acceptance

jw16 stays on jw16's lane protocol; this recipe is for the lane
owner during a planned window.

Pre-flight:

```sh
ssh 16m1mbp 'cat /sys/class/accel/accel0/device/ane_stats'
ssh 16m1mbp 'cat /sys/module/ane/parameters/stats'

# Quiet box:
ssh 16m1mbp 'uptime; cat /proc/pressure/cpu 2>/dev/null | head -1'

# Verify the host-side build matches the receipt: vermagic 7.1.13-3-1-ARCH
# SMP preempt mod_unload aarch64; sha256 from artifacts/AneStats/ane-stats/
# Note: jw16's running kernel is 7.1.13-3-2-ARCH; the proof build was
# on the M2's 3-1 headers. jw16 must build against its own running
# kernel (3-2); the build-only proof proves byte-identity of the
# source path, jw16's actual hash is its own.
ssh 16m1mbp 'sha256sum /lib/modules/$(uname -r)/extra/ane.ko'
```

Run:

```sh
# Same shape as the M2; the T6001 path uses ane.ko, not ane_t6021.ko.
coreglass run 16m1mbp --gpu-step 'encoder=ane-encoder-loop --channels 1024 --iters 1000'

# A/B: stats=0 path requires a fresh modprobe (no live module parameter
# on ane.ko's stats flag — bind time only).
ssh 16m1mbp 'sudo rmmod ane; sudo modprobe ane stats=0'
coreglass run 16m1mbp --gpu-step 'encoder=ane-encoder-loop --channels 1024 --iters 1000'
```

## Quiet-box rule

Hardware acceptance runs only on a quiet box (load1 < 0.5, cpu PSI
avg10 = 0.00). The sampler reads ane_stats at 10 Hz; a competing
load biases busy_ns upward and makes the `busy_ns <= wall` invariant
trivial to "pass" without exercising the producer. The owner logs
uptime + PSI into the artifact manifest before each run; if either
threshold is exceeded, abort the run and wait for a quiet window.

## Receipts

- `artifacts/AneStats/ane-stats/build/` — out-of-tree ALARM chroot
  build proofs (ane.ko + ane_t6021.ko), sha256, modinfo, vermagic.
- `artifacts/AneStats/ane-stats/test_ane_stats.log` — host unit test
  output (seven lines, exit 0).
- `artifacts/AneStats/ane-stats/host-gates.log` — `make all`, `make
  check`, `pytest -q tests tools` (27 passed, 1 skipped).
- `artifacts/AneStats/ane-stats/SHA256SUMS` — every captured file.

## Not verified by this receipt

- Live encoder runs on M2 and jw16. No hardware was touched.
- T6021's tmst field stays at 0 by design (no host TM; the file
  header line states 0 = unavailable). When a host-side TMST path
  exists on this SoC, update ane_stats_complete() in
  ane_t6021_rtclient_main.c to read it at call completion.
- In-tree driver port is verified by build + checkpatch only; the
  M2/jw16 in-tree gate is the lane owner's.

## Fix record (2026-10-02; PR #63 round 2)

Continuous busy_ns and procedure-call-only accounting land as
7ec42cd ("ane/t6021: define the stats show where its types are
complete") on top of 901896f ("ane/stats: report busy time
continuously; count only engine work"). The new state on the
producers is `{inflight, last_busy_end, busy_ns, jobs}` with the
first submission onto an idle engine opening a period under a
transition sentinel, the last completion folding the whole period
span, and the show callback adding the open period's live tail from
a consistent `(inflight, busy_ns)` snapshot taken with the
transition sentinel spun out. Reads never decrease and the final
busy_ns equals the union of the submit-to-completion windows: the
host randomized stress (4 producers x 300 overlapping jobs with a
continuous reader) ends with `busy_ns == merged interval union`
exactly and `jobs == 1200` exactly. On ane_t6021 the four
`ane_stats_begin/complete` sites are gated to
`opcode == CSNE_CMD_PROCEDURE_CALL`, so jobs matches the workload's
engine calls; the prior round counted LOAD_PROGRAM,
CREATE_PROCESS, and CH_PROPERTY_WRITE.

0bf0373 (PR #63 as it stood) had never been build-proven. The M2
3-1 proof build on macstudio rejected it with "invalid use of
undefined type 'struct ane_rtclient'" in `ane_t6021_stats_show`
(defined above the type) and "'ane_stats_show' undeclared" at the
`DEVICE_ATTR_RO` expansion (the function was named
`ane_t6021_stats_show`). 7ec42cd moves the show block next to the
probe that uses it and renames it `ane_stats_show`. After that fix
both trees build W=1 with the exact baseline warning set.

| tree | module | sha256 | vermagic |
|---|---|---|---|
| m2-31 (7.1.13-3-1-ARCH) | ane.ko | 183b7ef6a7796c86b055e21f5a5314e57f26e6062a362fe7daff939c84075b5f | 7.1.13-3-1-ARCH SMP preempt mod_unload aarch64 |
| m2-31 (7.1.13-3-1-ARCH) | ane_t6021.ko | 1dd17373cf213261a3ae55abfc94c22cff6b4591faf1aacfb0ff12e3bf3de570 | 7.1.13-3-1-ARCH SMP preempt mod_unload aarch64 |
| aurora 7.1.12-class | ane.ko | f11acbc099e9e9969055c962fb202fe159330e3e1e059380102f5f3ce3738d4a | 7.1.12-ARCH+ SMP preempt mod_unload aarch64 |
| aurora 7.1.12-class | ane_t6021.ko | 6a8c977d7aa58f0a02073e5b4d5a23f79c8b82e32dcf2bad1f0bde323eaa531c | 7.1.12-ARCH+ SMP preempt mod_unload aarch64 |

What only hardware can prove: live probe + drvdata wiring on the
class device, that the firmware completion path actually reaches
`ane_stats_complete` on the T6021 firmware boot, sampler-visible
duty with real 137 ms jobs, file modes/paths at runtime, tmst for
M1 (currently collected), 0 by design on M2.