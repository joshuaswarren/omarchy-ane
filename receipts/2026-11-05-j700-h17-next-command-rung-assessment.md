# J700/H17 next command-rung assessment

Status: offline/source assessment after the channel-registration PASS on
`8ec803ee-85f7-434c-8a5e-19317934c9c9`. No further ANE hardware access has
been performed. The `ane_h16` module remains bound with firmware-visible
mappings retained until reboot.

## What registration completed

The previous rung observed the firmware clearing SCRATCH3 after the host
registered the validated eight-entry channel table. The captured log records
the table and all channel records, the four type-0 buffer-initialization
ranges, `H17 CHANNEL_REGISTER_ACK value=0`, and the explicit stop marker. It
did not send a message through a channel or acknowledge an IRQ. See
`receipts/2026-11-05-j700-h17-channel-registration.md` and its root-owned raw
evidence directory.

## Next boundary in the pinned trace

The retained J700 kernelcache trace is SHA-256
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`;
decoded excerpts are in
`receipts/2026-11-04-j700-h17-firmware-load-path/analysis/`.

After registration, Apple's `ANE_Init` resolves `SHAREDMALLOC`, `TERMINAL`,
and `BUF_T2H` to their published source indices (trace §14.3). A command
request then uses the IO channel's shared-memory FIFO, with 64-byte slots;
the static sender writes two 64-bit payload words, orders the slot publication,
and updates a producer word (trace §13.2 and
`analysis/h17-messenger-irq-chain.txt`). Firmware completion arrives through
the IRQ status word at `0x188c000`; Apple's handler writes `0xffff` to the
W1C acknowledgment register at `0x1890000` before channel dispatch (trace
§13.3).

## Why no command is submitted yet

The static record does **not** establish a safe initial command:

- The command/message ID and its two payload words for any no-side-effect
  request have not been pinned to an observed J700 exchange.
- The helper trace now identifies initialization command *families*, but not
  a proven-safe first command. `ANEFirmwareManager::sendPremapCmd`
  (`0x8f3d1c4`) constructs command `0x020d`, sets a 16-bit field to `1`,
  passes a 0x3490-byte command structure, and calls `aneCmdSend` with a
  0x3490-byte response buffer and synchronous flag `2`. Its four trailing
  arguments are zero. `sendSetupCmd` (`0x8f3d414`) constructs command IDs
  `0x0400`–`0x0403` with payload sizes 0, 12, or 16 bytes; response handling
  copies a returned word for `0x0400`. These are firmware setup exchanges,
  not evidence of no side effects. The recorded disassembly is
  `receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-premap-setup-disasm.txt`
  (kernelcache SHA-256 given above). The `ANE_Init` excerpt at `0x8f3fc70`
  also shows command `0x0021` with a 12-byte payload when `[dev+0xc8]` is
  nonzero, followed by setup commands `0x0400` and `0x0401`; it is
  configuration-dependent and cannot be assumed harmless.
- The sender's producer word can hold an inverted value while a peer
  acknowledgment is pending. The control-block producer/consumer
  synchronization is not established for a Linux-owned endpoint; a second
  send cannot be inferred safe from the enqueue primitive alone. The Apple
  retry wrapper is now disassembled at
  `receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-retry-disasm.txt`:
  it calls the FIFO primitive and derives
  its retry budget from device state (`dev+0x194 * 0x32`), with conditional
  wait/retry paths. That implementation-specific policy is not sufficient to
  define Linux retry semantics or a safe retry after an uncertain send.
- The IRQ handler's `0xffff` W1C write acknowledges all lines, and its
  dispatch depends on channel-index state. A speculative send or ACK could
  consume unrelated firmware state or strand a message.
- No first command is known to be side-effect-free. A task submission is
  explicitly outside this boundary.

These points are source analysis, not observed device failures. The already
observed registration ACK does not qualify the message queue, IRQ path, command
IDs, or compute path.

## Caller-context findings (offline)

The symbol/call-site scan and focused excerpts are from the same pinned
kernelcache; relevant addresses and decoded call arguments are captured in
`receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-premap-setup-disasm.txt`.

- `sendPremapCmd` has a single direct caller, `initializeInferenceRequestMemoryPool`
  at `0x8f3d13c`. That caller obtains a size from an object/vtable call and
  supplies premap type `2` and flag/value `1`. It is specifically part of
  inference request memory-pool initialization, not a generic firmware-ready
  handshake. Command `0x020d` is therefore not a suitable probe.
- `sendSetupCmd` call sites are in program/process lifecycle functions:
  process create, abort, program load/unload, and release paths. The shown
  abort path sends `0x0402` with two words derived from process state. This
  confirms that the helper's small fixed payloads participate in stateful
  operations; command ID alone does not imply query semantics.
- In the traced `ANE_Init` continuation, a configuration-gated command
  `0x0021` sends a 12-byte payload derived from `[dev+0xc8]`; after successful
  completion it issues `0x0400` and `0x0401`. The path is conditional and
  mutating/configuration semantics are unresolved. It cannot be lifted out
  of `ANE_Init` as a standalone command without proving preconditions and
  side effects.
- `aneCmdSend` registers an outstanding command before enqueue and then waits
  through its completion machinery. The response handler matches incoming
  command state to an outstanding request and invokes completion; this is a
  request/response transaction, not an enqueue-only notification. The
  synchronous completion path and IRQ dispatch are consequently inseparable
  from any proposed command rung.

Result: caller analysis has ruled out the apparent setup/premap candidates as
safe post-registration probes. It has not established a no-side-effect
command. Continue with firmware-side command dispatch semantics (including
response IDs/status fields) and Linux-owned IRQ/queue synchronization; do not
choose `0x0400`, `0x020d`, or `0x0021` merely because they appear during
initialization.

## Firmware-image command inventory check

The retained J700 image is
`/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho`, SHA-256
`32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c`.
Its string table contains an ANE controller command vocabulary including
`CSNE_CMD_PING`, `CSNE_CMD_CONFIG_GET`, `CSNE_CMD_PLATFORM_INFO`,
`CSNE_CMD_PREMAP_BUFFER`, power controls, and program/process lifecycle
commands. It also contains handler assertions describing input/output size
checks for commands such as config-get and platform-info. This is evidence
that the firmware has command classes that might include queries, but strings
alone do not give their numeric IDs, endpoint, exact wire header, required
boot state, or side effects. A string match is not a safe command contract.

The repository's T6021 protocol notes identify a header-only `CSNE_CMD_PING`
(`0x11`) on endpoint 1, but those notes concern a different SoC/firmware
protocol and explicitly derive the ID and layout from the Selene image. They
are not transferable evidence for J700/T8140. The J700 kernel-side
`aneCmdSend`/`sendSetupCmd` messages also use IDs such as `0x0400` and `0x020d`,
which have not been tied to the string-named CSNE interface. Do not send the
T6021 ping or infer a J700 ID from those numeric coincidences.

Next offline step: map references to the J700 firmware's command-name tables
and dispatcher, then correlate a candidate's numeric ID and exact structure
with the host wrapper and reply parser. If the stripped image does not retain
enough relocation/table information to prove that mapping, the correct result
is that no candidate is established—not a guessed ping.

The first reference pass confirmed why the string inventory is insufficient:
the relevant command names are in the `__TEXT,__cstring` section, but the
retained `__TEXT,__text` scan did not find direct PC-relative references to
the sampled names (`PING`, `CONFIG_GET`, `PREMAP_BUFFER`, `PLATFORM_INFO`).
The image has separate `__DATA,__const` and `__DATA_CONST` sections and a
dyld-info load command, so an indirect name table remains possible; this
negative direct-reference scan is not proof that the handlers are absent.
Resolving those fixups/table references is the next appropriate static step.

## Offline gates before proposing the next hardware rung

1. Trace the concrete request creation call chain and pin the command ID,
   payload schema, expected reply, timeout behavior, and whether it has any
   firmware side effect. Refuse to proceed if no genuinely non-compute,
   bounded request can be identified.
2. Resolve queue-object initialization and the peer-side acknowledgment,
   including how it changes the inverted producer word. Build an independent
   executable model and tests for slot
   bounds, phase, wrap, backpressure, and the no-retry-after-uncertain-state
   rule.
3. Design a separate default-off rung that retains all DMA mappings and
   module ownership on every post-release outcome. It must limit the first
   request to one channel/one message and stop before task submission. Model
   and review the IRQ/ACK path separately; do not simply copy Apple's blanket
   `0xffff` acknowledgment without proving Linux ownership of every pending
   line.
4. Build a strict one-shot preflight and verdict for the exact next operation.
   Hardware execution requires a fresh boot after the current registration
   rung, independent review, and separate authorization of that concrete
   command. Numerical execution remains a later rung.

Until these gates close, the next safe work is offline trace/model work only;
do not load another module, write the shared doorbell, or access the IRQ ACK.
