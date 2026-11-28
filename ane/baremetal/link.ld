/* SPDX-License-Identifier: MIT */
/* Bare-metal Image layout: text at 0, everything else discarded so any
 * accidental rodata/data/bss reference fails the link instead of silently
 * breaking position independence. */
SECTIONS
{
    . = 0;
    .text : {
        *(.text.head)
        *(.text)
        *(.text.*)
    }
    _payload_end = .;
    /DISCARD/ : { *(*) }
}
