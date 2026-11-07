# AF scan preparation — two corrected configuration-register reads

## Correction before constructing a probe

The proposed `0x11378b8..0x1137ac8` engine offsets are unverified.
The earlier HAL report also identifies `0x11378b8` as a kernelcache
**file offset** for tunable records. The later report explicitly leaves
the record stride unresolved. Do not treat these numbers as engine
register addresses until the applier and table layout are independently
reconciled. The prior zero readback cannot establish a dead AF block.

The tunable probe has another address error: `ioremap(0x1984000, ...)`
maps a physical address without adding the engine resource base. Its
freeze is not evidence that the ANE master clock wrapper is inaccessible.

## Proposed read-only experiment after resolving the addresses

1. Produce an explicit small allowlist of verified register offsets,
   widths, source instruction addresses and artifact hashes. Check for
   read-to-clear/FIFO semantics. No sweep over an entire unknown block.
2. Review the power/CE prerequisites separately; a read-only engine
   experiment still changes hardware through domain votes. Do not call it
   hardware-read-only or a dry run.
3. Map the engine DT resource, bounds-check each offset and width, and
   access only `engine_base + verified_offset`. No bare-offset ioremaps.
4. Before loading, save the exact command, boot ID, source commit, module
   hash and intended reads to a new run directory; fsync files and the
   directory, push source, and sync disk. Fail closed on logging errors.
5. Persist BEFORE and AFTER records around each individual access. A
   missing AFTER identifies an interval, not necessarily the instruction
   causing a reset. Kernel log writers must check short writes/fsync
   errors and bound formatted output to the actual buffer size.
6. One staged experiment per fresh boot; stop on prerequisite failure.
   No CPU start, RVBAR writes, scratch writes or speculative tunables.

## Interpretation

Zero, nonzero, and unchanged reads are observations only. None alone
proves clock state, register implementation, write acceptance, firmware
execution or long-term stability. Compare only source-qualified fields.

## Address verification and prepared implementation

Rechecked retained kernelcache SHA-256:
`7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe`.
Source context: HAL report at repository commit `380aae7` (same report
introduced earlier), independently checked against the binary here.

The configurations contain references to **12-byte descriptors**, not
register offsets. Descriptor first word: low 28 bits are offset; high
4 bits are width in bytes. `_tunableh_apply_configuration` at
`0xfffffe00097a87c0..0xfffffe00097a87c8` extracts these fields and
`0xfffffe00097a8810` adds offset to context base. The width-4 store is
`0xfffffe00097a8974`. AF block base is engine-relative zero in the HAL
report. Two configuration descriptors selected for this sample:

| Descriptor file offset | Raw words | Engine offset | Width |
|---|---|---|---|
| `0x11378b8` | `40000000 0061c3c0 00180000` | `0x0` | 4 |
| `0x1137ad4` | `40000338 0061c530 00180000` | `0x338` | 4 |

The second is reached by the configuration reference at
`0xfffffe0009799648` (configuration file offset `0x1137c10`, whose
first word is a reference to `0x1137ad4`). These are tunable configuration
fields, not identified FIFO/pop registers. That is source evidence, not
proof of read safety on Linux. No hardware access occurred to verify it.

`ane_h17_af_scan.c` reads just these two words after genpd votes, with
ACTUAL checks, using the DT engine resource plus offset. It deliberately
does not perform the disputed HAL program or CE handshake. Thus its
observations describe the genpd-only state, not Apple's initialized state.
Release uses genpd; this does not establish that power sequencing is fully
qualified. Existing freeze risk remains.

Build: `make -C ane/h17 KERNELDIR=/lib/modules/$(uname -r)/build`.
Preparation check (no device access): `python3 ane/h17/run-af-scan.py`.
After committing/pushing and a confirmed fresh cold boot, execution is:
`sudo python3 ane/h17/run-af-scan.py --run --cold-boot-confirmed`.

The launcher saves identities and the exact insmod command before loading,
fsyncs the log and directories, syncs disk, and reserves this boot's run
directory exclusively. It refuses existing logged H17 activity. A cold
boot cannot be proved by Linux boot ID; the operator must confirm it.
No automatic retries and no sleep-based observation. Preparation does not
load a module. Logs go under `/var/log/omarchy-ane/af-scan-BOOT_ID/`.
