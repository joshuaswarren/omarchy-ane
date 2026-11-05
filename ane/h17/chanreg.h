/* SPDX-License-Identifier: MIT */
#ifndef ANE_H17_CHANREG_H
#define ANE_H17_CHANREG_H

/* Source contract: e1c4839, firmware-load-path receipt sections 15/16.
 * These helpers encode bytes and bounds only. They perform no I/O.
 */
#define ANE_H17_DM_BYTES 0x40000u
#define ANE_H17_DESC_BYTES 0x180u
#define ANE_H17_FW_BYTES 0x380000u
#define ANE_H17_IPC_BYTES 0xc441u
#define ANE_H17_HEAP_BYTES 0xf00000u
#define ANE_H17_TABLE_BYTES 0x800u
#define ANE_H17_TABLE_REQUEST 0xf7fbdff9u
#define ANE_H17_DMA_BASE 0x10000000000ull
#define ANE_H17_DMA_END 0x40000000000ull

static inline void ane_h17_put_le(u8 *p, u64 value, unsigned int bytes)
{
	unsigned int i;

	for (i = 0; i < bytes; i++)
		p[i] = value >> (8 * i);
}

static inline u64 ane_h17_get_le(const u8 *p, unsigned int bytes)
{
	u64 value = 0;
	unsigned int i;

	for (i = 0; i < bytes; i++)
		value |= (u64)p[i] << (8 * i);
	return value;
}

static inline bool ane_h17_full_range(u64 base, u64 size, u64 addr, u64 len)
{
	return size && len && size <= ~0ull - base && addr >= base &&
		addr - base <= size && len <= size - (addr - base);
}

static inline void ane_h17_encode_descriptor(u8 *p, u64 firmware, u64 dm,
					     u64 ipc, u64 heap)
{
	memset(p, 0, ANE_H17_DESC_BYTES);
	ane_h17_put_le(p + 0x00, firmware, 8);
	ane_h17_put_le(p + 0x08, ipc, 8);
	ane_h17_put_le(p + 0x10, ANE_H17_FW_BYTES, 8);
	ane_h17_put_le(p + 0x18, 0x10000000 - ANE_H17_FW_BYTES, 8);
	ane_h17_put_le(p + 0x20, heap, 8);
	ane_h17_put_le(p + 0x28, ANE_H17_HEAP_BYTES, 8);
	/* +0x30 is the preload default firmware argument, zero. */
	ane_h17_put_le(p + 0x50, ANE_H17_IPC_BYTES, 8);
	ane_h17_put_le(p + 0x58, dm, 8);
	ane_h17_put_le(p + 0x60, ANE_H17_DM_BYTES, 8);
	ane_h17_put_le(p + 0x68, 0x40, 4);
	/* Traced default options block: Z_ZERO allocation, +0xc0 |= 4. */
	ane_h17_put_le(p + 0x12c, 4, 4);
}

#endif
