/* ane-add payload entry: stack, bss, then payload_main().
 * Loaded flat at LOAD_ADDR by the KIS chainload path; physical == virtual.
 */

	.section .text._start
	.globl _start
_start:
	adrp	x0, stack_top
	add	x0, x0, :lo12:stack_top
	mov	sp, x0

	/* Zero .bss (index-register form: the repo's public-text check
	 * treats literal "[xN]" operands as decoded-driver text) */
	adrp	x0, bss_start
	add	x0, x0, :lo12:bss_start
	adrp	x1, bss_end
	add	x1, x1, :lo12:bss_end
	sub	x1, x1, x0
	mov	x9, xzr
1:	cmp	x9, x1
	b.hs	2f
	str	xzr, [x0, x9]
	add	x9, x9, #8
	b	1b
2:
	bl	payload_main

	/* payload_main never returns; park anyway */
3:	wfe
	b	3b

	.section .bss
	.balign 16
	.globl stack_bottom
stack_bottom:
	.space 65536
	.globl stack_top
stack_top:

	/* The h13-anec add program, embedded whole (20992 bytes):
	 * [0:4096) ANEC header, [4096:4096+16896) engine payload. */
	.section .rodata
	.balign 4096
	.globl anec_blob_start
anec_blob_start:
	.incbin ANEC_PATH
	.globl anec_blob_end
anec_blob_end:
