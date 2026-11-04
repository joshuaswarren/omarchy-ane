# H17 DT-only rung and two ADT parser fixes

Parent commit `d4513e4`. Boot `984bf0a3-fe01-4c12-b92c-3c7152556dbc`.
The updated boot DT has apple,ane-type=0x200, engine/pmgr windows, and all
three ANE DARTs bound to apple-dart. The ANE driver was initially unbound.

`insmod ane/h16/ane_h16.ko optin=t8140 stage=dt` returned 0 and reported
`verdict=PASS reason=dt-parse-only`. This stage does not acquire runtime
power or access ANE registers. It found the reserved ADT but failed to
find the ANE segment-ranges. Private full journal/manifest artifacts:
`/var/tmp/opencode/h17-dt-only-984bf0a3-fe01-4c12-b92c-3c7152556dbc/`.

## Confirmed software defects

1. The arm-io node name is seven bytes including its NUL. The parser
   required size > 7; its simulator fixture incorrectly used eight bytes.
   Use size >= 7 and test the actual seven-byte encoding.
2. In the 32-byte segment-ranges record, size at +0x18 is **u32**, not
   u64. The text record's next dword is 1, so the old decoder produced
   0x1000c0000 instead of 0xc0000. The adjacent dword's meaning is not
   assumed by this fix. Retained kernelcache SHA256
   7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe:
   mapFwCTRRRegion loads sizes at 0xfffffe0008f440a0 and
   0xfffffe0008f440c0 using 32-bit ldr w9 instructions (offsets 0x18 and
   0x38 respectively). Local verification output:
   `/var/tmp/opencode/h17-segment-size-disasm.txt`.

Tree bounds and recursion depth are now validated before offset walkers
run. Truncated header/property and excessive-depth tests refuse safely.
This does not establish arbitrary segment contents are safe to map.

## Offline replay

Captured ADT SHA256:
90103c9a9884f6eb39e524b101d8b1cff321de1704bc17ce514ab9f7fe81c4c2.
The C decoder and an independent Python parser agree on raw fields.
After the u32 size correction, the C replay reports:

```
TEXT phys=0x10000774000 iova=0x10000000000 size=0xc0000
DATA phys=0x100019ac000 iova=0x100000c0000 size=0x2ac000
```

Commands:

```
make -C ane/h16 sim
ane/h16/sim/sim_h16 adt-file /var/tmp/opencode/adt.bin
make -C ane/h16 KERNELDIR=/lib/modules/$(uname -r)/build W=1
```

All pass. New mutation tests restore each defect in a source copy and
require its matching simulator test to fail. No Apple ADT bytes are
committed. Simulation/replay is software evidence only. T8140 stage=boot
remains refused before hardware access; the ordering, DMA ownership and
resource-lifetime blockers in the preceding safety review still apply.
