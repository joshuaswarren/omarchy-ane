/* Host shim for <linux/mm.h> and <linux/sizes.h> — alignment + sizes. */
#ifndef ANE_POOL_SHIM_LINUX_MM_H
#define ANE_POOL_SHIM_LINUX_MM_H

#define PAGE_SIZE	0x1000ull
#define PAGE_SHIFT	12

#define PAGE_ALIGN(v) (((v) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

#define SZ_1K	0x400
#define SZ_1M	0x100000

#endif /* ANE_POOL_SHIM_LINUX_MM_H */
