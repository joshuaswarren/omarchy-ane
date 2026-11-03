# H18 add fixture (T8152, Mac mini M6)

`model.hwx` is a one-op fp16 add (two runtime inputs a and b, `[1,64,1,1]`, output y) compiled on an M6
(Mac18,5, macOS 27.0.1 build 26A434) by `ANECCompile` with target `h18` (Mach-O subtype 10).
`program-0.anec` is that HWX converted by `tools/hwx_h14_staged_to_anec.py` with two H18 changes: subtype 10 is
accepted and the program-descriptor IOVA array sits at +0x60 instead of +0x50. The container is the H14 layout
holding the raw H18 task stream (316 bytes, one task).

`a.phys4096`, `b.phys4096` and `y-oracle.phys4096` are 4096 byte buffers with channel c at byte c*64, made by
`make-add-oracle.py` (a = (c+1)/8, b = 0.25 * (c % 7 + 1), y = fp16(a + b)). The oracle is a CPU reference, not a
CoreML or hardware result. The 64 byte channel stride is inferred from 4096 = 64 x 64.

Nothing here has run to completion on an H18 ANE. The M6 accepted and started a first pack of this program (no
finish event). `tools/test_ane_prog_pack_h18.sh` checks the packed file against the structure the module reads.
The BAR slot and tag table the builder derives for this program is slot 5 to tag 5, slot 6 to tag 6, slot 7 to
tag 4 (the three surfaces a, b and y of the program descriptor). The three one-word records at slot 4 name a zero
descriptor entry and stay unpatched; an earlier revision patched them with the kernel section, which was a
hypothesis the descriptor contradicts. See the comment above `h18_ext_slot` in `libane/ane_m2.c`.
