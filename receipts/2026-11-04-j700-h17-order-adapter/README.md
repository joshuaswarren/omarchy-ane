# H17 (T8140) ordered boot path wired into the driver

Parent `e7a8a52`. Software only: no CPU release, no module load, no
device access. The T8140 `stage=boot` fail-closed refusal
(`-EOPNOTSUPP`) is replaced by the dedicated H17 path it was waiting
for; every prerequisite it named is now in place (DMA ownership
`1a31d8b`, ownership admission `e7a8a52`, checked ordering `bf21193`).

## What the driver now does on T8140 stage=boot

1. Segments, DMA domain, alias reservation, preload ownership
   admission, quiescence — unchanged gates from the parent receipts.
2. `ane_h16_h17_enable()` — kext steps 1–8 of the firmware-load-path
   receipt §5.4 (CE status/safety check and toggle, bounded ack poll,
   64-bit ack, the 37 decoded HAL masked writes, PS `0xc000` = 0xf →
   poll, PWGATE `0x268c` = 0 → poll `&3==0`, PS `0xc000` =
   0x1000000f → poll, PS `0xc008` = 0xf → poll), executed through
   `ane/h17/bringup_order.h` via an MMIO adapter. Every access is
   durably logged with a monotonic sequence number BEFORE it is
   performed (`h17-order seq=%u ...`, dev_info); a logging failure
   stops the sequence before MMIO.
3. Cold-branch scratch init (steps 9, 10, 12): SCRATCH0..7 = 0,
   SCRATCH6 = 1, SCRATCH7 = 0. The warm branch (SCRATCH7 := 1) is the
   `this[0xa2]`-bit0 path and is deliberately NOT implemented: a fresh
   Linux boot corresponds to the kext's first `ANE_Init` (cold).
4. RVBAR (skipped when latched — iBoot preload), CPU_CONTROL 0 then
   0x10, SCRATCH7 wake-word poll — the shared H16 release path.
5. Post-wake steps 19–21 only after the wake word: `0x1880058` = 1,
   `0x1880060` = 0, `0x196c000` setBit 0. Step 17 is absent (this[0xa4]
   is always 0 on J700).

New window: the overlay now gives the ANE node a third reg,
`pwgate` = `0x3_00724000` size `0x4000` (kext "PWGATE" provider window,
receipt §2), and the driver requires it when `soc->h17_order`.

## Safety properties kept

- Ordering failure → REFUSED `h17-order` BEFORE any CPU release; no
  staging snapshot has been taken yet (ordering runs before `fw_pin`).
- After CPU release, binding, mappings, module reference and runtime
  power are retained on both success and timeout (parent receipt).
- The other rows (t8132/t604x) keep the generic path; `h17_order` is
  true only for the T8140 row.

## Tests (host only; not device qualification)

Simulator additions: pwgate bus region with real window size, CE
handshake model (request at 0x18a4000 acknowledged at 0x18a4008), pmgr
ps-word model (ACTUAL follows TARGET — the condition the kext polls),
the real H17 SoC row compiled into the sim, and `pos-h17-boot`, which
checks the exact PS/PWGATE/SCRATCH6/post-wake writes and requires ≥80
durable `h17-order seq=` log lines. Pin/preload/map refusals now assert
"no release-relevant access" instead of "zero engine writes", because
the clock/power ordering legitimately precedes them.

All 22 scenarios, discrimination mutations, ADT mutations, and the W=1
kernel build pass (the 29 build warnings are pre-existing kernel-header
noise, none in this tree). Local outputs:
`/var/tmp/opencode/h17-order-adapter-sim.log`,
`-disc.log`, `-build.log`. `make -C ane/h17 sim-all` still passes
(ordering helper unchanged).

## Next hardware rung (fresh boot)

`optin=t8140 stage=boot` with `boot_wait_ms` default. One rung, one
write budget: the ordering sequence performs the machine's first
PS/PWGATE/CE writes. Failure modes and parking rules are unchanged:
timeout ⇒ RESULT FAIL + reboot before retry; never rmmod/reload.
