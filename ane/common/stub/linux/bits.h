/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel-shaped stub for the kernel-style syntax check only. GENMASK_ULL
 * and BIT_ULL from include/linux/bits.h, same values as the userspace
 * fallback in dart_t8110.h.
 */
#ifndef _STUB_LINUX_BITS_H
#define _STUB_LINUX_BITS_H

#define BIT_ULL(nr)		(1ULL << (nr))
#define GENMASK_ULL(h, l)	(((~0ULL) << (l)) & (~0ULL >> (63 - (h))))

#endif /* _STUB_LINUX_BITS_H */
