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
