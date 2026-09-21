# macOS gate-table dump runbook (authorized; execute on next window)

Goal: dump AppleARMPerformanceController runtime gate tables
(ctl+0x2BC count, +0x2E0 bitmap, +0x320/+0x778/+0x798 tables) on
jw14m2 macOS, resolve gate 473 (ANE-SYS-V, expansion {AFR=380,
ANE_CPU=76} decoded) and the release-clock question. Userland ioreg
proven insufficient (pass-9); this runbook uses the macOS kernel
debugger (KDP), which reads arbitrary kernel memory on a halted
target. NO OS update; SIP changes are boot-args only and reverted
after the session.

## Preconditions
- jw14m2 dual-boot (APFS slices present; 2026-09-17/18 captures prove
  the switch + recovery flow).
- Owner lock: coordinate M2GPUInterleave (35-min-style exclusive
  window; ANE pinned state survives — proven across 4 reboots).
- Linux return path: asahi boot picker (proven every reboot tonight).

## Steps (macOS side, admin terminal)
1. Boot macOS (boot picker; capture boot selection screenshot for the
   recovery receipt).
2. Disable SIP only as far as KDP needs:
      sudo nvram boot-args="debug=0x14e kcsuffix=debugserial=0
      kdp_match_name=en0"
   (record original nvram boot-args first; restore at step 6.)
   Reboot into macOS recovery once if SIP strictly blocks: csrutil
   enable --without kext --without debug — record original CSR
   value; restore at step 6.
3. Reboot macOS; find the AppleARMPerformanceController instance:
      ioreg -l | grep -B2 -A8 "AppleARMPerformanceController"
   (the instance is the provider nub behind ane0; note its
   registry-entry address).
4. KDP attach: target must be halted. Two options in preference
   order:
   a. Two-machine KDP (jw16 or macstudio as debugger host over the
      network): kd = RemoteKernelDebug via `kdp` tooling — requires
      the target's FireWire/ethernet KDP; if unavailable,
   b. Local live read via a signed debug kext is NOT available —
      fallback: `sudo dtrace -w -n` probe reading the symbol
      (dtrace needs the same boot-args; read-only fbt probe on
      apple_dart map paths is enough to dump the tables when the
      controller initializes).
   Dump: for the controller instance base I (from IORegistry
   "Instance"/vmaddr), read I+0x2BC (count), I+0x2E0 (bitmap),
   I+0x320/0x778/0x798 (table pointers + entries).
5. Resolve gate 473: bitmap bit (473>>5, 473&31); table entry ->
   target IOService + symbol; print the AFR (380) and ANE_CPU (76)
   entries too. Save raw dumps to the receipt dir.
6. RESTORE: original nvram boot-args + CSR value; reboot Linux
   (asahi picker); stage1 + insmod + walk verify (alias 320/320);
   confirm to M2GPUInterleave.

## Failure/abort
- macOS boot failure: boot picker -> Linux; pinned ANE state
  re-verified (walk tool); no persistent change except nvram (step 6
  restores).
- No kernel-debug route with available tools: ABORT (documented);
  the AFR DT-node arm becomes the remaining arm only with Main's
  explicit freeze-risk acceptance (two unproven semantics links).
