/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint64_t u64;
typedef uint32_t u32;
#include "../chanreg.h"

static void set_entry(u8 *table, unsigned int i, const char *name,
		      u32 type, u32 source, u32 slots, u64 buffer)
{
	u8 *entry = table + i * ANE_H17_CHANNEL_ENTRY_BYTES;
	size_t length = strlen(name);
	assert(length < ANE_H17_CHANNEL_NAME_BYTES);
	memcpy(entry, name, length);
	ane_h17_put_le(entry + 0x40, type, 4);
	ane_h17_put_le(entry + 0x44, source, 4);
	ane_h17_put_le(entry + 0x48, slots, 4);
	ane_h17_put_le(entry + 0x50, buffer, 8);
}

static void test_channel_table(void)
{
	u8 table[ANE_H17_TABLE_BYTES] = { 0 };
	struct ane_h17_channel_init_range init[ANE_H17_CHANNEL_COUNT];
	u32 count = 0;

	set_entry(table, 0, "TERMINAL", 2, 0, 0x200, ANE_H17_IPC_IOVA + 0x800);
	set_entry(table, 1, "IO", 0, 1, 0x10, ANE_H17_IPC_IOVA + 0x8800);
	set_entry(table, 2, "DEBUG", 0, 2, 0x08, ANE_H17_IPC_IOVA + 0x8c00);
	set_entry(table, 3, "BUF_H2T", 0, 3, 0x40, ANE_H17_IPC_IOVA + 0x8e00);
	set_entry(table, 4, "BUF_T2H", 1, 4, 0x40, ANE_H17_IPC_IOVA + 0x9e00);
	set_entry(table, 5, "SHAREDMALLOC", 1, 5, 0x08, ANE_H17_IPC_IOVA + 0xae00);
	set_entry(table, 6, "IO_T2H", 1, 6, 0x40, ANE_H17_IPC_IOVA + 0xb000);
	set_entry(table, 7, "DATA_CHAIN_H2T", 0, 7, 0x10, ANE_H17_IPC_IOVA + 0xc000);
	assert(ane_h17_validate_channel_table(table, ANE_H17_IPC_IOVA,
					      ANE_H17_IPC_BYTES, init, &count));
	assert(count == 4);
	assert(init[0].index == 1 && init[0].offset == 0x8800 && init[0].bytes == 0x400);
	assert(init[1].index == 2 && init[1].offset == 0x8c00 && init[1].bytes == 0x200);
	assert(init[2].index == 3 && init[2].offset == 0x8e00 && init[2].bytes == 0x1000);
	assert(init[3].index == 7 && init[3].offset == 0xc000 && init[3].bytes == 0x400);

	/* Type-1 firmware-produced buffers must also be wholly in logical IPC. */
	ane_h17_put_le(table + 6 * ANE_H17_CHANNEL_ENTRY_BYTES + 0x50,
		       ANE_H17_IPC_IOVA + 0xc040, 8);
	assert(!ane_h17_validate_channel_table(table, ANE_H17_IPC_IOVA,
					       ANE_H17_IPC_BYTES, init, &count));
	ane_h17_put_le(table + 6 * ANE_H17_CHANNEL_ENTRY_BYTES + 0x50,
		       ANE_H17_IPC_IOVA + 0xb000, 8);

	/* Overlap and missing required-name refusals. */
	ane_h17_put_le(table + 2 * ANE_H17_CHANNEL_ENTRY_BYTES + 0x50,
		       ANE_H17_IPC_IOVA + 0x8900, 8);
	assert(!ane_h17_validate_channel_table(table, ANE_H17_IPC_IOVA,
					       ANE_H17_IPC_BYTES, init, &count));
	ane_h17_put_le(table + 2 * ANE_H17_CHANNEL_ENTRY_BYTES + 0x50,
		       ANE_H17_IPC_IOVA + 0x8c00, 8);
	memset(table + 2 * ANE_H17_CHANNEL_ENTRY_BYTES, 0, ANE_H17_CHANNEL_NAME_BYTES);
	memcpy(table + 2 * ANE_H17_CHANNEL_ENTRY_BYTES, "IO", 2);
	table[2 * ANE_H17_CHANNEL_ENTRY_BYTES + 3] = 0x7f;
	assert(!ane_h17_validate_channel_table(table, ANE_H17_IPC_IOVA,
					       ANE_H17_IPC_BYTES, init, &count));
	set_entry(table, 2, "DEBUG", 0, 2, 0x08, ANE_H17_IPC_IOVA + 0x8c00);
	memset(table + 5 * ANE_H17_CHANNEL_ENTRY_BYTES, 0, ANE_H17_CHANNEL_NAME_BYTES);
	memcpy(table + 5 * ANE_H17_CHANNEL_ENTRY_BYTES, "OTHER", 5);
	assert(!ane_h17_validate_channel_table(table, ANE_H17_IPC_IOVA,
					       ANE_H17_IPC_BYTES, init, &count));
}

int main(void)
{
	u64 dm = ANE_H17_DM_IOVA, ipc = ANE_H17_IPC_IOVA;
	u64 reply = (dm & ~0xffffffffull) | (ipc & 0xffffffffull);

	assert(ane_h17_channel_layout(dm, ANE_H17_DM_BYTES, ipc, ANE_H17_IPC_MAPPED));
	assert(reply == ipc);
	assert(ANE_H17_DM_IOVA == ANE_H17_DMA_BASE + ANE_H17_FW_BYTES);
	assert(ANE_H17_IPC_IOVA == dm + ANE_H17_DM_BYTES);
	assert(ANE_H17_HEAP_IOVA == ipc + ANE_H17_IPC_MAPPED);
	assert(ANE_H17_DM_IOVA == 0x10000380000ull);
	assert(ANE_H17_CHAN_ALIAS_BYTES == 0xf50000u);
	assert(ane_h17_channel_range(ANE_H17_HEAP_IOVA, ANE_H17_HEAP_BYTES));
	assert(!ane_h17_channel_layout(0x3ffffbc0000ull, 0x40000, 0x3ffffbb0000ull, 0x10000));
	assert(!ane_h17_channel_layout(0x20000380000ull, 0x40000, ipc, 0x10000));
	assert(ane_h17_channel_range(ANE_H17_DMA_BASE + 0xffffc000ull, 0x4000));
	assert(!ane_h17_channel_range(ANE_H17_DMA_BASE + 0xffffc000ull, 0x8000));
	assert(!ane_h17_channel_range(ipc, 0));
	assert(!ane_h17_channel_range(ipc + 1, 0x4000));
	assert(!ane_h17_channel_range(ipc, 0xc441));
	assert(!ane_h17_channel_range(0xffffffffffffc000ull, 0x8000));
	assert(!ane_h17_channel_range(0x400000, 0x10000));
	test_channel_table();
	puts("PASS: real C layout helper, width/boundary/prefix/overflow/refusal cases");
	return 0;
}
