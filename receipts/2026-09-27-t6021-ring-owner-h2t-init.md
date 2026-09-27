# T6021 ring-owner fix: H2T slots host-initialized before the legacy ACK

Date: 2026-09-27
Scope: experimental `legacy_only` handshake only. No inference, no real
command submission, no install/uninstall lifecycle is claimed or enabled.

## The defect

The 13.5 (22G74) firmware's ChMan H2T ring protocol counts a slot as
empty when its first 24 bytes are all zero. The previous runs left the
H2T rings zeroed at ACK, so the firmware's DebugTask consumed the first
slot of `BUF_H2T` as a real command, faulted (captured context: ELR
`0x128c0`, FAR `0x4`, x0 = 0), and the post-ACK exception globals
(nonzero at `0x4fab80`/`0x4fabb0`/`0x4fabb8`) plus the stuck SCRATCH3
were the observable results. The earlier "timer/scheduler starvation"
reading of that park was wrong: the firmware was crashing on a
host-authored null command, not waiting for a scheduler that never ran.

## The fix

`ane_t6021_chman_host_init()` (in `ane_t6021_boot.h`) validates the
whole 'IPC ' surface against the pinned 13.5 channel layout, then, for
every type-0 (host-to-firmware) channel, writes ownership value `1`
into the first byte of every ring slot (first 24 bytes `{1,0,0}`) —
before the legacy P8 host ack. The write pass is ordered before the
SCRATCH3 store with `dma_wmb()`. The ACK only fires when the table
validated AND the slot init succeeded; any mismatch or short surface
leaves the ACK withheld (no partial writes: the check pass precedes
the write pass, verified atomically in the regression).

## Live verification (single-variable change on the same boot shape)

- Fresh boot, `boot_id 10ea6a92-1891-4bfd-9b91-ff3399531bc5`, kernel
  `7.1.13-ARCH-polltx`, staged module sha256
  `eef1ca22f3b456be9fbe95ff79cb0a62c2e0a6118a8e4b48a2003c239fc963ec`,
  firmware sha-pinned `a9c4b771…` (13.5 22G74) and verified before
  load. The invocation asserts boot-id, vermagic, module absence, and
  both hashes before any `insmod`; `set -euo pipefail` throughout;
  the run captured dmesg before/after, scratch before/after, exit code.
- Channel table: all eight entries `OK` against the pinned layout
  (TERMINAL/2/0, IO/0/1, DEBUG/0/2, BUF_H2T/0/3, BUF_T2H/1/4,
  SHAREDMALLOC/1/5, IO_T2H/1/6, DATA_CHAIN_H2T/0/7 with the exact ring
  offsets the 13.5 firmware builds), mismatch mask `0x0`.
- `chman: H2T slots initialized host-owned before ACK` logged; ACK
  written once; `insmod` exit code 0 in 60 s (no timeout, no hang).
- Post-ACK exception globals: `0x4fab80 = 0`, `0x4fabb0 = 0`,
  `0x4fabb8 = 0` — the exception triple from the pre-fix boot did not
  recur.
- **SCRATCH3 after: `0x00000000`** — the firmware consumed the ACK and
  cleared it through `6d84..6d98`. This is the first observed host-ack
  handoff completing the 13.5 post-DONE sequence end to end. SCRATCH7
  holds the READY word (`0x08042006`), SCRATCH6 the stage counter (9).
- Pre-fix comparison boot (`m2-context-20260927T165639Z`, boot
  `09d05c88…`): same parameters minus the slot-init fix — exception
  globals went nonzero (`0x4fab80 = 1`, `0x4fabb0 = 4`), OWNED-CONTEXT
  carried the faulting PC `0x128c0`, SCRATCH3 stuck at `0x08042006`.

## What this does NOT claim

- No CSNE command, no inference, no program load has been run. The
  legacy transport is armed and observed only; command submission is
  the next protocol step and is not part of this commit.
- No install/uninstall lifecycle change: the module pins per the
  existing wedged-pin rule and stays HELD until reboot.
- The SCRATCH0/1 "result" u64 semantics remain UNSOURCED; nothing here
  infers firmware success from `booted`.

## Offline checks (all run in this tree)

- `gcc -std=c11 -Wall -Wextra -Werror -O2 -Iane/t6021
  tools/h14_boot_regression.c` — 105 checks, 0 failures (includes the
  new `ane_t6021_chman_host_init` cases: short IPC rejected without
  writes, invalid ring rejected atomically, all-H2T-slots-init
  byte-exact expectation, DVA overflow rejected without writes).
- `make -C ane/t6021 check` — 11-case probe-top predicate boundary
  matrix, all hold.
- Kernel module build (`omarchy-linux` arm64, cross build) — clean;
  `ane_t6021.ko` sha256
  `0f11b042fb4d7211b69a767a83626973c38ee764b60e1a57e4b665fa40bf71e8`,
  `ane_t6021_rtclient.ko` sha256
  `3981e1b56692070d8a563f8278c5c61a730e6850efa5a43ff1157c70b530bfe5`.

## Source deltas

- `ane/t6021/ane_t6021_boot.h` — the pinned 13.5 channel layout is
  corrected to the ACTUAL firmware table (TERMINAL ring 0x300 entries
  at ipc+0x800, IO at ipc+0xc800, DEBUG at ipc+0xcc00, BUF_H2T at
  ipc+0xce00, BUF_T2H at ipc+0xde00, SHAREDMALLOC at ipc+0xee00,
  IO_T2H at ipc+0xf000, DATA_CHAIN_H2T at ipc+0x10000; total surface
  `0x10440`), and `ane_t6021_chman_host_init()` is added.
- `ane/t6021/ane_t6021_rtclient_main.c` — legacy_only path: ACK now
  gated on `chman_ok && ane_t6021_chman_host_init(...)`, with
  `dma_wmb()` before the SCRATCH3 store; the fwbuf_audit window adds
  the thread-table rows (0x4fa490..0x4fa4a8) and an OWNED-CONTEXT
  reader for exception frames landing in the owned staged buffer
  (fail-closed, bounds-checked, no `/dev/mem`); stale comments
  corrected (ring-head dump comment; mismatch handling now described
  as ACK-gating, which it is).
- `tools/h14_boot_regression.c` — regression cases for the corrected
  layout and the new init function, including atomic-rejection
  assertions.
