# T6021: the finish event is the output-landed signal; the 1 ms settle goes (2026-10-06)

## Problem

Every T6021 CALL cost a fixed 1 ms on top of the work. After the IO_T2H
finish event arrived, `ane_rtclient_command` slept `call_settle_us`
(1,000-1,100 us). H14 add ran at 1.478 ms per call and the Coreglass
ledger's ANE cell at 674 jobs/s, against 17.5 us per job on the M1.

## Why the settle existed

- 2026-09-29 (`c9a1b44`): on three boots about 1 add call in 5 returned an
  all-zero output. The ack, the last-committed-TD word and the TQ status
  words reported done about 0.13 ms before the output reached DRAM. The
  settle was a fixed margin of about 8 times that lag.
- 2026-09-30 (`3a942d6`, [2026-09-30-t6021-call-wait](../2026-09-30-t6021-call-wait/README.md)):
  those three signals mark the dispatch of the last task, not its end. The
  wait moved to the firmware's state-1 IO_T2H event. The settle stayed on
  an inference: "for short programs the finish event arrives with the
  signals that were earlier measured 0.13 ms ahead of the output". No run
  measured output landing against the finish event with the settle at 0.

## The completion signals

| Path | Completion signal | Wait after it |
| --- | --- | --- |
| T8103 host TM (`ane/src/ane_tm.c:223-244, 284-286`) | TM event `0x05000000 \| nid << 16 \| (td_count - 1)` on both IRQ lines, and TM idle; 1 us poll, 1 s timeout | none |
| T6021 firmware, 13.5 selene (legacy ChMan) | IO_T2H (MBI channel 6) event with the CALL cookie and state 1 | 1 ms settle, now none |
| T6021 RTKit / mailbox IRQ | none: the 13.5 firmware sends no HELLO, and starting the mailbox fires AIC2 884 about 700,000 times/s (`hello_wait_ms` row in `ane/t6021/README-install.md`) | - |
| T6021 SCRATCH words | boot phase only; no per-call word is known | - |

macOS sends PROCEDURE_CALL (0x204) over its RTBuddy "IO" endpoint. Its
completion path was not traced; [INFERENCE] it receives the same firmware
finish message.

## Change

`ane/t6021/ane_t6021_rtclient_main.c`:

- `call_settle_us` defaults to 0 instead of 1000. A CALL returns when the
  state-1 event arrives. The parameter stays (0644) to test a suspected
  late output write; it does nothing at 0.
- The wait keeps its bound: the CALL's `timeout_ms`, 5,000 ms when the
  caller passes 0. On timeout the driver now logs at error level,
  `call completion wait failed -110: no finish event in N ms`, and
  quarantines the device as before. Nothing falls back to a sleep.

ABI unchanged.

## Device test

jw14m2 (M2 Max), boot `772d212d`, kernel `7.1.12-2-11.36-sep-ARCH`, in-tree
`ane_t6021.ko` sha256 `8ca4e257` (linux-aurora 7.1.12.aurora2-11.36; the
finish-event wait and the settle in its source branch,
`ane-driver-aurora-combined-v2` `f088ca5`, match `db99b0d` except for
whitespace and the `ane_stats` hooks),
tools from omarchy-ane `259ba06` with the libane pad fix. One
gpu-turn ticket per arm. The after arm set `call_settle_us` to 0 at run time,
the new default, and restored 1000 at the end. A module built from this
branch cannot load on that boot: the module never unloads, so a swap needs
a reboot.

| Check | settle 1000 (before) | settle 0 (after) |
| --- | --- | --- |
| H14 add, 7 runs x 200 calls, median per run | 1.478 ms (all 7) | 0.308-0.319 ms |
| same, min / p90 (range over runs) | 1.373-1.422 / 1.482-1.484 ms | 0.205-0.209 / 0.359-0.370 ms |
| add output sha256, 14 runs, fixed seeded inputs | `1262ed81` | `1262ed81` (1 distinct) |
| landing check (`landing.c`), new inputs every call, read right after the ioctl | 1000 calls, 0 mismatched | 5000 calls + 50 processes x 20 calls, 0 mismatched, 0 stale, 0 all-zero |
| Qwen program 20, one call per process | 5 runs, 4.817-4.863 ms | 10 runs, 3.710-3.798 ms |
| program 20 output sha256; vs M1 golden | `b58be2a4`; rel L2 0.001174 | `b58be2a4` (1 distinct); rel L2 0.001174 |
| Parakeet encoder, 20 calls, median | 254.497 ms, golden `fca96f13` bit-exact | 253.438 ms, bit-exact |
| Coreglass ledger ANE cell (H14 add, 600-call batches, 10 s) | 673.2, 674.3 jobs/s | 3,200.8, 3,210.5 jobs/s |
| new dmesg lines during the arm | 0 | 0 |

- Ledger cell: `ledger_cell.py` runs the ledger's own step, sampler and
  metric (Coreglass `071c590`) for 3 reps per arm. The table lists the reps
  that pass the ledger's idle rule (`idle_w_max` 9 W); one rep per arm
  idled at 14.4 W and 18.7 W while another lane used the GPU (674.2 and
  3,216.9 jobs/s).
- The landing check rotates 8 seeded input pairs, so each call expects a
  result that differs from the previous call's in almost every lane. A
  write that landed after the ioctl returned would read as a mismatch.
- Timing ran at load1 < 0.5 and PSI cpu 0.00 (checked by `common.sh`).
- The before arm ran twice. In the first attempt the script held
  `ane-run.lock` around `qwen_prog_run.py`, which takes the same lock, so
  program 20 deadlocked until the per-run timeout; the attempt was
  stopped and wrote no program 20 output. Its add timing matched the
  second attempt (1.478-1.479 ms).

## Staged on the M2 for the 10:40Z reboot (not loaded)

02:23Z, jw14m2 boot `772d212d`, no reboot, the running module untouched
(`call_settle_us` reads 1000):

- Built from the `ane/` tree of `a43639a` against the M2's own headers:
  `make ANE_VERSION=a43639a KCFLAGS="-iquote <tree>/ane/src"` (the aurora
  headers ship their own `uapi/drm/ane_accel.h`, which kbuild would find
  first). No warnings. `ane_t6021.ko` sha256 `7f15b4a8f259eae6...`, vermagic
  `7.1.12-2-11.36-sep-ARCH SMP preempt mod_unload aarch64` = `uname -r`,
  version `a43639a`, `call_settle_us` present (default 0), the same four
  `apple,{t6020,t6021,t6022,t8112}-ane` aliases as the in-tree module.
- Backup of the in-tree module: `/var/tmp/keep-ane/settle/backup/ane_t6021-intree-8ca4e257.ko`
  (sha256 `8ca4e257...`, equal to `kernel/drivers/accel/ane/ane_t6021.ko`,
  which stays in place).
- `sudo install -D -m 0644 ane_t6021.ko /lib/modules/$(uname -r)/updates/ane_t6021.ko`,
  `sudo depmod -a -e -E /lib/modules/$(uname -r)/build/Module.symvers` (no
  unresolved symbols), `sync`. `modinfo -n ane_t6021` now resolves to
  `updates/ane_t6021.ko` (depmod search order: updates, extramodules,
  built-in). The initramfs (`/boot/initramfs-linux-aurora.img`) holds no
  ane module, so the next boot loads the staged file from the root.

Revert, before or after the reboot:

```sh
sudo rm /lib/modules/$(uname -r)/updates/ane_t6021.ko
sudo depmod -a && sync
modinfo -n ane_t6021   # .../kernel/drivers/accel/ane/ane_t6021.ko, sha256 8ca4e257
```

The running module never unloads, so a revert takes effect at the next
boot. Never rmmod or insmod `ane_t6021`.

After the reboot, check before any other ANE work:
`modinfo -F version ane_t6021` is `a43639a`;
`/sys/module/ane_t6021/parameters/call_settle_us` is `0`; dmesg shows the
usual boot chain and no `EXCH`, quarantine or DART fault line; then the
gates (`gates-1136.sh`: add, mul, matvec, islands, encoder `fca96f13`) and
the `after.sh` checks. `after.sh` restores 1000 when it exits; on the new
module change that restore to 0 first.

## Limits

- One boot. The 2026-09 all-zero boots predate the finish-event wait and
  have not recurred, so this run cannot show what such a boot does without
  the settle.
- The after numbers come from the in-tree module with the parameter at 0,
  not from a build of this branch; that build runs from the 10:40Z boot.
- The add latency is bimodal (p10 about 0.21 ms, median about 0.31 ms).
  The finish-event poll sleeps 50-100 us per turn, so part of each call is
  poll granularity. It was not changed.

## Files

- `landing.c`: the per-call landing check (links libane).
- `common.sh`, `before.sh`, `after.sh`: the two gpu-turn tickets.
- `ledger_cell.py`: the ledger ANE cell, one arm.
- `before.log`, `after.log`: the ticket logs (trailing blanks stripped).

Private notebook: entry `entries/AneSettle/20261006T020047Z-jw14m2-linux-call-settle.md`,
artifacts `artifacts/AneSettle/20261006-settle0/` with `SHA256SUMS` (both
arms' outputs, dmesg snapshots, ledger captures, the aborted first attempt,
the 11.36 build of this branch).
