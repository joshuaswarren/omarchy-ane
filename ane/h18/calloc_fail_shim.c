/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Test seam: LD_PRELOAD shim failing exactly the staging allocation
 * (n*size == 0x370000) when ANE_H18_TEST_FAIL_CALLOC is set. Serves dlsym's
 * own early allocations from a static arena before the real calloc is bound. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

static void *(*real_calloc)(size_t, size_t);
static char arena[8192];
static size_t arena_used;

void *calloc(size_t n, size_t size)
{
	if (!real_calloc) {
		size_t need = n * size;
		void *p;

		if (arena_used + need > sizeof(arena))
			return NULL;
		p = arena + arena_used;
		arena_used += need;
		memset(p, 0, need);
		return p;
	}
	if (getenv("ANE_H18_TEST_FAIL_CALLOC") && n * size == 0x370000u)
		return NULL;
	return real_calloc(n, size);
}

__attribute__((constructor)) static void shim_init(void)
{
	real_calloc = dlsym(RTLD_NEXT, "calloc");
}
