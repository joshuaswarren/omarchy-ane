# H17 table discovery: kernel implementation, NOT a hardware result

Continues `e1c4839`'s offline descriptor model. No module was loaded, no
table-discovery marker created, and no reboot requested while implementing
this change. Boot remained `0855c23b-dce5-47b0-83c5-a19baef2beef`.

## Scope and first-run envelope

`ane_h16.ko optin=t8140 stage=boot chanreg=1` is a new **default-off** rung.
`chanreg=0` retains the existing wake-only allocation/mapping sizes and
result grammar. Wrong SoC or stage is refused before mapping registers.

The rung accepts only the recorded initial scratch geometry from `6610ec7`:
count 8, IPC last offset `0xc440`, optional HEAP request `0xf00000`.
It is deliberately narrower than the general offline model. The default
options block uses flags 4 and firmware argument zero; this is a tested
construction choice from the traced defaults, not a measured macOS options
block or device qualification.

After wake it allocates coherent DM (256 KiB), IPC (requested `0xc441`,
mapped `0x10000`), HEAP (15 MiB), and a host table-copy buffer. It verifies
DMA handles are aligned, inside the source-pinned DART aperture, disjoint
from staged firmware and other buffers, and translated for every page.
It does not invent fixed IOVAs or treat DMA handles as physical addresses.

The descriptor is encoded using `ane/h17/chanreg.h` at DM offset zero.
After `dma_wmb`, the module publishes SCRATCH0/1 and requests the table
through SCRATCH7 `0xf7fbdff9`. A bounded one-second poll waits for the
second `0x08042006` reply. It rejects any reply whose **entire 0x800-byte
table** does not fit inside the logical IPC extent (mapping padding is
not valid storage). After `dma_rmb`, it copies once and logs SHA-256 plus
eight opaque entries. Names are hex-encoded; observed channel-buffer
addresses are never dereferenced.

Successful RESULT is specifically `PASS reason=boot-table-discovery`,
not `boot-wake`. Timeout, allocation failures and bounds refusals produce
their own verdicts; none falls back to a wake PASS. Every post-release
outcome retains all allocations, binding and power until reboot.

**It stops before SCRATCH3 registration.** No per-channel buffer writes,
doorbell, IRQ-ack access or task submission are added. Table discovery is
not command or task qualification.

## Firmware-arena padding correction

Integration exposed a previous receipt arithmetic error:

```
image segments:  0xc0000 + 0x2ac000 = 0x36c000
nominal FWIM arena:                  0x380000
difference:                          0x014000
```

The descriptor advertises the nominal arena. For **chanreg=1 only**, the
stage allocation and second alias mapping therefore include zeroed owned
backing through the full `0x380000` arena. The code requires the expected
preload segment sizes and contiguous IOVAs at `0x10000000000`; the complete
extended alias must be reserved before DMA allocation. No extra preboot
physical memory is read or claimed: the tail is owned coherent allocation.
Unwind uses the actual mapped prefix, including the extension.

Read-only inspection of the running boot's DT found the full reservation:

```
/proc/device-tree/reserved-memory/ane-fw-alias-iova/iommu-addresses
cells: 0xb8 0x100 0x0 0x0 0x380000
```

That is DT evidence only, not an execution result. The earlier wake path
continues to stage/map only its two original image segments.

## Validation

Commands and retained outputs:

- `make -C ane/h16 sim` — **34 scenarios pass**; original mutation gates
  remain effective. New mutations caught: start-pointer-only table check,
  missing descriptor DMA barrier, premature SCRATCH3 registration, and
  advertised-but-unmapped FWIM padding. `sim-summary.txt`.
- `make -C ane/h17 sim-all` — existing simulator/planner/ordering gates
  plus offline descriptor and verdict tests pass. `h17-sim-all.txt`.
- `make -C /lib/modules/$(uname -r)/build M="$PWD/ane/h16" W=1 modules`
  — succeeds. GCC differs from the kernel's Clang toolchain, producing
  compiler/header attribute warnings; no warnings originate in driver
  sources. `build.log` records the actual output; this is not a claim of a
  warning-free toolchain or device qualification.
- `bash -n ane/h17/run-table-discovery.sh` — passes.

The simulated firmware checks **every descriptor byte against an independent
store-list oracle**, then supplies a synthetic IPC table. Foreign channel
buffer pointers in that table are intentionally invalid: the positive test
must still succeed without following them. Negative scenarios cover timeout,
one-byte overrun, foreign reply, mapping-padding reply, changed input geometry,
partial allocation failure, out-of-aperture DMA, wrong SoC, and a reservation
that covers the image but not the advertised arena. Tests count accesses of
any value, not just writes of zero. They require exactly the original boot
scratch writes plus the three publication writes, no SCRATCH3 registration,
and no doorbell or IRQ-ack access.

## Separate runner; not armed

`ane/h17/run-table-discovery.sh --run` requires root, a separate
`/var/lib/omarchy-ane/h17-table.pending` marker, the clean/pushed-tree
`prepare-run.sh` gate, and no loaded ANE module. There is **no default action**
and no installed/enabled table-discovery service. The existing boot-wake
service is unchanged and cannot opt into `chanreg`.

The runner refuses an existing per-boot evidence directory and records a
failure verdict before `insmod`, so incomplete runs cannot inherit PASS.
Its Python judge requires exactly one preparation/copy/stop/RESULT marker,
eight uniquely ordered channel observations, exact sizes, full IPC bounds,
valid/disjoint preparation extents, and a table-specific PASS. Duplicate,
missing, malformed and wrong-rung markers fail closed. These are log checks;
the simulator's event recorder is the access-count check.

**Next hardware gate:** independent review of this implementation and its
runner, findings fixed, then a separately armed fresh-boot rung. No hardware
run is claimed by this receipt.
