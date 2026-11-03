/* Fake <linux/slab.h> for sim/sim_overlay.c: the allocator is tracked so the tests can see who owns what. */
#ifndef SIM_OVL_SLAB_H
#define SIM_OVL_SLAB_H
#include <stddef.h>

#define GFP_KERNEL 0

void *kzalloc(size_t size, int gfp);
char *kstrdup(const char *s, int gfp);
void *kmemdup(const void *src, size_t len, int gfp);
void kfree(const void *p);
#endif
