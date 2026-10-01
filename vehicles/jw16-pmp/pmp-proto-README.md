# Jw16PmpProto — candidate boot plan (for Main's GO)

Ticket: offline recovery of the macOS ApplePMPv2 host-driven PM protocol + minimal
Linux host-side addition, up to a built hashed candidate + ready-to-run boot plan.
NO hardware writes have happened in this thread. This file is the GO checklist.

## What was recovered (evidence: entries/Jw16PmpProto + artifacts/Jw16PmpProto)

One RTKit application endpoint (OS ep 0x20 under Linux; the firmware's table also
declares a runtime-numbered `pmp_ctrl` ep), four message classes by bits 55:52:

| class | messages (direction) | Linux state before this patch |
|---|---|---|
| 1 Mem | 0x10 MemInitReq / 0x12 Alloc / 0x14 Free (fw→host) | served ✓ |
| 3 Reg | 0x30 RegistryInit / 0x32 AddEntry (IOREG) / 0x34 RemoveEntry (fw→host) | served ✓ (78 channels) |
| 0 legacy | 0x0 Startup (fw→host) → 0x10 Configure (host→fw, shmem DVA) → 0x20 Configure_Ack | NEVER SPOKEN ✗ |
| 2 PM | bit53 set, cmd = bits 47:44: 0=ping, 6=set-DVFS-states {domain,0,min,max-rem}, 8/0xe dev-pwr | NEVER SPOKEN ✗ |

The map113 DVFS unlock hypothesis (evidence-weighted): the AP-side enable write aborts
because the firmware's DVFS engine is never armed — no Configure handshake, no per-domain
DVFS state window (PM cmd 6), no PTD SOC-DEV-PS-REQ arming (m1n1 arms it pre-start;
row 0xf8 @ report SRAM 0x28e3c0000+0x10000). The candidate boot completes the handshake
and OBSERVES the firmware's class-0/2 traffic (all logged); the map113 probe then
discriminates enable-accepted vs still-locked.

## Candidate artifacts (built this thread, nothing installed)

- Kernel: omarchy-linux branch `agent/jw16-pmp-pm-protocol` (commit bb7b68a94fb5, from
  HEAD 57f8f6deaa3a), patch = +98/-5 lines in drivers/soc/apple/pmp.rs ONLY:
  class-0 handshake (Startup→Configure with a 64 KiB zeroed shmem, reg maps at +0xe000
  per m1n1), PM-class logging, unknown-class logging (ep+class+raw), pmp_ctrl ep 0x21
  start attempt, richer unknown-message log. Syscall arms byte-identical behavior.
- Build: cross aarch64 on omp-studio-local (gcc 12.2 cross, rustc 1.98, bindgen 0.71.1),
  config = jw16's live /proc/config.gz with ONLY: DEBUG_INFO/BTF off (build-size),
  CONFIG_LOCALVERSION="-3-2-ARCH" (release string preserved → module vermagic parity;
  recorded /proc/config.gz delta). Built-in-only change: no module ABI surface touched
  (CONFIG_MODVERSIONS not set; exported symbols unchanged).
  Candidate vmlinuz sha256: <PINNED_AT_STAGE> (artifacts/Jw16PmpProto/build/).
- Variant DTB: the PROVEN Pmp5 v5 bytes unchanged
  (bf5bb60476cf034a77d93f7e7bdc3489acac29f03ff49a26c394c35e1c1a0ac7 — 78-channel
  variant, ane/dart nodes byte-identical to base 7b6ac97a).
- Vehicles (omarchy-ane origin/agent/jw16-pmp-proto, vehicles/jw16-pmp/):
  pmp-proto-kernel-install.sh (stage|install|revert|check — second GRUB entry,
  grub-reboot one-shot, stock entry default), pmp-proto-identity.sh (kernel-sha +
  PM-line + ane-bound + IRQ gate), reuse of the proven pmp4-watcher.sh /
  pmp4-variant.sh / pmp4-deadman-revert.sh / pmp5-dvfs-run.sh / pmp5_report.ko.

## Boot plan (ONE variant boot per GO; abort/revert on any gate failure)

1. Stage (no hardware effect): kernel-install.sh stage <sha>; variant DTB rebuild via
   the proven pmp4-variant.sh machinery (byte-check against bf5bb604...).
2. Watcher FIRST: pmp4-watcher.sh live (variant.sha = bf5bb604; triggers unchanged;
   revert surface = boot.bin 6e8f90c8 + DTB 7b6ac97a + update-m1n1 + grub revert).
3. Off-host ESP backup refresh (boot.bin + DTB, byte-verified).
4. JW16_MAINTENANCE gate; herdr panes w6Z:p1 + w73:p1 ask; netconsole armed early
   (configfs, pmp4-nc-arm.sh) since the candidate changes kernel, not just DT.
5. Swap boot.bin (variant DTB embedded; EMBEDDED-variant-OK scan) + install candidate
   kernel + `grub-reboot '<title>'` + reboot.
6. Identity gate ≤60 s: pmp-proto-identity.sh <sha> — kernel sha PASS, apple_pmp probe
   lines, NEW: 'PMP Startup -> Configure', 'PMP Configure ack', any 'PMP PM:' lines,
   exactly one 'unknown property' (ce-map), 'PMP started', ane bound, no oops.
7. Stability ≥120 s (no crash signatures, IRQ < 2000/s).
8. n1 smoke (gpuwin discipline, hidden16 fca96f1355485ec3 bit-exact) — the ANE lane
   must be UNAFFECTED.
9. map113 probe (pmp5-dvfs-run.sh, ONE rung): DVFS_ON write-1 readback (the Pmp5
   EFAULT is THE discriminator — accepted write or PTD row movement = unlock found),
   idle token, rows read, restore. NO ladder climb in this GO.
10. Close-out either way: revert boot.bin + DTB + grub entry, reboot, full close-out
    verification (n1 smoke bit-exact, llm health 200 + authenticated completion,
    btrfs stats zero, ane bound by apple-dart).

## Named deltas/risks of the candidate vs the running kernel

- Built from omarchy-linux HEAD 57f8f6deaa3a; if jw16's running 7.1.13-3-2-ARCH
  package predates the m2-mailbox poll-TX re-apply, the candidate carries it. On
  T6001 the send-empty IRQ exists, so the poll path is dormant. Named, not hidden.
- DEBUG_INFO/BTF off in the candidate (build size only; module loading unaffected —
  CONFIG_MODVERSIONS is not set and exported symbols are unchanged).
- /proc/config.gz will show CONFIG_LOCALVERSION="-3-2-ARCH" (stock shows "-ARCH";
  release string identical).
- If the firmware's PM/Startup traffic rides pmp_ctrl (0x21) instead of 0x20, the
  identity log shows it (all eps logged) — that is a protocol RESULT, not a failure.

## Pass/fail for the ticket

PASS = the handshake lines appear + no crash + ane lane unaffected + probe result
recorded (either unlock observed or still-locked with the exact message log).
Either outcome closes the ticket's step 1-2; a still-locked result names the next
lever (PM cmd 6 per-domain DVFS arming / PTD SOC-DEV-PS-REQ pre-arm — both already
recovered and implemented as follow-ups).
