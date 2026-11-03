# J700 / T8140 (H17) — ANE exclave: ownership, transport, and gating

Offline binary analysis only. No hardware, no module, no MMIO, no reboot.

## 0. Artifacts and method

| Artifact | Identity |
|---|---|
| `/var/tmp/neo/kernelcache.bin` | SHA-256 `7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`, MH_FILESET, 366 images, arm64e, Darwin 25.6.0 `xnu-12377.161.13~4/RELEASE_ARM64_T8140` |
| `/var/tmp/opencode/adt.bin` | SHA-256 `90103c9a9884f6eb39e524b101d8b1cff321de1704bc17ce514ab9f7fe81c4c2` |

Method: parsed LC_FILESET_ENTRY images, LC_SEGMENT_64 VA→file map, LC_SYMTAB
(9,142 symbols in the ANE kext alone), full-text disassembly of
`com.apple.driver.AppleH16ANEInterface` `__TEXT_EXEC` (275,822 instructions)
with a complete call graph, plus the prelink-info `IOKitPersonalities`
plist embedded in the cache. Addresses below are link-time (no KASLR slide).
All instruction-level statements are for that exact SHA-256.

Tooling left in `/var/tmp/opencode/`: `kclib.py` (Mach-O/fileset parser),
`kdis.py` (annotated disassembly), `xref.py`, `find_exclave2.py`,
`exclave_strings.txt` (every string hit), `adt_ane.txt`.

---

## 1. Where "exclave" lives

2,379 raw occurrences; 691 distinct strings. Symbols containing `exclave`
(global/local text+data), by image:

| Image | symbols | of which non-`_os_log_fmt` |
|---|---:|---:|
| com.apple.driver.ExclavesAudioKext | 445 | 396 |
| com.apple.driver.AppleH16CameraInterface | 319 | 188 |
| **com.apple.driver.AppleH16ANEInterface** | **295** | **167** |
| com.apple.driver.IOPAudioVoiceTriggerDevice | 98 | 92 |
| com.apple.driver.DMAChannelProxy | 90 | 90 |
| com.apple.driver.AudioSharedDARTMapperProxy | 88 | 88 |
| com.apple.ExclaveKextClient | 70 | 70 |
| com.apple.driver.ExclaveSEPManagerProxy | 70 | 70 |
| com.apple.kernel | 14 | 14 |
| com.apple.driver.EXDisplayPipeH17P | 8 | 8 |
| IISAudioIsolatedStreamECProxy / XrtHostedXnu | 5 / 5 | 5 / 5 |
| EXBrightKext / SecureRTBuddyProxy | 3 / 3 | 3 / 3 |
| EmbeddedAudioLibs, AudioDMAController-T8140, IOPAudioIsolatedIOBufferDevice, XSPMI | 2 each | 2 each |
| AppleMobileDispH17P-DCP, AppleARMIISAudio | 1 each | 1 each |

So the exclave is not an ANE peculiarity: on this SoC camera, display,
audio, DMA, XSPMI and the SEP all reach their hardware through the same
mechanism. The ANE is one of ~15 exclave clients.

There is also a large body of exclave strings attributed to
`com.apple.filesystems.webdav` in my file-offset scan (1,301). Those are
false positives from the shared `__LINKEDIT`/prelink-info region at the
end of the file (that is where the `IOKitPersonalities` plist lives); the
string data itself is the ANE kext's personality, quoted in §2.

### 1.1 ANE kext personality (from the cache's prelink plist)

```
<key>t8132exclave</key>
  IOClass           = H11ANEIn
  IONameMatch       = ane,t8132exclave
  IOProviderClass   = AppleARMIODevice
  IOExclaveProxy    = true          <-- same key XNU's generic IOExclaveProxy uses
  IOProbeScore      = 10000

<key>ANEExclaveProxy</key>
  IOClass           = ANEExclaveProxy
  IONameMatch       = ane,ane-exclave-proxy
  IOProviderClass   = AppleARMIODevice
  IOExclaveProxy    = true
  IOProbeScore      = 10000

<key>RTBuddyBringup</key>   IOClass = H11ANEIn, provider RTBuddyService, IOPropertyMatch role = ANE
<key>RTBuddyBringup1</key>  IOClass = H11ANEIn, provider RTBuddyService, IOPropertyMatch role = ANE1
```

The J700 ADT node's `compatible = ane,t8132exclave` therefore matches the
`t8132exclave` personality, which is declared `IOExclaveProxy = true` and
whose provider is `AppleARMIODevice` (not `RTBuddyService`).

---

## 2. `ANEExclaveProxy` — methods, RPC table, and dispatch

Class vtable/class objects: `__ZTV15ANEExclaveProxy` `0xfffffe0007f503b0`,
`__ZN15ANEExclaveProxy9metaClassE` `0xfffffe0007f503a0`.

| Method | Address |
|---|---|
| `ANEExclaveProxy::start(IOService*)` | `0xfffffe0008f3a4f4` |
| `ANEExclaveProxy::stop(IOService*)` | `0xfffffe0008f3a680` |
| `ANEExclaveProxy::exclaveSetup()` | `0xfffffe0008f3a74c` |
| `ANEExclaveProxy::exclaveTeardown()` | `0xfffffe0008f3ab48` |
| `ANEFirmwareECCmdHandleITQInterrupt(...)` (server-side callback) | `0xfffffe0008f3ad3c` |
| `ANEDriverECCmdQueryForResult(bool*)` | `0xfffffe0008f3b160` |
| `ANEDriverECCmdPersistentClientUpcallAck(bool*)` | `0xfffffe0008f3b2d4` |
| `ANEDriverECCmdSaveExclaveState(bool*)` | `0xfffffe0008f3b448` |
| `ANEDriverECCmdRestoreExclaveState(bool*)` | `0xfffffe0008f3b5b4` |
| `ANEDriverECCmdPauseExclaveWork(bool*)` | `0xfffffe0008f3b720` |
| `ANEDriverECCmdResumeExclaveWork(bool*)` | `0xfffffe0008f3b88c` |
| `ANEDriverECCmdStartFirmwareRecoveryProcess(bool*)` | `0xfffffe0008f3b9f8` |
| `ANEDriverECCmdEndFirmwareRecoveryProcess(bool*)` | `0xfffffe0008f3bb64` |
| **`ANEDriverECCmdPreparePowerOff(bool*)`** | **`0xfffffe0008f3bcd0`** |
| `ANEDriverECCmdDebugReadProperty(u32,u32*)` | `0xfffffe0008f3be3c` |
| `ANEDriverECCmdDebugWriteProperty(u32,u32,u32)` | `0xfffffe0008f3be4c` |

`start()` stores the provider (`safeMetaCast` → `AppleARMIODevice`) at
`this+0x88`, then calls `this->vtable[0x5e0](this, 0)`.
`setupANEExclaveProxyService()` (`0xfffffe0008f67da8`) allocates the proxy
via `ANEExclaveProxy::metaClass->alloc` and stores it at `ANEHWDevice+0x2d0`,
then `init` / vtable `+0x6a0` / vtable `+0x5f0` with the provider.
`ANEExclaveInit()` (`0xfffffe0008f680b8`) drives the rest; a failure is
logged and **does not fail `start()`**.

### 2.1 The `_aneexclave_*` RPC surface (kernel → exclave, "downcalls")

Client stubs, all in the ANE kext `__text`:

| RPC stub | Address | Called from |
|---|---|---|
| `_aneexclave_aneexclavexnu_queryforresult` | `0xfffffe0008fdffdc` | `ANEDriverECCmdQueryForResult` |
| `_aneexclave_aneexclavexnu_persistentclientupcallack` | `0xfffffe0008fe0140` | `ANEDriverECCmdPersistentClientUpcallAck` |
| `_aneexclave_aneexclavexnu_saveexclavestate` | `0xfffffe0008fe02a4` | `ANEDriverECCmdSaveExclaveState` |
| `_aneexclave_aneexclavexnu_restoreexclavestate` | `0xfffffe0008fe03f0` | `ANEDriverECCmdRestoreExclaveState` |
| `_aneexclave_aneexclavexnu_pauseexclavework` | `0xfffffe0008fe053c` | `ANEDriverECCmdPauseExclaveWork` |
| `_aneexclave_aneexclavexnu_resumeexclavework` | `0xfffffe0008fe0688` | `ANEDriverECCmdResumeExclaveWork` |
| `_aneexclave_aneexclavexnu_startfirmwarerecoveryprocess` | `0xfffffe0008fe07d4` | `ANEDriverECCmdStartFirmwareRecoveryProcess` |
| `_aneexclave_aneexclavexnu_endfirmwarerecoveryprocess` | `0xfffffe0008fe0920` | `ANEDriverECCmdEndFirmwareRecoveryProcess` |
| **`_aneexclave_aneexclavexnu_preparepoweroff`** | **`0xfffffe0008fe0a6c`** | **`ANEDriverECCmdPreparePowerOff`** |
| `_aneexclave_aneexclavexnudebug_debugreadproperty` | `0xfffffe0008fe1964` | `ANEDriverECCmdDebugReadProperty` |
| `_aneexclave_aneexclavexnudebug_debugwriteproperty` | `0xfffffe0008fe1ac4` | `ANEDriverECCmdDebugWriteProperty` |
| `_aneexclave_aneexclavexnu__init` / `__init_static` | `0xfffffe0008fe0bb8` / `0xfffffe0008fe0c0c` | `exclaveSetup` (`0x8f3aa48`) |
| `_aneexclave_aneexclavexnu__server_start[_owned]` | `0xfffffe0008fe0c98` / `0xfffffe0008fe0cc0` | no in-kext caller |

Driver-side wrappers and their exact callers:

| Wrapper | Address | Caller |
|---|---|---|
| `ANEHWDevice::aneExclaveQuery(u64)` | `0xfffffe0008f6a138` | `aneExclaveWorkerThreadEntry` (`0x8f68b40`) |
| `ANEHWDevice::aneExclaveSaveState()` (`ANE_ExclaveSaveState` `0x8f6b430`) | `0x8f6a8c8` | `setPowerStateGatedPriv` |
| `ANEHWDevice::aneExclaveRestoreState()` (`ANE_ExclaveRestoreState` `0x8f6b5f8`) | `0x8f6ad38` | `power_on_hardware` |
| `ANE_ExclavePause` | `0x8f6b814` | `ANE_PowerOn_gated`, `processTargetToHostIOCommand` |
| `ANE_ExclaveResume` | `0x8f6ab54` | — |
| `aneExclaveStartFWRecoveryProcess` | `0x8f6ae74` | `HandleFirmwareTimeout_gated` (2 sites) |
| `aneExclaveEndFWRecoveryProcess` | `0x8f6b044` | — |
| `aneExclavePreparePowerOff` | `0x8f6b180` | `DisableANEClocksAndPower` (`0x8f2af7c`) |
| `aneExclaveReadProperty` / `aneExclaveWriteProperty` | `0x8f6b2bc` / `0x8f696f0` | — |
| `ANE_ExclaveModeCycle(bool,bool)` | `0x8f69830` | `ANE_PowerOn_gated`, `ANE_PowerOff_gated`, SEP↔EXCLAVE handoff |

`ANE_ExclaveCycle/Load/Evaluate/Unload` (`0x8f6b400/40c/418/424`) are
12-byte stubs that `return 0` — dead in this build.

### 2.2 The other direction — `aneengine.*` (exclave → kernel)

The kext also contains a full server IDL for two interfaces, with every
method's marshaller/unmarshaller:

`aneengine.aneenginerequest` (server start `0xfffffe0008fefa78`):
`modeswitch, initengine, setupisocontext, teardownisocontext,
mapl2spillbuffer, unmapl2spillbuffer, loadprogram, unloadprogram,
teardownprogram, createprocess, terminateprocess, procedurecall,
requestprogramid, returnprogramid, inferencecall, requestcacherequestid,
teardowncacherequest, returncacherequestid, triggercacherequest,
queryrequeststatus, propertyread, propertywrite`

`aneengine.aneengineinterrupt` (server start `0xfffffe0008ff3018`):
`handleitqinterrupt`

Supporting IDL types carried over this interface are exactly what a
*driver of the engine* needs: `aneenginebarinfo`, `aneenginesidsetup`,
`aneengineisolationcontextsetup/teardown`, `aneenginebufferdescriptor`,
`aneenginesegmentproperty`, `aneengineproceduresegmentproperty`,
`aneenginel2spillbuffermapinfo/unmapinfo`, `aneengineoperationproperty`,
`aneengineinferencecallproperty`, `anerequesthwtimestampinfo`.

Caveat on direction: `_aneengine_aneenginerequest_*` client entry points
have **zero** call sites in this kext, so I can prove the IDL and the
server stubs exist, not that the kernel driver invokes them. What *is*
called is `_aneengine_aneengineinterrupt__init` (`0x8f3a9cc`, from
`exclaveSetup`) and the local handler
`ANEFirmwareECCmdHandleITQInterrupt` (`0x8f3ad3c`, from
`aneExclaveInterruptHandler` at `0x8f68ce8`).

---

## 3. Transport

### 3.1 XNU API actually used (undefined symbols of the ANE kext)

```
__ZN9IOService12exclaveStartEPS_PPNS_19IOExclaveProxyStateE
__ZN9IOService15exclaveEndpointEPNS_19IOExclaveProxyStateE
__ZN9IOService32exclaveAsyncNotificationRegisterEPNS_19IOExclaveProxyStateEP22IOInterruptEventSourcePj
_exclaves_get_status
```
Resolution addresses in `com.apple.kernel`:
`IOService::exclaveStart` `0xfffffe000bc975f8`,
`exclaveEndpoint` `0xfffffe000bc97eb0`,
`exclaveAsyncNotificationRegister` `0xfffffe000bc9a014`,
`exclaves_get_status` `0xfffffe000b3f3a00`.

XNU additionally exports ANE-specific upcall hooks (present, but with zero
call sites anywhere in the cache — so unused by these kexts):
`exclaveRegisterANEUpcallWorkBegin` `0xfffffe000bc9a504`,
`…WorkSubmit` `0xfffffe000bc9a494`, `…WorkEnd` `0xfffffe000bc9a574`,
`…SetPowerState` `0xfffffe000bc9a1dc`.

### 3.2 What `exclaveStart` actually does (disassembled in full)

`IOService::exclaveStart(IOService *svc, IOExclaveProxyState **out)`,
`0xfffffe000bc975f8`:

1. Enters the exclaves execution domain (thread enter/exit machinery,
   incl. the "attempt to re-enter exclaves" trap) and checks platform
   readiness via the helper at `0xfffffe000b3f38a4`.
2. Reads four properties **from the IOService being started** with
   `vtable[0x308]` (property get):
   `"exclave-endpoint"` (`0xfffffe00070e7f1a`), `"exclave-service"`
   (`0x…f2b`), `"exclave-edk-endpoint"` (`0x…faa`),
   `"exclave-edk-service"` (`0x…fbf`).
   Numeric endpoints are OSData; service names are resolved through the
   exclaves service registry (`0xfffffe000b3f4b64`) — failure logs
   `"%s: %s-0x%qx could not find exclave-service %s"`.
3. `tb_endpoint_create_with_value(7, endpoint, 0)` (`0xfffffe000bd0bee8`),
   then `tb_endpoint_set_interface_identifier(endpoint, 0xbd43a9312d3b19e0)`
   (`0xfffffe000bd0bff8`), then
   `tb_client_connection_create_with_endpoint` (`0xfffffe000bd0ad8c`) and
   `tb_client_connection_activate` (`0xfffffe000bd0ae24`).
   A second, EDK endpoint is created from `exclave-edk-endpoint`.
4. Allocates the `IOExclaveProxyState` (kalloc_type view), two
   OSDictionary + lck_mtx, a workloop/command-gate pair, and returns.

**An endpoint is a boot-assigned Tightbeam port number**, not something a
driver can invent: it comes from the device tree
(`exclave-endpoint`/`exclave-edk-endpoint`) or from the exclaves service
registry by name. `IOExclaveProxyState` is the kernel object holding the
two `tb_connection_s` plus upcall bookkeeping.

The mental model from the generated code is explicit in the log strings:

| endpoint | log string | IDL client initialised |
|---|---|---|
| `exclave-endpoint` | "Endpoint for ANE firmware EC initialized" | `aneengine.aneengineinterrupt` ("Firmware EC tightbeam service AneEngineInterrupt client init failed") |
| `exclave-edk-endpoint` | "Endpoint for ANE driver EC for downcalls initialized" | `aneexclave.aneexclavexnu` ("Driver EC tightbeam service ANEExclaveXNU client init failed") |

### 3.3 Driver-side transport wiring (`ANEExclaveInit`, `0x8f680b8`)

1. if `[this+0x2d0]` (proxy) == NULL → return.
2. `proxy->exclaveSetup()` → on failure "Failed to setup Exclave connections".
3. `ANEWorkLoop::workLoop()` → `IOCommandGate::commandGate`
   (`0xfffffe000bc04298`) → `IOCommandGate::addEventSource`.
4. `IOInterruptEventSource::interruptEventSource(this,
   aneExclaveWorkerThreadEntry, provider, idx)` (`0xfffffe000bc02d74`) →
   "Setting up Exclave IOIES failed".
5. `IOFilterInterruptEventSource::filterInterruptEventSource(this,
   aneExclaveInterruptHandler, aneExclaveInterruptHandlerFilter, provider,
   idx)` (`0xfffffe000bc07b38`) → "Setting up Exclave IOFIES failed".
6. Two more IES: `aneExclaveUpcallEventHandler`, and
   `aneExclaveSecureTransitionEventHandler` ("Setting up Exclave Secure
   Transition IOIES failed").
7. `IOService::exclaveAsyncNotificationRegister(proxyState, upcallIES,
   &token)` at `0x8f687b8`.
8. Sets `this+0x3b56` = fANEExclaveInitialized; logs
   `"fANEExclaveInitialized: %d"`; then `aneExclaveWriteProperty(k,v)`.

`exclaveTeardown` destroys both endpoints ("Destroyed AneEngineInterrupt
endpoint", "Destroyed driver EC endpoint") and de-initialises both clients.

### 3.4 Is there a capability/ownership check?

- **No entitlement/task check in `exclaveStart`.** It is a kernel-internal
  IOService API; there is no task, team-ID, or entitlement compare in the
  1,300-byte body. The `com.apple.private.exclaves.*` strings in
  `com.apple.kernel` (`…kernel-domain`, `…conclave-spawn`,
  `…conclave-host`, `…boot`) belong to the user-space exclaves paths and
  are not referenced from `exclaveStart`.
- **The gate is the boot-provided name space**: without a valid
  `exclave-endpoint`/`exclave-service` (or a registered service with that
  name), `tb_endpoint_create_with_value` returns NULL and `exclaveStart`
  fails. There is no way for a kext to conjure an endpoint.
- **Driver-level gate** is `ANEHWDevice::checkExclaveEnablementStatus()`
  (`0xfffffe0008f67b90`), called from `setupANEExclaveProxyService` and
  before `ANEExclaveInit`:
  - `this+0x3b54` bit0 = "exclave supported" — must be set, else
    "Exclave not supported";
  - `this+0x3b55` bit0 = "enabled" — else "Exclaves disabled";
  - `exclaves_get_status()` must return ≤ 1, else
    "Exclave feature supported but disabled by default"
    (`exclaves_get_status` returns 0 when the exclaves subsystem is up and
    `0xff` when it is not).
- **Ownership attach**: `setupANEExclaveProxyService` first reads
  `"exclave-assigned"` from the provider via `vtable[0x2d8]`
  (`0x8f67e00`):
  - present → "Exclave assigned to ANE node (Legacy)" — the proxy object
    is allocated directly against the ANE node;
  - absent → `IOService::serviceMatching("ANEExclaveProxy")` +
    `waitForMatchingService` + `registerInterestedDriver`
    (`0xfffffe0008f67b24`) → "Exclave assigned to ANEExclaveProxy node."
- `start()` calls `ANEExclaveInit` only when a flag on the load-balancer
  object (`[x24+0x599]` bit0) is set (`0x8f56df0`), and a failure there is
  logged but **does not abort `start()`**.

J700's ADT has `exclave-assigned` (zero-length/true), so it takes the
"assigned to ANE node (Legacy)" branch.

---

## 4. Is non-owner access rejected or faulted?

### 4.1 What the kernel itself still does to the engine

`ANEHWDevice::ANE_Init` (`0xfffffe0008f3dc28`, 0x3500 bytes):

```
0xfffffe0008f3dd28  ldrb  w8,[x19,#0x200]      ; fRTBuddy flag
0xfffffe0008f3dd2c  tbz   w8,#0 -> 0x8f3dd70
                    ...  InitializeRTBuddyEndpoints()   ; RTBuddy branch
0xfffffe0008f3dd70  InitANEScratchRegisters()      ; legacy branch
0xfffffe0008f3dd84  ANERegisterControl::write32(...)
                    ...  RVBAR write64, CPU_CONTROL write32 x2, SCRATCH7 poll
```

Byte 512 is set in exactly one place in the whole kext:
`InitializeProvider+0x248`, `0xfffffe0008f52b84`, on the
`provider->getName() == "RTBuddyService"` branch only. For an
`AppleARMIODevice` provider (`ane,t8132exclave` on J700) byte 512 stays 0,
so **`ANE_Init` runs the legacy direct-boot path**: scratch registers,
RVBAR, `CPU_CONTROL` 0→0x10, SCRATCH7 poll. `ANE_LoadFirmware_gated`
(`0x8f46194`) also branches on the same byte and skips the download when
RTBuddy is in use (J700's ADT says `pre-loaded = 1`, i.e. iBoot stages the
image).

Power sequencing is likewise direct, and unconditional with respect to the
exclave:
- `EnableCPUClocksAndPower` writes PS `0xc000`, PWGATE `0x268c/0x26b0`, ASC
  `0x1600044`;
- `EnableANEClocksAndPower` / `DisableANEClocksAndPower` vote clocks/power
  through the provider and only *additionally* call
  `ANE_ExclavePreparePowerOff` (`0x8f2af7c`) on the disable path;
- `ANE_ExclaveModeCycle` (`0x8f69830`) sends a **mailbox command**
  (`aneCmdSend`, `0x8f69cfc`) to move the firmware between SECURE and
  EXCLAVE modes, and `ANE_PowerOn_gated` gates it on the caller name
  `"PearlSEP"` (`0x8f5b950`).

`aneExclaveInterruptHandler` (`0x8f68b58`) reads the ANE interrupt-status
register through `ANERegisterControl::read32` (`0x8f68c48`) before it
forwards the ITQ event to the exclave.

### 4.2 No firewall code, and no ownership precondition on register access

- Nothing in `com.apple.driver.AppleH16ANEInterface` tests
  `exclave-assigned`/`fExclaveInitialized` before constructing
  `ANERegisterControl` objects, reading, or writing the engine/PS/PWGATE
  windows. The exclave flags gate the *proxy*, not the register path.
- The `PTD` register control (the only thing in this driver that could be
  a protection-table updater) is built only if byte 14036 is non-zero, and
  there is no writer for that byte in this build (prior receipt, confirmed
  unreachable here).
- The string `"exclave-reg"` does **not** occur anywhere in the
  kernelcache: no kext and not XNU looks that window up by name. Nothing in
  the cache maps, writes, or validates `0x4_41c00000+0x88000`.
- `ApplePMP` (`com.apple.driver.ApplePMP`) is a power/thermal management
  processor kext (thermal user clients, `powerStateHandler`), not an MMIO
  firewall; `/arm-io/pmp` is `iop,ascwrap-v6`, role `PMP`.

### 4.3 The isolation that *does* exist: DART / IOMMU

`/arm-io/dart-ane` (raw ADT, cells decoded little-endian):

```
sid          = 0x0 0xb 0xf 0xa 0xd          <- AP stream IDs
exclave-sid  = 1 2 3 4 5 6 7                <- exclave stream IDs
sid-count    = 0x10
manual-availability, bypass-10, bypass-13
children: mapper-ane, mapper-ane-iso1 … iso7
```

Seven stream IDs and seven dedicated mapper nodes are reserved for the
exclave, and the exclave IDL carries `aneenginesidsetup` /
`aneengineisolationcontextsetup|teardown`. So DMA/IOMMU is where ownership
is actually enforced — not the CPU's MMIO path.

---

## 5. What state the exclave owns

| Domain | Owner | Evidence |
|---|---|---|
| PMGR clock/power votes, genpd, PSD | **kernel kext** | `enableDeviceClock`/`enableDevicePower` (`0x8f2a034`, `0x8f2a090`), no exclave precondition |
| Register windows (engine, PS, PWGATE, ASC) | **kernel kext** | `ANERegisterControl` built in `start` regardless of exclave; `ANE_Init` legacy path |
| Firmware image | **iBoot**, pre-staged | ADT `pre-loaded = 1`, `segment-ranges`; `ANE_LoadFirmware_gated` skips download when RTBuddy; Linux saw RVBAR latched `0x10000000001`, CPU stopped |
| Firmware boot/run/mode (SECURE↔EXCLAVE) | **kernel kext issues it**, exclave coordinates | `ANE_ExclaveModeCycle` → `aneCmdSend` mailbox cmd; `aneSEPToExclaveHandoff` / `aneExclaveToSEPHandoff` |
| Request submission (`aneCmdSend`, mailbox, DART maps) | **kernel kext** | `SendRequestToFirmware`, `ANE_ProgramSendRequest*`, `dartMapMemoryDescriptor*` |
| Completion/result tracking | **exclave** | `queryForResult` RPC; `aneExclaveQuery` on every worker-thread wake |
| Interrupt (ITQ) delivery and upcalls | **shared** | kernel reads status reg, then RPC `HandleITQInterrupt`; `exclaveAsyncNotificationRegister` for upcalls |
| Firmware state save/restore, pause/resume of work | **exclave** (kernel asks) | `saveExclaveState`, `restoreExclaveState`, `pauseExclaveWork`, `resumeExclaveWork` |
| Firmware recovery brackets | **exclave** (kernel asks) | `startFirmwareRecoveryProcess`, `endFirmwareRecoveryProcess` on timeout |
| Power-off quiesce acknowledgement | **exclave** (kernel asks) | `preparePowerOff` from `DisableANEClocksAndPower` |
| Persistent-client bookkeeping / upcall ack | **exclave** | `persistentClientUpcallAck`, `ANE_AddPersistentClient_gated` |
| Debug property read/write | **exclave** | `debugreadproperty`, `debugwriteproperty` |
| DMA stream IDs / isolation contexts | **exclave** (SIDs 1–7) | ADT `exclave-sid`, `mapper-ane-iso1..7` |

In one line: **the exclave owns the ANE firmware's lifecycle state and the
completion/notification channel; the kernel kext owns the power, clocks,
MMIO windows, and the mailbox command path.**

---

## 6. Verdict on the hypothesis

Hypothesis as stated: *"the ANE is exclave-owned, so direct engine access by a
non-owner is gated or would fault."*

Two halves, two different answers.

### 6.1 "Non-owner cannot join the exclave service" — **SUPPORTED**

Proven from this cache:
- `ane,t8132exclave` is declared `IOExclaveProxy = true` (§1.1), and a second
  personality `ane,ane-exclave-proxy` exists for the proxy object.
- The kernel reaches the exclave only through `IOService::exclaveStart` →
  Tightbeam endpoints whose numbers/names come from the device tree or the
  exclaves service registry (§3.2). A kext with no such property/registration
  gets `NULL` and cannot start.
- The exclave commands the driver (save/restore state, pause/resume work,
  firmware recovery, power-off prepare) and tracks completions; the kernel
  polls it for results (§2.1, §5).
- The DART reserves SIDs 1–7 plus seven mapper nodes for the exclave (§4.3).
- On this SoC the ANE node carries `exclave-assigned`, `exclave-service =
  com.apple.service.ANEExclave`, `exclave-edk-service =
  com.apple.service.ANEExclave_EDK`, `exclave-reg = 0x4_41c00000+0x88000`.

### 6.2 "Direct engine-window access is gated or faults" — **CONTRADICTED**

Proven from this cache (and from the repo's own device results):
- Apple's own kernel maps and writes the same engine/PS/PWGATE windows on
  the same SoC while the exclave is assigned: `ANE_Init` legacy path,
  `EnableCPUClocksAndPower`, `ANE_ExclaveModeCycle`'s mailbox command,
  `aneExclaveInterruptHandler`'s status read. No code makes those conditional
  on exclave ownership.
- `ANE_Init`'s direct-boot path is selected by the **RTBuddy** flag
  (`+0x200`), not by any exclave flag; on an `AppleARMIODevice` provider that
  flag is 0, so the direct path is the *normal* path on J700.
- `start()` logs and continues when `ANEExclaveInit` fails — the driver is
  designed to run without the exclave.
- Nothing in the cache programmes an AP-access firewall for the ANE window,
  and `exclave-reg` is not referenced by any code in the cache.
- Independently, the repo's own device work already showed AP-side engine
  reads succeeding (stage 4: `CPU_STATUS=0x30006a`, `RVBAR=0x10000000001`) and
  mailbox reads succeeding (stage 3c) — with no SError, no panic.

### 6.3 Overall

**Undetermined for "faults", leaning contradicted**; **supported only in the
service/ownership sense**. There is no static evidence of a hardware or
software gate on AP MMIO to the ANE engine window, and there is positive
evidence against one (Apple's own kernel does it, and so did Linux). What is
gated is the exclave RPC channel: without the boot-assigned endpoints/service
name a driver cannot enter exclave mode, cannot be told to pause/resume, and
will not receive ITQ/upcall notifications — it is a client of a protocol it
cannot join, not a CPU that gets fenced out.

Practical consequence for the Linux driver: the intermittent PMU-level resets
are better explained by *sequencing* (unvoted clock ids 327/326/325, the
`pmp-virt-notify`-gated second clock vote, genpd ordering) than by a
protection fault, because a protection fault would be deterministic and would
have shown up as an SError/panic rather than a PMU boot error with no software
trace. Nothing in this analysis makes an engine access *safe*; it only removes
"the exclave fences the AP out" as the explanation.

### 6.4 Limits of this analysis

- Static only. No exclave-side binary, no iBoot, no ADT runtime state: the
  endpoint *values* and the exclave's own code are not in this artifact.
- `exclave-reg`'s physical meaning is unproven; the ADT cell decode for
  `reg`/`exclave-reg` is taken from the team's recorded
  `data/ane-soc/t8140.json` (I re-read the raw property bytes and the
  strings, which are unambiguous; the 64-bit cell packing is not).
- Direction of the `aneengine.aneenginerequest` IDL is inferred from naming
  and server stubs, not from a call site.
- Absence of a gate in this one kernelcache is not proof that no gate exists
  on another firmware/OS build.
