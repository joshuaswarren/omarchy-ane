# H17 offline firmware reply/channel-address analysis

2026-11-05. Offline only. No kernel deployment, register/device access,
module load/unload, task, doorbell, marker creation or reboot.

## Conclusion and diagnostic decision

**The rung-1 allocation layout violates a concrete firmware channel-pointer
width constraint.** Firmware reads the initialization descriptor using a
64-bit address, builds a channel table using 64-bit IPC addresses, but then
reloads each channel buffer pointer with a **32-bit load**. That narrowed
pointer is passed to an initializer which writes through it.

For recorded IPC DVA `0x3ffffbb0000`, the first channel pointer is
`0x3ffffbb0800`; the narrowed value is **`0xffbb0800`**, exactly the DART
fault address in the rung-1 receipt. This is channel-header initialization
before the discovery reply, **not a channel-table reply store at +0x800**.
The earlier attribution of that offset to the table itself was incorrect.

This is a source-backed failure mechanism with a matching device symptom,
not proof of the exact PC executing on the failed device, or proof that no
additional faults exist. CPU MMU state and DART root contents on that boot
remain unmeasured. Nevertheless, there is no reason to boot the diagnostic
kernel merely to discover whether this width narrowing exists: it is now
established offline. **Defer diagnostic-kernel deployment.** First design
and review firmware-compatible allocation/alias handling and its refusal
paths. Use the diagnostic kernel only if a compliant layout still fails or
mapping/publication state cannot be established through existing gates.

## Input identity and reproduction

Retained firmware:
`/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho`, SHA-256
`32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c`.
Mach-O TEXT VA 0 is file offset `0x4000`; thus every instruction VA below
maps to file offset `VA + 0x4000`. Verify with `trace.py info`.

```
python3 receipts/2026-11-04-j700-h17-offline-firmware-reply/evidence.py
python3 -m unittest discover -s receipts/2026-11-04-j700-h17-offline-firmware-reply -p 'test_address_model.py' -v
```

`evidence.py` verifies 35 exact instruction anchors against the hashed
firmware, emits bounded annotated disassembly and `address-model.json`.
Five arithmetic tests cover the recorded failure, compatible arithmetic,
mixed address prefixes and low-32 boundary crossings, including a first
pointer that fits but a later logical IPC extent that does not. These do not model
device execution or authorize the example addresses. Capstone 5.0.7 is
cross-checked with LLVM18's Mach-O disassembler at the narrowing/store sites
(`llvm-crosscheck.txt`). No complete firmware image is copied into this
receipt. Earlier subagents stopped on their usage limit; the completed
analysis was performed in the parent session. Subsequent independent review
`ses_e53a66e71ffeOBhrfy4uxfzHzh` **APPROVED** the findings/decision with two
low-severity notes: retain the LLVM cross-check and test whole-extent
boundary crossing. Both were addressed; this is still not device evidence.

## Instruction chain (firmware SHA above, VA -> file VA+0x4000)

1. **Host descriptor address is not simply truncated to 32 bits.** The
   request loop compares SCRATCH7 against `0xf7fbdff9` at `0x73a8` after
   constructing it at `0x718c/0x7194`. It reads SCRATCH1, shifts it left 32
   (`0x73f0`), reads SCRATCH0 and combines them (`0x7458`), then calls the
   mapping interface. There is a conditional high-half compatibility case
   at `0x73f4..0x73fc`; the traced high half `0x3ff` differs from the initial
   `0xc440`, so it is retained on this path. Scratch read/write methods are
   `0x882c/0x8838`; constructor `0x87b8..0x87cc` sets their base to
   `0x401880020`. Vtable at `0xc01d8` resolves +0x28 to `0x882c` and +0x30
   to `0x8838`. Chained pointer low targets are corroborated by the methods'
   instruction bodies, not treated as raw callable PAC pointers.
2. **Descriptor +8 is read as a full-width IPC DVA.** `ldp x25,x8,[x0]`
   (`0x748c`), `str x8,[sp,#0x38]` (`0x749c`), then
   `ldr x20,[sp,#0x38]` (`0x76e0`). The caller passes this original IPC
   address to `0x21564` at `0x76fc..0x7708`.
3. **CPU alias and original address are distinct.** The mapping helper
   initializes its returned CPU alias with `and x19,x1,#0xfffffffff`
   (`0x2159c`), a **36-bit** mask. Mapping code retains original address
   inputs; e.g. `0x20e88..0x20e98` passes masked CPU VA separately from the
   original address to `0x28e8c`. This mask alone is not proof of loss of
   the original DART address. On successful return, rung-1 IPC's CPU alias
   is `0xfffbb0000`, which still does **not** fit 32 bits.
4. **Table builder takes original DVA and mapped CPU alias separately.**
   `0x7748..0x7750` calls `0x52e4` with x0 = original IPC DVA and x1 = its
   mapped CPU alias. It sets x19 = align64(x1) (`0x5300..0x5308`),
   x20 = align64(x0) (`0x530c..0x5314`), then x22 = x20 + `0x800`
   (`0x5324`). `stp x8,x22,[x19,#0x48]` (`0x533c`) stores slots and the
   **full 64-bit** first buffer pointer into the table. Entry zero is
   `TERMINAL`, type 2, source 0, slots `0x200`; its string starts at
   VA `0x9d31c`. The table itself starts at the mapped IPC base, not +0x800.
5. **Actual narrowing site:** channel construction reloads that +0x50
   pointer with `ldr w8,[x27,#0x50]` at **`0x6e0c`**. ARM64 W-register
   writes clear the upper half of the corresponding X register. It stores
   the zero-extended value with `str x8,[x24,#0x10]` (`0x6e18`) in a
   temporary per-channel descriptor. The field is not read with `ldr x8`.
6. **Narrowed pointer reaches a writer.** Group constructor `0x69d4`
   reads the temporary descriptor's +0x10 into x4 (`0x6a60`) and calls
   `0xdbcc`. That constructor preserves x4 in x20 (`0xdc04`), passes it as
   x1 (`0xdc20`) and calls `0x298fc` (`0xdc44`). For entry-zero type 2,
   the default type transform `0x54a4` preserves 2 (caller mode 0), and
   `0xdc14..0xdc40` converts it to flags 3. Flag bit 0 is therefore set.
   The channel initializer saves x1 into x19 (`0x29920`); its enabled loop
   writes `stp xzr,xzr,[x19,#8]` (`0x299b8`) and `str x8,[x19]`
   (**`0x299bc`**), advancing by `0x40`. The first base write operand is
   `low32(align64(IPC DVA)+0x800)` = **`0xffbb0800`**.
7. **Reply reconstruction has a second layout constraint.** Only after
   channel construction does firmware write the table pointer's low 32
   bits to SCRATCH0 (`0x77c4..0x77e4`) and acknowledge via SCRATCH7
   (`0x77e8..0x7810`). The shown reply path does not replace SCRATCH1.
   The host reads SCRATCH1/SCRATCH0 as high/low words. A layout correction
   must therefore preserve that round trip too, not move IPC independently
   while leaving an incompatible DM prefix in SCRATCH1.

## Derived constraints, not an approved mapping plan

For the traced default channel-init path to use the CPU alias created for
IPC, every channel-buffer address consumed via the W load must fit that
alias's low-32 view. A sufficient arithmetic condition is that the IPC
allocation's **low 36-bit range lies entirely below 2^32**, without crossing
that boundary. This is not a requirement that the original 42-bit DVA be
below 4 GiB. Original addresses above 2^40 can satisfy it.

For the observed host reply reconstruction, DM's published upper 32 bits
must reconstruct the original IPC table DVA with firmware's low-word reply.
The model demonstrates that relocating IPC alone across prefixes can fix
the channel narrowing while breaking that reply check.

`0x10000300000` / `0x10000400000` in the model are **arithmetic examples
only**, not reserved/mapped/owner-approved addresses. No conclusion here
establishes root-slot ownership or availability. A future implementation
must reserve any alias before DMA allocation, use only owned backing,
validate its whole extent and collisions, refuse incompatible mappings,
retain all state after CPU release, and pass the established staged gates.
The HEAP's other consumers have not been qualified by this channel trace.
