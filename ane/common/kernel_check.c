// SPDX-License-Identifier: GPL-2.0
/*
 * kernel_check.c - compiles the __KERNEL__ branch of dart_t8110.h
 * against the kernel-shaped stubs in stub/linux/. Nothing here runs;
 * building this translation unit is the check: every static inline in
 * the header is semantically analyzed even when uncalled, so a missing
 * include (errno codes, memset) fails the compile.
 */
#include "dart_t8110.h"

u64 dart8_kernel_check_ttbr(u64 root_pa)
{
	return dart8_ttbr(root_pa);
}
