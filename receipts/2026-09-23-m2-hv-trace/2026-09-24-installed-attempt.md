# Installed-path full attempt — 2026-09-24 ~10:21 UTC

## Vehicle
- ane_mailbox_poll.ko (send-empty optional, polled TX): mailbox bound,
  all 13 ANE devlinks `available` (mailbox was the bind wedge).
- ane_t6021_rtclient.ko fw_load=1 fw_start=1 fw_start_venc_gates=1
  (built from this worktree): platform bound 284000000.ane by itself.

## Sequence (dmesg BOOT-PHASE, box survived throughout)
- genpd: eight islands raised via supplier links.
- VENC_SYS 0x2902803e0 then leaves 318-321: all final 0x3ff (ACTUAL on).
- Tunables granted (P-1a..l), scratch cleared + pulsed 1->0.
- RVBAR skipped (latch 0x10000000001 lawful, bit0 set).
- RUN: CPU_CONTROL 0 -> 0x10, status 0x28.
- poll A (READY 0x08042006, <=1000x1ms): full ~1.5 s, no READY.

## Outcome
- cpu_started=1 fw_alive=0 booted=0. HELD, RTKit skipped, bind
  fenced-inert. "no SCRATCH7 READY after CPU release — ASC fetch did
  not reach the staged alias (kernel-context start discriminator:
  negative)".
- This is the measured mode-bits park: the released CPU cannot fetch
  through dart-ane0 at the latched entry. Next lever: entry-alias
  mapping (write-arm B9 lane).

## State left
- M2 on Linux, CPU released (HELD until reboot), ESP 43ec6090, jwm1 Linux.
