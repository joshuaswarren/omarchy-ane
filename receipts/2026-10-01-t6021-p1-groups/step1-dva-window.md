# Step 1: the DVA window words 0x300, 0x308 and 0x310 of the three ANE DARTs (read only)

Inputs: the macOS 27.0 (26A428) `ioreg` of the dart-ane0 node (ane-linux-experiments `d3dc1aef`,
`receipts/2026-09-23-m2-macos-denominator/m2-macos-window/ane-evidence/dart-ane-nodes.txt`), the reads of
[2026-10-01-t6021-dart-tunables](../2026-10-01-t6021-dart-tunables/README.md) on three boots, the read-only DART
probe on the A0 and S boots of this receipt (boots 4 and 5, `logs/*/ane_dart_probe.log`), m1n1
`a878095967c6`, and omarchy-linux `57f8f6deaa3a`. Nothing was written to a DART for this step.

## The words are ADT tunables

The macOS 27.0 ADT node `dart-ane0` has three tunable lists: `dart-tunables-instance-0` (DARTLLT, 6 records),
`-1` (DARTBRD, 22 records) and `-2` (DARTBWR, 22 records). All three lists contain the same three window records:

| offset | ADT mask | ADT value | meaning of the value |
|---|---|---|---|
| 0x300 | 0x00001f31 | 0x00000001 | bit 0 set, bits 4, 5 and 12:8 clear |
| 0x308 | 0x3ffffffc | 0x10000000 | vm-base 0x100_0000_0000 >> 12 |
| 0x310 | 0x3ffffffc | 0x3ffffffc | (vm-base + vm-size - 16 KiB) >> 12 |

The node has vm-base 0x100_0000_0000, vm-size 0x300_0000_0000 and page-size 0x4000. So 0x308 and 0x310 hold the
first and the last 16 KiB page of the IOMapper range, in 4 KiB units and inclusive. The masks leave out bits 1:0,
which fits a 16 KiB granule. The macOS 13.5 hv trace writes the same values to BRD and BWR (events 2860-2862 and
2890-2892).

## macOS against Linux

| DART | offset | macOS (ADT / 13.5 trace result) | Linux, boot 1 | boot 2 | boot 3 | boot 4 | boot 5 |
|---|---|---|---|---|---|---|---|
| LLT | 0x300 | 0x00000001 | 0x00000001 | 0x00000001 | 0x00000001 | 0x00000001 | 0x00000001 |
| LLT | 0x308 | 0x10000000 | 0x10000000 | 0x10000000 | 0x10000000 | 0x10000000 | 0x10000000 |
| LLT | 0x310 | 0x3fffffff | 0x3fffffff | 0x3fffffff | 0x3fffffff | 0x3fffffff | 0x3fffffff |
| BRD | 0x300 | 0x00000001 | 0x00001e10 | 0x00001d10 | 0x00001d10 | 0x00001c10 | 0x00001c10 |
| BRD | 0x308 | 0x10000000 | 0x3ffffdac | 0x3fff7da4 | 0x3fffffac | 0x3ffffdac | 0x3fff7da4 |
| BRD | 0x310 | 0x3fffffff | 0x3f6cfbcb | 0x3f6cfdcf | 0x3f6cffcf | 0x3e6cffcf | 0x3f6cfdcf |
| BWR | 0x300 | 0x00000001 | 0x00001f30 | 0x00001f30 | 0x00001e30 | 0x00001f30 | 0x00001f30 |
| BWR | 0x308 | 0x10000000 | 0x2fedebfc | 0x2bedebf4 | 0x2fedebf4 | 0x2fedebf4 | 0x2fedebd4 |
| BWR | 0x310 | 0x3fffffff | 0x3e3fedff | 0x3f3fedff | 0x3e3fedff | 0x3e3dedff | 0x3f3debff |

Observations:

- LLT holds the macOS values on every boot. Its `pmp` power domain stays on through the handover, so it keeps
  what the earlier boot stage wrote.
- On BRD and BWR, bit 0 of 0x300 reads 0 on every boot. Within one boot every read is equal (4 to 22 reads, before
  and after encoder calls). Between boots only scattered bits change.
- Bits 1:0 read 0 in 0x308 and 3 in 0x310 on every boot, the same bits the 13.5 trace shows before its write.
- m1n1 names the words only "hwrev 2 only" (`proxyclient/m1n1/hw/dart8110.py`), and Linux `apple-dart` does not
  write them.

[INFERENCE] Bit 0 of 0x300 enables a DVA window from 0x308 to 0x310, and macOS sets that window to the IOMapper
range. Under Linux the window is off on BRD and BWR, and 0x308 and 0x310 hold power-on contents that have no
reset value. While bit 0 is 0, the hardware does not use them. What the window does when it is on is not known.
One LLT observation (2026-09-29) argues against a plain range check: the firmware access to IOVA 0x28e084008, which
is outside the LLT window, gave page-walk fault codes (NO PMD, then NO PTE) and not a range error.

## BO IOVAs above 4 GiB

All BO IOVAs are below 4 GiB because the driver sets a 32-bit DMA mask (`ane_t6021_rtclient_main.c:1910`).
To move them into the macOS window:

1. Set the DMA mask to 42 bits. PARAMS_8 [21:16] (VA_WIDTH) reads 0x2a = 42 on all three DARTs. The kernel
   IOVA allocator tries the 32-bit range first only for PCI devices (`drivers/iommu/dma-iommu.c:796`, flag set at
   `:2158`), so a platform device gets IOVAs from the top of the range, just below 0x400_0000_0000. That is inside
   the macOS window [INFERENCE from the allocator code].
2. For the exact macOS range, add `apple,dma-range = <0x100 0x0 0x300 0x0>` to the three ANE DART nodes.
   `apple-dart.c:1393-1408` turns it into the domain aperture.
3. The command format needs no change. The LOAD and CALL records carry 64-bit IOVA fields (slot offset 0x18,
   `cpu_to_le64(iova)`). The firmware already runs from IOVAs above 1 TiB on Linux (the firmware map at
   0x100_0084_8000, RVBAR 0x100_0000_0001), through the same page tables: one domain covers all three DARTs.
4. Not known: whether the 13.5 firmware or the ANE DMA engines keep only 32 bits of a BO base on our command
   path. macOS places ANE buffers in the window [INFERENCE from the IOMapper range], so the hardware must accept
   such addresses there.

Test order for a later run: the mask change alone, with the default DART words, the gates and the encoder
golden. Only then E3, the window words on a DART whose BOs lie inside the window. A live write of the window
while BOs sit below 4 GiB is not part of this work.
