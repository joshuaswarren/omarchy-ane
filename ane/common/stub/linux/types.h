/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Kernel-shaped stub for the kernel-style syntax check only. Mirrors
 * what include/linux/types.h provides a module: bool, size_t and the
 * fixed-width types (include/linux/types.h:34,61).
 */
#ifndef _STUB_LINUX_TYPES_H
#define _STUB_LINUX_TYPES_H

#include <stdbool.h>
#include <stddef.h>	/* size_t */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#endif /* _STUB_LINUX_TYPES_H */
