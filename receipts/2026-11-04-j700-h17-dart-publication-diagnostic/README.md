# H17 DART publication diagnostic — offline draft reviewed

Status: compile-only, independently reviewed; NOT installed or armed.
No device access, module unload, task, doorbell, or reboot was performed.

## Source and artifacts

- Exact installed-package source pin:
  `4fdd0fc5c931ef9f14474942b37c3d451e97fc04` (recipe provenance in the
  rung-1 receipt, addendum 5).
- Worktree: `/home/admin/aurora-build/wt-dart-diag`, branch
  `diag/h17-publication-pinned`; patch remains uncommitted there.
- `publication-diagnostic.patch`: diagnostic diff against that pin.
- `build.log`: successful LLVM18 `W=1` compilation of `apple-dart.o`
  using installed `7.2.11-14-1-ARCH` headers. This is an external-object
  syntax/type check, not a full built-in kernel build or link test.
- `capture-tests.log`: test of the extracted actual C capture helper:
  four-level slots 16/63/0, three-level high-address TTBR bounds, missing
  roots, retired ownership pointer, zero tables, unsupported levels.
  The test compiles with warnings as errors and undefined-behavior
  sanitizer. Script: `ane/h17/test_dart_publication_capture.py`.

## Diagnostic behavior and limits

`h17_pub_diag` is false by default, with a read-only parameter. It is
intended for boot-time opt-in in a future built-in diagnostic kernel;
no boot configuration has been changed. Scope additionally requires
`apple,t8140-dart`, locked state, exactly SID 0 and one of the three
device names `401800000.iommu`, `401820000.iommu`, `401840000.iommu`.

At the existing locked-sync exit, up to 64 snapshots per DART capture
already mapped host/live/ownership root words under `dart->lock`.
Candidates are the **rung-1** stage DVA, intended reply DVA and printed
low-32 candidate. These are not pointers to dereference and are not
claims about future allocation addresses. Indexing follows the domain's
software level count; bounds checks precede table accesses. Output occurs
outside the lock and includes sequence, sync result, deferred flag, return
code, level/table counts, candidate DVA, index/slot, presence mask and words.
Presence bits 0/1/2 mean host/owned/live respectively; zero words alone
must not be interpreted as missing pointers. An owned pointer without a
live root is deliberately not sampled because retirement can free it
outside the lock after clearing live.

No new MMIO reads or writes are introduced. Existing publication, TLB,
exception acknowledgement and unlock behavior remain in place. Logging
can affect timing; this is not an assertion of runtime transparency.
The output tests publication-state hypotheses only, not exception-field
semantics, hardware TCR level count, or a complete device page-table walk.
No descendant tables are followed. The 64-snapshot limit may omit later
events; absence of a log is not a successful publication receipt.

## Independent review

Review session `ses_e5636db31ffeVNWuBTQkH5Ft5g` first requested changes:
an ownership-table dereference could race retirement after live cleared.
Fixed by requiring `live && owned` under the lock and adding a regression
with an invalid residual owned pointer and NULL live root. Tests and
LLVM18 compilation were rerun. Re-review: **APPROVE**, no remaining findings.
This is source review, not deployment authorization or device qualification.

## Reproduction and next gates

```
python3 ane/h17/test_dart_publication_capture.py /home/admin/aurora-build/wt-dart-diag/drivers/iommu/apple-dart.c
make -C /lib/modules/7.2.11-14-1-ARCH/build M=/var/tmp/opencode/h17-pub-compile LLVM=/usr/lib/llvm18/bin/ W=1 apple-dart.o
```

The external compile directory contains the patched source, its exact-pin
`dma-iommu.h` and a Makefile containing `obj-m := apple-dart.o`. No module
was linked or loaded. Before hardware: build and link a matching built-in
kernel, review the boot/deployment plan and fail-closed runner identity
checks, preserve the known-good boot, and stage the fresh-boot rung.
The failed boot's ANE module must stay pinned until the authorized reboot.

## Offline observation parser

`ane/h17/dart_publication_observations.py` validates saved `h17-pub`
records without opening a device. Usage:

```
python3 ane/h17/dart_publication_observations.py saved-dmesg.log --page-shift 14
make -C ane/h17 dart-publication-test
```

The page shift is an explicit caller assumption, not a hardware measurement
inferred from these logs. The parser rejects malformed/out-of-scope records,
missing/duplicate candidates, missing sequence numbers, contradictory
geometry, inconsistent snapshot metadata and words for absent tables. Same
root indexes must share a presence mask; a deferred-missing-root result
requires the deferred flag. Complete records are labeled `PARSED`, never
hardware PASS; `hardware_qualification` is always false. It reports missing
devices, the sequence cap and equality/difference/unavailability of words.
It does not replace the table-discovery judge or certify a complete capture
after the cap. Interleaved output is grouped by device and sequence.

Eleven unit tests pass. They are included in `sim-all`, which also passed
after integration; output retained in `publication-sim-all.log`.
Independent review session `ses_e562a6492ffes4dV3eZpRJ3iZt` identified the
shared-root presence and deferred-flag consistency issues; both were fixed
with regressions and re-review returned **APPROVE**. No device evidence was
created by parser tests.

## Standalone read-only identity preflight

`ane/h17/diagnostic_preflight.py` is a future-run identity check, **not yet
wired into the hardware runner**. It cannot arm, load/unload modules, submit
work, or create pending markers. There is no approved identity manifest yet;
one requires a completed full build, reviewed artifacts and a matching ANE
module. Do not fill those values with guesses or fixture values.

The CLI requires a root-owned regular manifest, opened without following
symlinks and checked by file descriptor; group/world-writable manifests are
refused. It requires the reviewed source/patch, exact diagnostic release,
the runtime GNU build ID from `/sys/kernel/notes`, enabled diagnostic
parameter, J700/T8140 identity, a boot other than the failed/pinned rung-1
boot, no loaded `ane`/`ane_*` module, matching module vermagic release, and
SHA-256 matches for the installed kernel image, ANE module and firmware.
It reads metadata/files only, not device MMIO. `MATCHED` retains
`arms_rung: false` and does not replace the one-attempt marker, full DT/PM
checks, clean/pushed checkout or hardware runner's other gates. It is not
a secure-boot attestation or permission to use a result read after failure.
Caller-supplied module paths and later loading must be kept stable by a
future reviewed runner; this standalone check does not prevent substitution
between a successful check and a later command. Build-ID and manifest trust
are explicit assumptions; the checker cannot prove its own running code.

Twelve offline fixture/CLI tests pass, included in `sim-all`. Tests cover
failed boot, runtime/build/parameter/board/module mismatches, each artifact
hash, malformed/truncated/duplicate notes, manifest symlinks, ownership and
permissions, malformed JSON and modinfo failure without root privileges.
Review session `ses_e56212d6dffep2GOlpRXOxkFpH` returned **APPROVED WITH
NITS**, no blockers; the requested CLI trust-boundary tests were added and
the release-mismatch test was unmasked. No live preflight, approved manifest,
boot change or hardware run was performed to test this checker.
