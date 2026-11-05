/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint64_t u64;
#include "../chanreg.h"

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
	puts("PASS: real C layout helper, width/boundary/prefix/overflow/refusal cases");
	return 0;
}
