/* Flat-layout link script for the ane-add payload.
 * The chainloader places the image at LOAD_ADDR (physical = virtual);
 * all addressing is position-dependent on that base.
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(_start)

SECTIONS
{
	. = LOAD_ADDR;
	.text : {
		*(.text._start)
		*(.text)
		*(.text.*)
	}
	. = ALIGN(16);
	.rodata : {
		*(.rodata)
		*(.rodata.*)
	}
	. = ALIGN(16);
	.data : {
		*(.data)
		*(.data.*)
	}
	. = ALIGN(16);
	bss_start = .;
	.bss : {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	}
	. = ALIGN(16);
	bss_end = .;
	. = ALIGN(4096);
	heap_start = .;
	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
