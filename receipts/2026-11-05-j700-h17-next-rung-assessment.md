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

## Separate registration rung: implemented offline, not reviewed or qualified

The source now keeps `chanreg=1` as table discovery and adds a second
default-off `register_channels=1` switch, which requires `chanreg=1` and the
T8140 boot stage. After copying the bounded table, the new path validates all
eight channel ranges against logical owned IPC, initializes only the traced
type-0 ranges, publishes SCRATCH3, and stops after its clear acknowledgement.
It adds a separate one-shot runner, manifest/marker namespace, and strict log
judge. This is an offline implementation only; it has not been independently
reviewed, built as a device module, or run on hardware.

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

Sections 13.2 and 14.2 of the same pinned firmware receipt establish the
channel buffer geometry: the queue uses 64-byte entries, and each table
record's `size` is the entry count used by the channel object. The constructor
initializes those entries only for type-0 channels; the other types' buffers
are firmware-produced/consumed and are not initialized by the host there.

The offline-only validator `registration_init_ranges` in
`ane/h17/chanreg_model.py`, tested by `ane/h17/test_registration_model.py`,
uses the retained successful table log as its fixture. It requires all eight
table buffer ranges (`size * 0x40`) to be aligned, non-overlapping, and wholly
inside the owned **logical** IPC extent—not merely DMA mapping padding—and
returns the subset the host constructor initializes. This validates address
geometry against the traced access width; it does not simulate the firmware
protocol or prove actual device accesses. It is not wired into the driver or a
hardware runner.

The same bounded geometry check is implemented in the pure helper
`ane/h17/chanreg.h:ane_h17_validate_channel_table` and exercised by
`ane/h17/sim/chanreg_layout_test.c` against the retained table-log fixture.
The kernel registration path now calls that helper. The offline C simulation
models a successful SCRATCH3 clear, a timeout, malformed ranges, overlap, and
missing names; the log judge and one-shot preflight have separate tests. These
simulations prove the modeled code path only, not firmware acceptance or safe
device behavior.

## Remaining offline gates before proposing registration hardware

1. Independently review the table-entry interpretation and validator against
   the pinned trace, especially that every channel buffer's `size` is an entry
   count and that no type has a larger structure or alignment requirement.
   Unknown fields such as channel-type-specific protocol behavior remain
   outside the model.
2. Review the mutation/simulation cases and verify retention on every failure
   after firmware CPU release. No unmap/free rollback is safe without separate
   evidence.
3. Independently review the new runner, preflight marker namespace, verdict,
   and exact module build; ensure the manifest and marker are provisioned for
   the intended fresh boot.
4. Only after those reviews, present the concrete rung and risks for separate
   authorization. Do not treat host simulations or source review as device
   qualification, and do not add a doorbell, task, IRQ ack, or numerical
   execution to this rung.

These are design gates, not evidence that the registration operation is
safe or that any next hardware rung is authorized. A future hardware attempt
requires a fresh boot; never unload the currently resident `ane_h16` module.
