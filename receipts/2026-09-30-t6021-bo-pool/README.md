# T6021: io BOs the firmware saw are recycled, not held until reboot (2026-09-30)

## Problem

`ane_t6021` kept every BO whose IOVA reached the firmware (`fw_ref`) allocated
and counted until reboot, under the rule that the firmware never sees a freed
IOVA. `DRM_IOCTL_ANE_BO_INIT` returns `-ENOSPC` once the counted bytes would
pass `ANE_T6021_BO_TOTAL_MAX` (2 GiB). Each `add` process holds about 144 KiB
of io BOs, so a boot ran out after about 14,500 processes. On boot `42d63200`
a 60 s four-worker burst ran 9,601 exact processes, and then every `BO_INIT`
failed until reboot (receipt `2026-09-30-t6021-stock-mailbox`, "Option A
applied"). The IRQ storm had hidden this, because it made each call about 23
times slower.

## Measurement first: `free_io_bos=1` (boot `998cb1e0`)

The existing knob `free_io_bos=1` frees io BOs and unmaps their IOVAs at the
last reference. The installed-path receipt lists it as never run. It was set
with a modprobe option on module `a09a6359` (commit `4648648`):

- add, mul and matvec gates: PASS.
- Two 60 s four-worker bursts: 14,705 and 14,615 processes, 29,320 exact,
  0 failures. This is about twice the old per-boot cap.
- dmesg: 0 DART translation faults, 0 DART errors, 0 `EXCH ... failed`,
  0 quarantine, 0 completion-wait lines.
- Used memory: 4,210, 4,579 and 4,646 MiB before, between and after.

Why io BOs were held: only the rule above (the release comment and the
installed-path receipt). The completion predicate `ane_rtclient_call_wait`
(`ane_t6021_rtclient_main.c:564`) waits until all seven pmgr power words read
0x3ff, the TD count at TM+0x20458 moved, and all eight TQ status words are
idle, and then the call sleeps a 1 ms settle. The ChMan slot and the
target-to-host drain use the IPC surface, not io BOs. No known path reads an
io BO after completion, and none showed up.

What the measurement cannot show: under four workers a freed IOVA is mapped
again within milliseconds, so a late firmware write would land in another
live BO instead of faulting. Zero faults bounds faults only. The exact
checks found no corrupted output.

## Change: an io BO pool (commit `b17f49b`)

In `ane/t6021/ane_t6021_rtclient_main.c`:

- `ane_t6021_bo_release` (`:288`): a `fw_ref` BO is never freed. A program
  section BO (`fw_program`, set at LOAD, `:889`) stays held, because the
  cached firmware program keeps reading it and later loads with the same
  digest reuse that program. An io BO (marked at CALL, `:1063`) goes to
  `ane_t6021_bo_pool` (`:279`, a spinlocked list, because the release runs
  both under `ane_t6021_bo_lock` and from `vm_close`). The IOVA stays mapped
  and the bytes stay counted.
- `ane_t6021_bo_pool_take` (`:307`) and `ane_t6021_bo_init_ioctl` (`:1118`):
  BO_INIT first takes a parked BO of the same page-aligned size, zeroes the
  whole mapping (`:1120`), and hands it to the new owner without new
  accounting. Otherwise it allocates as before.
- A quarantined device neither parks nor takes: after a timed-out command
  the firmware may still write the io BOs, so they stay held.
- `free_io_bos` is removed. The pool makes it pointless, and it unmapped
  IOVAs that the firmware had seen.

The firmware still never sees a freed IOVA. Held memory is the program
sections plus the peak number of io BOs in use at the same time. A late
firmware write, if one exists, can only reach a recycled buffer. It cannot
fault and cannot reach memory the kernel reused. The ioctl ABI is unchanged.

## Device test (boot `4aed18b3`, module `7b592674`, no module options)

Stock `7.1.13-3-1-ARCH`, DTB from the packaged overlay, `hello_wait_ms` 0.

| Check | Result |
| --- | --- |
| add, mul, matvec 2048x2048 m8 gates | PASS |
| matvec 2048x5120 m1 gate | PASS (5120/5120 in band, 4480-5120 bit-exact) |
| Lifecycle: 4 workers x 38 loads of the 2048x5120 matvec, `--repeat 3` | 152/152 PASS |
| 420 s four-worker `add` burst | 105,232 processes (315,696 executions), 105,232 exact, 0 failures |
| Used memory during the burst, every 30 s | 4,797-4,885 MiB (spread 88 MiB) |
| Program dedup after all loads: add, mul, relu, add-scalar, mul-scalar, real-div-scalar, clip-low, clip-high, matvec | all GATE PASS, no `ENOSPC` |
| dmesg, whole boot | 0 DART faults, 0 `EXCH ... failed`, 0 quarantine, 0 completion-wait lines |
| AIC2 884 | 0 |

Before the pool, the same boot would have refused every BO_INIT after about
14,500 processes.

## Limits

- One boot for each phase. The residual late-write risk is bounded only by
  the exact checks.
- The pool matches exact page-aligned sizes. A workload whose sizes change
  every process fills the pool with sizes it never reuses, and the 2 GiB cap
  still applies to that.
- Every M2 boot is a USB chainload from the M1 host.

## Receipts

Private notebook: entry `entries/BoPool/20260930T221400Z-…-bo-pool.md`,
`artifacts/BoPool/` with `SHA256SUMS` (232 files: the three scripts, verify
logs, dmesg, capture logs, gate directories, lifecycle logs, burst logs with
the memory samples).
