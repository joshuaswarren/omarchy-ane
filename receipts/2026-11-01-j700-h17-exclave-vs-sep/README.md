# J700/H17: exclave vs SEP, and the firmware unblock path

Offline analysis only. No device power, MMIO, module load, or reboot.

## SEP and "exclave" are different layers

- **SEP** (Secure Enclave Processor) is a physical secure coprocessor. On
  macOS it is `AppleSEPManager` + `AppleMesaSEPDriver` (Mesa = Touch ID).
  On this machine the aurora kernel drives it directly: `sep@282600000`
  (`apple,sep`, mailbox + `ps_sep`/`ps_gpio`) and the Rust driver set
  `drivers/soc/apple/sep.rs`, `bio.rs`, `keybag.rs`, `sks.rs`, `pmp.rs`,
  `trusted.rs` — `bio.rs` exposes `/dev/sep-bio` for enrol/match. Touch ID
  working here is the proof that Linux can drive an Apple secure
  coprocessor directly, without Apple's service layer.
- **Exclave** is Apple's newer secure-service framework (ExclaveKit). On
  T8132/T8140 the kernel reaches several coprocessors through it. The
  retained kernelcache shows `ExclaveSEPManagerProxy`,
  `AppleExclaveCameraProxy`, `H16ISPExclaveEventSource`, and
  `ANEExclaveProxy`. So the SEP is *one* of the exclave-managed
  coprocessors; "exclave" is the transport/ownership layer, not a
  processor.

## The ANE exclave on T8140

The ADT records `exclave-assigned = true`,
`exclave-service = com.apple.service.ANEExclave`,
`exclave-edk-service = com.apple.service.ANEExclave_EDK`, and
`exclave-reg = 0x441c00000+0x88000`. On macOS, `ANEExclaveProxy` is an
RPC client: `exclaveSetup()` calls `IOService::exclaveStart` /
`exclaveEndpoint`, and power-off goes through
`_aneexclave_aneexclavexnu_preparepoweroff`. That is why macOS no longer
drives the ANE registers directly on this SoC.

## Why the exclave is an ownership layer, not a hardware gate

1. **The firmware is unencrypted and iBoot-stageable.** The installed
   `apple/ane/h17_ane_fw_theia_d9x.macho` is a raw arm64e `MH_PRELOAD`
   (SHA-256 `32ce2ba5…`, 0x180000 bytes), matching
   `packaging/omarchy-ane-firmware-fetch`'s `THEIA_D9X` pin (verified with
   `--check`). `__TEXT` 0xc0000, `__DATA` 0xc0000+0x2ac000
   (`data_fileoff` 0xc4000, `data_filesize` 0xbc000), `_rtk_patchbay`
   at 0xcbdc8, `_rtk_tunables` at 0xde030.
2. **The hardware boot sequence is the same direct path Linux already
   uses.** `ANE_Init` at `0xfffffe0008f3e138..0x8f3e198` writes RVBAR
   `0x1050000`, clears/sets CPU_CONTROL `0x1600044` (RUN = 0x10), and
   polls SCRATCH7 `0x188003c` for `0x08042006` — the identical shape the
   shipped driver uses for T8112.
3. **Linux already bypasses the exclave for the SEP** on this exact
   machine (see `sep.rs`).

## What was done this round (offline)

- Verified the installed T8140 firmware against the fetch pin (size,
  SHA-256, Mach-O segments, section offsets).
- Authored the T8140 soc row for this repo's own driver family:
  `ane/h17/ane_h17_soc.c` + `ane/h17/ane_h17_soc.h`, targeting the
  existing `struct ane_h16_soc` table (whose T8132 row already uses
  `0x1600044`/`0x1600048`/`0x1050000`/`0x1880020`/`0x1608000` — the same
  layout H17 needs). The row compiles and passes its type/value check.
- Confirmed the DTS patch (`docs/t8140-ane-power.patch`) is the right
  shape for this driver: the mailbox is engine-relative (`0x1608000`), so
  no DT mailbox node is needed; only the `pmgr`/`set` windows and the
  `ps_ane_sys_cpu` pwrstate.

## Row values: pinned vs candidate

| Field | Value | Evidence |
| --- | --- | --- |
| `ane_type` | `0x200` | ADT |
| `cpu_control` | `0x1600044` | kernelcache boot block |
| `cpu_status` | `0x1600048` | kernelcache / status-diagnostic |
| `rvbar` | `0x1050000` | kernelcache `write64` |
| `scratch0` | `0x1880020` | `InitANEScratchRegisters`; DONE `0x08042006` at `0x188003c` |
| `ps_off[0]` (ANE_SYS) | `0x290` | ADT |
| `ps_off[1..4]` | `0xc000/0xc008/0xc010/0xc018` | **candidate** (T8132 template); only `0xc000` has a T8140 trace |
| `irq_status`/`irq_ack`/`mbox` | `0x188c000`/`0x1890000`/`0x1608000` | **candidate** (T8132 template) |

No `of_match` entry was added: the H16 driver would then claim a boot
path that has never run. Wiring it is a separate, hardware-gated step.

## Remaining unknowns

- Whether any T8140 step genuinely requires the exclave (clock,
  attestation). Unqualified, but not supported by the evidence above.
- The `ps_off[1..4]` values and the ANE_CPU `no_ps` meaning.
- `irq_status`/`irq_ack`/`mbox` for T8140.
- Whether the H17 firmware speaks the same ChMan/RTKit mailbox protocol
  as T8112.
