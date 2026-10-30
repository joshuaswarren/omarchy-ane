# J700 H17 stage 0: live boot ADT

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`.
Repository baseline `063ae8e`; aurora device-tree source
`68df87370abac4e227936552dd9be2252972e551`. The running tree has
`/soc/ane@400000000` and three `iommu@4018*` nodes, all `disabled`;
`omarchy-ane-probe` reported `reachability.verdict=not-exposed` and
`driver_loaded=false`. No ANE MMIO, power-domain request or task submission
occurred in this receipt.

`make -C ane/h17` against the running kernel's headers exited 0. GCC 16
reported warnings in kernel headers because that kernel was built with
Clang 18; there were no source warnings from `ane_h17_adt_probe.c` in the
final build. `make -C ane/h15 check` exited 0 (`test: ane_h15_adt — ok`).

The first stage 0 attempt found a 64-byte `segment-ranges` property but
refused it because the H15 helper preserves the flags in the upper 32
bits of the TEXT size word. The corrected T8140 probe splits the flag
and byte count. Its built module SHA-256 was
`3f76396d38a3c03f2ae6f73a727474908040b960825ed692c7e5cbc354738f08`
for the loaded build. The next `insmod ... optin=1`
exited 0 and printed:

```text
ane_h17_adt_probe segment-word[0]=0x10000774000
ane_h17_adt_probe segment-word[1]=0x0
ane_h17_adt_probe segment-word[2]=0x10000000000
ane_h17_adt_probe segment-word[3]=0x1000c0000
ane_h17_adt_probe segment-word[4]=0x100019ac000
ane_h17_adt_probe segment-word[5]=0xc0000
ane_h17_adt_probe segment-word[6]=0x100000c0000
ane_h17_adt_probe segment-word[7]=0x2ac000
ane_h17_adt_probe TEXT flags=0x1 DATA flags=0x0
ane_h17_adt_probe TEXT phys=0x10000774000 iova=0x10000000000 size=0xc0000
ane_h17_adt_probe DATA phys=0x100019ac000 iova=0x100000c0000 size=0x2ac000
ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=PASS reason=segments
```

`sudo rmmod ane_h17_adt_probe` exited 0, and `lsmod` showed no probe
module afterward. This establishes that iBoot left ANE segment metadata
in the live ADT. It does not establish that the preload bytes are valid,
that the ANE can power up, or that inference runs.

## Stage 1: reserved-RAM preload read

The first `inspect_preload=1` attempt refused `reason=outside-dram` before
mapping a segment. Linux's `memory@10000000000/reg` begins at
`0x10003438000` on this boot, after the iBoot-reserved firmware area. The
probe was corrected to admit only ADT-named ranges in the first 64 MiB of
the J700 DRAM aperture that end before the first Linux RAM range. The
corrected build had SHA-256
`dc4f00b7a3235a906654182ada4ecaac6b463fe9fc65cebf98fbe3f33c1f6eae`
when loaded. `sudo insmod ... optin=1 inspect_preload=1` exited 0 and
printed:

```text
ane_h17_adt_probe preload segment=TEXT sha256=cda09ec1aee68b88b47edc0eebc810452532a855563fbd650b95e2846c345310 nonzero=1
ane_h17_adt_probe preload segment=DATA sha256=ceea79eadab680c3a849dc93442a8d5bec6d43b5e304fa88673e89969c721f5c nonzero=1
ane_h17_adt_probe RESULT stage=1 soc=t8140 verdict=PASS reason=preload-readable
```

`sudo rmmod ane_h17_adt_probe` exited 0. This proves the two named ranges
hold readable, nonzero bytes on this boot. It does not compare them with
the pinned firmware payload or prove that ANE hardware can run them.

After the empty-segment refusal was added, the final module built without
source warnings (SHA-256
`0cebd48a202ed52689bf29f570212568abfb746512e48447783b4d9c882bbaf0`).
The same stage 0 and stage 1 outputs passed again, with the same two
segment digests. `sudo rmmod` exited 0. An `insmod` without `optin=1`
then refused with `reason=board-or-optin`, and no module remained loaded.
