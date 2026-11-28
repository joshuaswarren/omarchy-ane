/* SPDX-License-Identifier: MIT */
/*
 * bmn03-payload.S — arm64 Image-format entry stub for the BMN-03 bare-metal
 * ANE power-up payload (t8103 / jwm1).
 *
 * Two delivery paths share this entry contract (x0 = devicetree, MMU off):
 *  1. ESP `aura-try` marker names a trial stage2 (stock boot.bin + this Image
 *     appended); the stage2's payload scan treats the last kernel-format
 *     record as the kernel. Needs a post-Oct-31 stage1 — absent on jwm1.
 *  2. `kexec -s -e` from the running Linux; cpu_soft_restart enters at the
 *     image base with MMU off. Works on the current install.
 * The payload arms the t8103 WDT itself first thing — the trial watchdog
 * arm fails on this chip (wdt-version gate) and kexec has no trial stage.
 */

    .section .text.head
    .globl _start
_start:
    b       entry               /* code0: jump over the 64-byte Image header */
    .long   0                   /* code1 */
    .quad   0                   /* text_offset: image starts executing at +0 */
    .quad   _payload_end - _start  /* image_size: required by m1n1's loader */
    .quad   0x0a                /* flags: LE (bit0=0), kernel-anywhere, phys-end */
    .quad   0                   /* res2 */
    .quad   0                   /* res3 */
    .quad   0                   /* res4 */
    .byte   0x41, 0x52, 0x4d, 0x64   /* magic "ARM\x64" */
    .long   0                   /* res5 */

entry:
    msr     daifset, #0xf

    /* Private stack at the top of jwm1's reserved isp-heap hole
     * (0xbd2bb0000 + 0x1ac000 = 0xbd2d5c000). Nothing else owns that DRAM. */
    movz    x10, #0xc000
    movk    x10, #0xd2d5, lsl #16
    movk    x10, #0x000b, lsl #32
    mov     sp, x10

    bl      payload_main

    /* payload_main must not return; if it does, spin on WFE and let the
     * trial watchdog reset the box. */
1:  wfe
    b       1b

    .section .text
