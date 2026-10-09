/* Host shim for <linux/types.h> — the pool header needs bool, size_t, s64. */
#ifndef ANE_POOL_SHIM_LINUX_TYPES_H
#define ANE_POOL_SHIM_LINUX_TYPES_H

#include <stddef.h>
#include <stdbool.h>

typedef signed long long s64;

#endif /* ANE_POOL_SHIM_LINUX_TYPES_H */
