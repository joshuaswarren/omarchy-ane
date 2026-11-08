# J700/H17: CPU power sequences, control windows, and the Linux ownership contract

Offline continuation from `b0a0463`. No live device-tree/sysfs inspection,
MMIO, power transitions, module loads, watchdog operations, or reboot.
Everything below is compiled-source observation of the retained kernelcache;
nothing here is device qualification. The evidence ledger is
`data/h17/t8140-stage4.json` (this commit); the planner stays **BLOCKED**.

## Exact evidence identities

- Retained kernelcache SHA-256:
  `7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`
  (Darwin 25.6.0, `xnu-12377.161.13~4/RELEASE_ARM64_T8140`; HAL UUID
  `3bdfba52-…`, interface UUID `374de710-…`).
- Recorded ADT: repository `b0a0463`, `data/ane-soc/t8140.json`, source A1
  SHA-256 `b0fbf661a25626403fa1035ba681d5a7ab8ce13130e2a6d2320d4167255d0a88`.
- Linux source: `b6f070c911934c9c377c4be939b883bf32f37acf` in
  `/var/lib/linux-aurora`, inspected DTS paths unmodified.
- The old-versus-retained kernelcache provenance gap remains unresolved.

## Register-control construction and window binding (still partial)

`ANEHWDevice::start` builds `ANERegisterControl` objects from provider index
`object888` (direct path: 0; RTBuddy path: 2). With the start-function base
pointer `x23 = this + 0xe228`, and `x23' = this + 0xe270` inside the
CPU-power functions, the traced layout is:

| Label | Object field | Index | Guard (value on ane-type 0x200) | Call |
| --- | --- | --- | --- | --- |
| ANE | `0xe320` | `object888` | none observed | `0xfffffe0008f53b1c` |
| PS | `0xe328` | `object888 + 1` | byte 912 (1) | `0xfffffe0008f53b54` |
| PWGATE | `0xe330` | `object888 + 2` | byte 243 (1) | `0xfffffe0008f53b84` |
| PTD | `0xe338` | `object888 + 3` | byte 14036 (0; a whole-driver-text immediate-offset store scan finds no writer) | `0xfffffe0008f53bb4` |
| ANEHAL1 | `0xe348` | field 916 = `object888 + 4` | byte 235 (1) | `0xfffffe0008f53be8` |
| ANEHAL2 | `0xe350` | field 928 = `object888 + 5` | byte 235 (1) | `0xfffffe0008f53c1c` |
| ANEHAL3 | `0xe358` | field 944 = 0 | byte 235 (1) | `0xfffffe0008f53c50` |

On the direct path this maps to ADT resources ANE=0, PS=1, PWGATE=2,
ANEHAL1=4, ANEHAL2=5, ANEHAL3=0, and **no PTD control is created** on this
configuration. Every traced offset fits the retained ADT resource size at its
index: ANE scratch `0x1880020…0x188003c` and ASC `0x1600044/0x1600048` are
inside resource 0 (`0x2000000`), PS writes `0xc000/0xc008` are inside
resource 1 (`0x18000`), and PWGATE writes `0x268c/0x26b0` are inside
resource 2 (`0x4000`). This is arithmetic cross-checking, **not** a runtime
window binding and **not** safe-access proof.

## J700 CPU power-on sequence (compiled only)

`ANEHWDevice::EnableCPUClocksAndPower`, ane-type `0x200` block at
`0xfffffe0008f294b0`; field 14880 bit 1 is resolved set for J700
(`initializeANEProperties` stores 3 at `0xfffffe0008f64294`; the 0x200
branch stores nothing; `start` clears bit 2 only at `0xfffffe0008f53258`):

1. PS `write32(0xc000, 0xf)` — `0xfffffe0008f294b0`
2. PS `validate(0xc000, expected 0xff, mask 0xff, 5000)` — `0xfffffe0008f294c0`
3. PWGATE `write32(0x268c, 0)` — `0xfffffe0008f294f0`
4. PWGATE `validate(0x268c, expected 0, mask 3, 5000)` — `0xfffffe0008f29968`
5. PS `write32(0xc000, 0x1000000f)` — `0xfffffe0008f2997c`
6. PS `validate(0xc000, expected 0x100000ff, mask 0x100000ff, 5000)` —
   `0xfffffe0008f29b0c`

With bit 1 clear only steps 1–2 run. `ANERegisterControl::validate`
(`0xfffffe0008f74194`) polls `read32(offset) & mask` until it equals
`expected`, one `_IODelay(10)` per poll, `timeout+1` polls; it returns
`0xe00002bc` on timeout only when its bool argument is nonzero and **panics**
otherwise. Every traced call passes 0, so a Linux port must not copy that
failure mode.

## J700 CPU power-off sequence (compiled only)

`ANEHWDevice::DisableCPUClocksAndPower`, ane-type `0x200` block at
`0xfffffe0008f2b848`:

1. (bit 1 set) PS `read32(0xc000)` — `0xfffffe0008f2bf80`
2. PS `write32(0xc000, value & ~0x10000000)` — `0xfffffe0008f2bf9c`
3. PS `validate(0xc000, expected 0, mask 0x10000000, 5000)` — `0xfffffe0008f2bfa8`
4. PWGATE `write32(0x268c, 3)` — `0xfffffe0008f2bfc0` (reached on both bit-1 outcomes)
5. PWGATE `validate(0x268c, expected 3, mask 3, 5000)` — `0xfffffe0008f2bfd8`
6. PWGATE `write32(0x26b0, 0)` — `0xfffffe0008f2bff0`
7. PWGATE `validate(0x26b0, expected 0, mask 1, 5000)` — `0xfffffe0008f2c2c4`
8. PS `write32(0xc000, 0x300)` — `0xfffffe0008f2c2d8`
9. PS `validate(0xc000, expected 0, mask 0x8ff, 5000)` — `0xfffffe0008f2c2fc`

Register roles, clock gating, and settle timings are unqualified; nothing
here authorizes a write attempt.

## PSD flag: the PSD-first branch is dead in this artifact

Byte 513 is read at nine sites (`EnableCPUClocksAndPower`
`0x8f2913c`, `EnableANEClocksAndPower` `0x8f29fb8`/`0x8f2a474`,
`DisableANEClocksAndPower` `0x8f2acd0`/`0x8f2af80`,
`DisableCPUClocksAndPower` `0x8f2b5e0`, `ANE_deInit` `0x8f424d4`,
`start` `0x8f53180`/`0x8f53f00`), always testing bit 0. The only stores
covering byte 513 in `com.apple.driver.AppleH16ANEInterface` text are the
constructor `strh wzr` at 512 (`0x8efa09c`, `0x8efa3ac`) and the
`initializeANEProperties` zero block (`0x8f64498`); the RTBuddy branch sets
byte 512 only (`0x8f52b84`). No immediate-offset store in the driver sets
byte 513 nonzero, so `AppleARMIODevice::enablePsdService` (slot `0x8c8`) is
never dispatched and the PSD-first provider path is unreachable in this
build. Limit: immediate-offset store encodings only; register-indexed stores
are not excluded by this scan.

## Clock and power requests

`EnableANEClocksAndPower`/`DisableANEClocksAndPower` call the provider
through `AppleARMIODevice::enableDeviceClock` (slot `0x8a8`,
`0xfffffe0008f2a034`/`0xfffffe0008f2a908`/`0xfffffe0008f2af10`/
`0xfffffe0008f2afc8`) and `enableDevicePower` (slot `0x8b0`,
`0xfffffe0008f2a090`) with index field 896 — 0 on the direct path — plus a
byte-242-guarded variant at index+1. Against the ADT `clock-ids`
`[327, 326, 325]` that resolves to clock id 327 unconditionally, 326 with the
byte-242 flag, and no attributed call site for 325. The provider translates
these indices through tables parsed by
`AppleARMIODevice::initDeviceWithProvider` (`0xfffffe000896a8c0`) from the
DT properties `clock-ids`, `clock-gates`, `power-gates`, `service-gates`,
`service-reset`, `service-name`, `pin-groups` — none of which exist on the
Linux `ane` node.

Byte 242 itself is traced to `initializeANESoCConfig`: the prologue stores
0 at `0xfffffe0008f64b64` (before the ane-type switch), fetches the provider
property `"pmp-virt-notify"` (string `0xfffffe00073b4f5d`), calls through
slot `0x198` on the result, and stores that call's boolean result at
`0xfffffe0008f64b9c`. The recorded ADT carries `pmp-virt-notify = 1`, so on
J700 the flag is expected set — the runtime call result is not qualified.

## Linux ownership/handoff contract requirements

Drafted against the traced Apple dependencies. Each item is **unresolved**
until source-qualified and device-qualified; none is satisfied by this round.

1. **Resource windows.** The J700 sequences write the PS (ADT resource 1,
   `0x300700000+0x18000`) and PWGATE (resource 2, `0x300724000+0x4000`)
   windows; the Linux `ane` node maps only resource 0 (`"engine"`). Linux
   needs mapped windows for every control the chosen path creates (including
   ANEHAL1/2 = resources 4/5 if HAL tunables are ever claimed). Unresolved
   which subset is minimal.
2. **Clock votes.** Apple requests clock ids 327 and (conditionally) 326
   through provider methods fed by `clock-ids`. The Linux DTS has no
   `clocks` property on the ANE node and `t8140-pmgr.dtsi` contains no
   clock-controller nodes at all; the pmgr gate identity for 327/326/325 is
   unknown. Unresolved whether `ps_ane_sys` suffices in their absence.
3. **Power-domain votes.** Linux votes only `ps_ane_sys` (PMGR `0x290`,
   ANE-SYS-V). The ADT also lists `ANE-SYS-V-PMP`, `aot-power`, and
   `pmp-virt-notify`; the byte-242-gated second clock vote and PMP semantics
   have no Linux counterpart traced.
4. **PSD absence.** The PSD-first branch is unreachable in the retained
   build, so a Linux port may omit PSD service — recorded as absence
   evidence in one artifact, not as a requirement proof.
5. **Firmware load and ownership.** `ANE_Init` splits RTBuddy versus
   direct-boot on byte 512; `ANE_ExclavePreparePowerOff` dispatches through
   the exclave proxy RPC `_aneexclave_aneexclavexnu_preparepoweroff`.
   Linux has no RTBuddy client, no exclave service, and no firmware-load
   story for `ane,t8132exclave`-shaped nodes. Owner and load order
   unresolved.
6. **DART.** Three ANE DARTs are wired in the Linux DTS with `ps_ane_sys`;
   ADT exclave SID entries and stream selection for the exclave path remain
   unqualified.
7. **Recovery.** Apple's `validate` panics on timeout; a Linux port must
   fail soft. Watchdog ownership stays excluded; recovery sequencing remains
   unqualified.
8. **Second-function flag.** Field 14880 bit 1 selects the extended
   sequences; J700 resolves it set. Its hardware meaning ("child/second
   function") and the single-ANE Linux equivalent are unresolved.

## Follow-up confirmations in this round

- Byte 14036 (PTD guard): zero covering stores in the entire
  `com.apple.driver.AppleH16ANEInterface` text under immediate-offset store
  encodings — the PTD control is not created on this configuration.
- Byte 242: resolved to the `pmp-virt-notify` provider lookup result
  (stores `0xfffffe0008f64b64`/`0xfffffe0008f64b9c`); it gates the second
  `enableDeviceClock` vote (clock id 326).
- Field 14181 (the CPU-power status byte Enable/Disable write through
  `x22 = this + 0x3765`) is written only through register-indexed
  addressing, which the immediate-offset scan does not enumerate; its
  semantics stay unresolved.

## What this round does not prove

No register value, sequence, or window above has been exercised on a device.
The byte-parity-style coherence between compiled offsets and ADT resource
sizes is not runtime binding, and a covered offset is not a covered
neighbourhood. Hardware release criteria remain open.
