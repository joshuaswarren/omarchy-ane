# T6021: can ane_t6021 power the ANE down when idle? (offline study, 2026-10-03)

Desk study only. No hardware ran. Every number below comes from a saved log or a
static decode, and the source is named next to it. INFERENCE marks a conclusion
that no source states.

Sources: omarchy-ane `160b209`; omarchy-linux `57f8f6deaa3a` (asahi 7.1.13-3 with
the m2-mailbox patch); the selene 13.5 (22G74) firmware and the 13.5
AppleH11ANEInterface kext, decoded with capstone 5.0.7; saved macOS kernel logs
and powermetrics from the lab record. No firmware or kext bytes are in this
receipt.

## Question

ane_t6021 takes a runtime-PM reference in probe and keeps it until reboot
(`ane/t6021/ane_t6021_rtclient_main.c:1991-1994`, `:2106`, `:2357-2368`). The
eight ANE power domains stay on while the module is loaded. Can the driver
release them when the ANE is idle, at what cost, and with what risk?

## What a power cycle loses

- The ASC CPU state and the whole firmware instance: the program table (the
  firmware never frees a program, `:958-974`), processes, its heap and ChMan
  state. Re-entry is a full cold boot with a fresh firmware copy and the iBoot
  patch replay (`docs/t6021-ane-bringup-findings.md:1506-1510`).
- Every `prog_id` and `proc_id` that userspace holds
  (`ane/src/uapi/drm/ane_accel.h:74-106`). A power-down is safe only when no DRM
  file is open, unless the driver reloads every program on wake.
- dart1 and dart2 state (they sit in ane_cpu, `packaging/dt/t602x-ane.dtsi:136`,
  `:146`). apple-dart runtime PM saves and restores TCR/TTBR through the device
  link (omarchy-linux `drivers/iommu/apple-dart.c:939-941`, `:1580-1623`).
  dart0 sits in the always-on pmp domain (`t602x-ane.dtsi:126`) and keeps its
  state. Page tables live in DRAM and stay.
- Program-section BOs cannot be freed today: the final put frees only the
  struct (`ane_t6021_rtclient_main.c:334-335`), so a power-down design needs a
  list of them.

## What a cold boot costs

The "4.3 s" figure is kernel uptime at READY (findings.md:1509), not a
duration. The whole probe takes 1.213 s and 1.210 s on two boots
(`receipts/2026-10-01-t6021-default-on-gate/logs/final/dmesg-ane.txt:5`, `:69`,
and `logs/P/dmesg-ane.txt`). About 1.1 s of it is the 30 ms pause after each of
about 36 boot-phase log lines (`ane/t6021/ane_t6021_boot.c:317-329`). Without
the pauses a cold boot would take about 0.1 s (INFERENCE). Program reloads are
not measured.

For reference, macOS on an M1 Max (macOS 26.6.2, same kext family) powers the
ANE on and finishes `ANE_Init` in 24.2 ms, and powers it off in 242 ms, of
which 240 ms is a WFI poll that times out.

## Firmware power commands (selene 13.5)

| Command | Firmware behaviour |
|---|---|
| 0x0b POWER_DOWN | `CPlatformEnvironment::Shutdown`: stops timers, `RTK_power_prevent_nap(1)`, `RTK_power_request(8)`, then WFI forever. No resume. |
| 0x24 SUSPEND | `WaitAllIdle`; warns if programs or processes are still loaded; then `CEnvironment::Suspend` (thread idle). The host must unload everything first. |
| 0x2d SET_DYNAMIC_POWERGATE | `setDynamicPowerGate`: with 1, the firmware turns TD, BASE and SET1-4 off after each job and on for the next (`powerDownAne` / `powerUpAne` through the PS words at 0x28e084008). The ASC stays on. |
| 0x0c / 0x29 SET_SNE_PMU_BASE(2) | `SetPMUBaseAddress` (fixed 0x28e084008), then `powerDownAne`. |
| 0x13 / 0x14 POWER_DEVICE_ON / OFF | prints one log line; no power action. |

The dynamic power-gate flag has one writer, `SwitchDynamicPowerGate`; its
default is off (INFERENCE). Linux never sends 0x2d, so the compute islands
stay on after the first job. RTKit power messages
(`ane/t6021/ane_t6021.h:265-280`) are not usable: the 13.5 legacy transport has
no RTKit session (`ane_t6021_rtclient_main.c:2134-2137`).

`boot_prevent_nap` defaults to 1 (`ane/t6021/ane_t6021_boot.c:144-149`,
`:388-391`). The firmware releases its nap prevention only when that bit is
clear, so with the default the ASC never naps. No kext evidence says that macOS
sets the bit.

## What macOS does

- The kext powers the ANE off in the same millisecond that the last client
  closes. `ANE_deInit` sends 0x0e, 0x01 and 0x0b (or 0x24), polls
  ASCWRAP_IDLE_STATUS (engine+0x1400048, the word this driver calls
  `ANE_ASC_CPU_STATUS`) for WFI, then `DisableANEClocksAndPower` writes 0 to
  set4..set1, base, td and ane_cpu and closes PWGATE. The next open cold-boots
  the firmware (`ColdStarts` grows; `Resumes` stays 0).
- Inside a session the firmware gates the compute islands between jobs. M2 idle
  words under macOS 27: ane_sys and ane_cpu 0x0f000300, islands 0x300
  (findings.md:563-576).
- The kext also has an idle sleep timer, a dynamic power-gate hysteresis timer,
  and `ANE_RestoreState`, which re-creates the programs of open clients after a
  wake.
- powermetrics "ANE Power" on the M2: 9.05-9.15 W under the encoder, 0 mW with
  the firmware running and compute idle (findings.md:634-636). Because it reads
  0 mW while the ASC runs, it does not show the ASC cost (INFERENCE).

No Linux measurement of idle power with the module loaded against not loaded
exists.

## Designs, ranked

1. Measure first: no driver change. Boot arm A (module blacklisted) and arm B
   (defaults), 5 minutes each, ABAB.
2. Firmware-managed idle (`dyn_pg=1`, `boot_prevent_nap=0`, both default off in
   a test build): send 0x2d after CONFIG_GET the way `fw_perf_mode` sends its
   property (`ane_t6021_rtclient_main.c:1604-1613`), and add a read-only debugfs
   file of the seven PS words. Keep the PM reference: if genpd ever powers on an
   island that the firmware gated, the firmware ASSERT in `PowerUp` hangs it, so
   this must never be combined with autosuspend of the same device. About 1.25
   agent-days.
3. Close-to-idle power-down: on the last close, after `idle_off_s`, send 0x0b,
   poll for WFI, clear CPU_CONTROL bit 4 then bit 5, free and reset all firmware
   state, put the PM reference; cold-boot on the next open with the probe's boot
   path moved into one shared function and the 30 ms pauses removed. No in-boot
   ASC restart was ever done on T6021, so the risk is high. About 5-6.5
   agent-days, plus 3-4 for macOS-style program replay.
4. SUSPEND-based sleep: needs every program unloaded and restored; no warm
   resume evidence. Not viable now.

## Hardware protocol (for the M2 lane)

One boot per arm, because the module pins itself and cannot unload
(`ane_t6021_rtclient_main.c:1878-1883`). Arms: A blacklisted, B defaults,
C `dyn_pg=1 boot_prevent_nap=0`, D `idle_off_s=30` (only after design 3
exists); order ABCABC. Per boot: 120 s settle, the 16 gates and one encoder
process, 30 s wait, then 5 minutes at 1 Hz of macsmc hwmon power, the seven PS
words, `pm_genpd_summary`, the ANE `runtime_status`, load and PSI. Quiet-box gate:
load1 < 0.5, PSI some avg10 = 0, screen off or fixed brightness, no GPU work,
AC with a full battery. Report B−A and C−B with a bootstrap 95% CI, first-call
latency after 5 s idle against steady state (20 cycles), encoder min-of-min
(+1% gate), golden exact, 16/16 gates, `ane_stats` jobs exact, zero EXCH
failures, quarantines, DART faults and external aborts, and a 10-minute random
idle/run stress for C. Any quarantine ends the arm.

## T6020, T6022, T8112

T6020 and T6022 die 0 use the same addresses, PS words, selene firmware and
`pmu_pa` as T6021 (`packaging/dt/t602x-ane.dtsi:40-45`,
`ane/t6021/ane_t6021_fwload.c:207-226`): same power sequence. T8112 uses the bia
firmware, PS words at pmgr +0xc000..+0xc038, PMU base 0x23b70c010, and a PWGATE
word at set +0x8b8 that the kext opens before the PS words
(`ane_t6021_fwload.c:203-206`, `:228-233`); a power-down there must close it
after them.

## Discrepancy

findings.md:577-579 says the stock Linux DTB marks ane_sys_mpm
`apple,always-on`. In omarchy-linux `57f8f6deaa3a`,
`arch/arm64/boot/dts/apple/t602x-pmgr.dtsi:568-575` has no such property. Which
DTB the M2 boots today was not checked.

## Not verified

No hardware run. The dynamic power-gate default, the macOS value of the nap bit,
the meaning of `RTK_power_request(8)`, and the RVBAR latch and dart1/dart2
tunables after an ane_cpu power cycle are open. The macOS timings come from an
M1 Max, not the M2.
