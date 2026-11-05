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

/* Host-owned alias design, not Apple fixed addresses. c4c43d3 traces the
 * firmware's ldr-w channel pointer and unchanged SCRATCH1 reply prefix.
 * All aliases fit in the same low-32 CPU-alias window, after FWIM.
 * The manual overlay MUST reserve this IOVA range before DMA allocation.
 */
#define ANE_H17_DM_IOVA (ANE_H17_DMA_BASE + ANE_H17_FW_BYTES)
#define ANE_H17_IPC_MAPPED 0x10000u
#define ANE_H17_IPC_IOVA (ANE_H17_DM_IOVA + ANE_H17_DM_BYTES)
#define ANE_H17_HEAP_IOVA (ANE_H17_IPC_IOVA + ANE_H17_IPC_MAPPED)
#define ANE_H17_CHAN_ALIAS_BYTES (ANE_H17_DM_BYTES + ANE_H17_IPC_MAPPED + ANE_H17_HEAP_BYTES)

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

static inline bool ane_h17_channel_range(u64 addr, u64 bytes)
{
	u64 alias = addr & 0xfffffffffull;

	return !(addr & 0x3fff) && !(bytes & 0x3fff) &&
		ane_h17_full_range(ANE_H17_DMA_BASE,
				  ANE_H17_DMA_END - ANE_H17_DMA_BASE, addr, bytes) &&
		alias < 0x100000000ull && bytes <= 0x100000000ull - alias;
}

static inline bool ane_h17_channel_layout(u64 dm, u64 dm_bytes,
					u64 ipc, u64 ipc_bytes)
{
	return ane_h17_channel_range(dm, dm_bytes) &&
		ane_h17_channel_range(ipc, ipc_bytes) && (dm >> 32) == (ipc >> 32);
}

static inline void ane_h17_encode_descriptor(u8 *p, u64 firmware, u64 dm,
					     u64 ipc, u64 heap)
{
	memset(p, 0, ANE_H17_DESC_BYTES);
	ane_h17_put_le(p + 0x00, firmware, 8);
	ane_h17_put_le(p + 0x08, ipc, 8);
	ane_h17_put_le(p + 0x10, ANE_H17_FW_BYTES, 8);
	/* +0x10/+0x18 pair semantics (review closed offline, e1c4839+):
	 * the kext owns the FWIM region as this[0x3c0] = 0x380000 exactly
	 * (allocator size, prereqs.txt:5-19), bounds its device access to
	 * that same extent (prereqs.txt:680-688), and the live macOS boot
	 * reserves the alias as exactly VM_BASE..+0x380000. It nonetheless
	 * publishes +0x18 = 0x10000000 - 0x380000 (prereqs.txt:460-463), a
	 * window-budget companion constant, most plausibly a 256 MiB DVA
	 * window reference (0x8f44838: 0x10001000 near a region call).
	 * It cannot name additional owned firmware memory: no such memory
	 * exists on the host. Firmware's own reader is not in the retained
	 * evidence; firmware requesting DMA outside a mapped extent faults
	 * the DART (observable), so the rung's stop-before-registration
	 * results never depend on interpreting this field. We reproduce
	 * the traced values byte-for-byte and own >= the kext's extents.
	 */
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
