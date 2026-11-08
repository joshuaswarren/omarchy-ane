# H17 boot safety review: generic H16 boot is not yet a valid H17 rung

Source reviewed at `0dc085a`, including `b10dbc1` (H17 match row),
`8c2af07` (duplicated PS offset), and `46efa4b` (H17 fwboot probe).
This review makes no device-execution claim.

## Stop before CPU release

The generic `ane_h16_boot()` writes SCRATCH7 and CPU_CONTROL without the
H17 CE handshake, HAL tunables, or PWGATE sequence. Prior H17 experiments
needed that ordering to avoid freezing. An of_match entry is not enough
to qualify this path. T8140 `stage=boot` now refuses before ioremap,
runtime power acquisition, firmware staging, or MMIO.

The simulator's new `neg-h17-boot` exercises the real probe and checks
zero bus accesses, zero ioremaps, zero runtime power gets, and zero DMA
allocations. `make -C ane/h16 sim` passes, including the existing mutation
checks. The kernel module builds with `W=1`. These are software results,
not device qualification. Full outputs are adjacent to this receipt.

## Corrections to earlier conclusions

- A latched RVBAR is evidence of a latched vector only. It does not prove
  resident firmware contents, valid DART mappings, fetch permissions, or
  successful execution. Prior receipts claiming it rules out firmware
  placement/mapping problems overstate the evidence.
- The recorded live RVBAR is `0x10000000001`. Removing bit 0 gives
  `0x10000000000`, **not** `0x1000000000`. Use canonical hex values and
  decode the actual boot's segment-ranges before comparing addresses.
- A failed wake after remapping would not uniquely prove a gated clock:
  CE ordering, permissions, DAPF, image identity, and mapping lifetime
  remain potential causes. A test without a wake token is inconclusive.
- Zero direct calls to named clkgen helpers does not prove there are no
  generic accesses to the same map. The PMP branch is conditional in the
  analysis itself; describing it as the only possible path was premature.
- Existence of a platform device directory is not proof it is bound.
  The attached preflight checks actual driver symlinks: all three DARTs
  are bound to apple-dart; the ANE is unbound in IOMMU group 15.

## Further source blockers before the DART/firmware rung

`ane/h16/ane_h16_main.c` at `0dc085a`:

1. `ane_h16_stage()` allocates with dma_alloc_coherent; map_stage converts
   that CPU pointer with virt_to_phys. Coherent mappings are not generally
   direct-map pointers, and their DMA handles are not physical addresses.
   Buffer backing and explicit IOMMU ownership need a correct design.
2. `ane_h16_unmap_stage()` unmaps entire segment ranges even on a partial
   failure or a pre-existing mapping collision. Track only owned mappings;
   never unmap mappings that this transaction did not create.
3. Probe drops runtime power after boot, even on the timeout path that
   says power/mappings must remain pinned. A failed probe also invokes
   devres cleanup. Resource lifetime after CPU release needs redesign.
4. pm_runtime_get_sync on a consumer listing multiple power-domains is
   not evidence that every listed domain has been acquired. The H17 probe
   explicitly attaches/votes each domain; the H16 path does not.
5. Repeating c010 in a fifth PS slot suppresses an absent-domain check but
   falsely labels the duplicate as another island. Use an explicit per-SoC
   valid count/mask rather than claiming five domains were checked.
6. The H17 fwboot probe still lacks fail-closed durable logging, full CE
   acknowledgement validation, and checked PS polls. Do not use it as a
   drop-in fix for the generic path without addressing those omissions.

## Installed state and next rung

The updated stage-boot overlay is installed and update-m1n1 has rebuilt
the boot image. The current boot still lacks apple,ane-type; that is why
the previous module probe refused before accessing registers. No firmware
boot ran on this boot. Preserve the journal, do not clear dmesg.

Next: reboot with the updated property, verify the live DT and actual
bindings, then run only `stage=dt` (hardware-silent source path). Neither
stage=status nor CPU release is authorised by these software test results.
The next firmware rung requires addressing the blockers above first.
