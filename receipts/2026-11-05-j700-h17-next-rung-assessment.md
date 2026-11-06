# J700/H17 post-table-discovery rung assessment

Status: offline assessment only. No module was unloaded, no reboot was
requested, and no device/register access was performed for this assessment.
The current boot has already released the firmware CPU for table discovery;
keep the resident module, mappings, and power intact until reboot.

## Observed stopping point

The corrected table-discovery run on boot
`ba568e3c-69f8-44c4-8e45-5698ee16b5e6` passed. Its retained verdict and logs
are in `/var/lib/omarchy-ane/h17-table-attempts/<boot-id>/`. At source
commit `29cce360d90e038c3e2b19d70af5e6e147170cc7`,
`ane/h16/ane_h16_main.c:1290-1391` allocates and maps DM/IPC/HEAP, publishes
the initialization descriptor, requests and copies the firmware channel
table, logs its eight records, then returns at the explicit
`H17 TABLE_STOP before SCRATCH3 registration` boundary. The offline judge at
`ane/h17/table_discovery_verdict.py:86-110` validates the reply range, table
size/count, record syntax, protocol ordering, and that stop marker. It does
not validate channel-buffer ownership or exercise registration.

The passing device log establishes this boot's runtime reservation,
DMA-backed alias mappings, firmware wake, and owned IPC table copy. It does
not establish that channel buffers are initialized, that registration is
accepted, or that the firmware command path works.

## Next rung is not implemented

There is no separate registration runner or registration verdict in
`ane/h17/`. The existing `chanreg=1` parameter names the table-discovery rung;
its description explicitly says it stops before registration. Reusing that
parameter with additional behavior would silently widen the reviewed stage
and is not appropriate.

The firmware trace receipt
`receipts/2026-11-04-j700-h17-firmware-load-path/README.md`, sections 14-16,
describes the later handshake: initialize the per-channel state before
publishing the registration word, then wait for firmware to clear it. That
receipt also records the required channel names and rejects the earlier
empty-buffer proposal. The H17 discovery output currently exposes channel
names, types, sources, slot counts, and buffer addresses as observations; the
driver deliberately does not dereference those buffers.

## Offline source contract: host initialization writes

The assessment above is narrowed by the pinned firmware analysis
`receipts/2026-11-04-j700-h17-firmware-load-path/README.md` §14.2-14.3,
based on kernelcache SHA-256
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
The trace shows that the constructor seeds `slots` control blocks of 0x40
bytes only for type-0 entries; for each block it stores the source-traced
initial state and invokes the cache-clean hook. It also identifies
`SHAREDMALLOC`, `TERMINAL`, and `BUF_T2H` as required names before
`ANE_Init` continues. This is source analysis, not a device observation of
registration.

An offline-only validator was added in `ane/h17/chanreg_model.py` as
`registration_init_ranges`, with synthetic tests in
`ane/h17/test_registration_model.py`. Its fixture is taken from the retained
successful table log. The model proves only that the host's source-traced
type-0 initialization ranges are aligned, non-overlapping, and inside the
owned **logical** IPC extent (not merely DMA mapping padding). It deliberately
does not validate or bless firmware-side accesses to the other channel
buffers, and it is not wired into the driver or a hardware runner.

## Remaining offline gates before proposing registration hardware

1. Resolve from the pinned analysis the complete firmware-side access
   contract for every non-type-0 channel buffer: byte ranges, producer/
   consumer fields, ownership, and ordering. The copied table contains
   pointers and counts, but the current stage never dereferences these
   buffers; the host-initialization model is not evidence that firmware-side
   accesses stay within owned IPC.
2. Extend the offline model to validate all eight records and every complete
   firmware-accessed range against owned DMA allocations, including overflow,
   overlap, granularity, and firmware address-width constraints. Do not infer
   lengths from adjacent addresses or slot counts without source evidence.
3. Add negative and mutation tests for malformed names/records, missing
   required channels, out-of-range/overlapping buffers, partial allocation or
   mapping, bad reply/ack, and timeout. Specify retention on every failure
   after firmware CPU release; no unmap/free rollback is safe without separate
   evidence.
4. Implement a new explicit default-off stage and runner. Preserve the
   already-tested table-discovery-only mode. Stop immediately after the
   registration acknowledgement; do not add a doorbell, task, IRQ ack, or
   numerical execution to that rung.
5. Have the new stage independently reviewed, build the exact pinned module,
   and add a one-shot preflight/attempt marker and verdict that fail closed on
   all identities and observed outputs. Only then present the concrete rung
   and its risks for separate authorization.

These are design gates, not evidence that the registration operation is
safe or that any next hardware rung is authorized. A future hardware attempt
requires a fresh boot; never unload the currently resident `ane_h16` module.
