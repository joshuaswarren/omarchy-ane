/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h15_adt_test.c — host self test for ane_h15_adt.h. Builds a
 * minimal IODeviceTree-shaped blob (no macOS bytes), invokes the
 * walker and the segment-ranges decode, and prints the result.
 * Run: make -C ane/h15 check. The H16 module's identical walker
 * ran on real macOS ADTs in receipts/2026-10-03-ane-h16; this
 * harness is the host-side sanity check, not a substitute.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane_h15_adt.h"

struct buf { uint8_t *p; size_t w, len, cap; };

static void bw(struct buf *b, uint8_t v)
{
	if (b->w + 1 > b->cap) {
		b->cap = b->cap ? b->cap * 2 : 256;
		b->p = realloc(b->p, b->cap);
	}
	if (b->w >= b->len) b->len = b->w + 1;
	b->p[b->w++] = v;
}

static void b32(struct buf *b, uint32_t v)
{
	bw(b, v); bw(b, v>>8); bw(b, v>>16); bw(b, v>>24);
}

static void bN(struct buf *b, const void *src, size_t n)
{
	const uint8_t *s = src;
	while (n--) bw(b, *s++);
}

static void bs32(struct buf *b, const char *s)
{
	char n[32] = { 0 };
	strncpy(n, s, 31);
	bN(b, n, 32);
}

static void bprop(struct buf *b, const char *name, const void *data, uint32_t size)
{
	bs32(b, name);
	b32(b, size);
	bN(b, data, size);
	while (size & 3) bw(b, 0);
}

static struct ane_h15_adt build_blob(void)
{
	struct buf b = { 0 };
	uint32_t at = 176;
	uint64_t e[6] = {
		0x10000000000ull, 0x10000000000ull, 0xc0000ull,
		0x10000800000ull, 0x10000800000ull, 0x2ac000ull,
	};

	/* root: 0 props, 1 child */
	b32(&b, 0);
	b32(&b, 1);
	/* arm-io: 1 prop "name", 1 child (ane) */
	b32(&b, 1);
	b32(&b, 1);
	bprop(&b, "name", "arm-io", 7);
	/* ane: 3 props (ane-type, name, segment-ranges), 0 children */
	b32(&b, 3);
	b32(&b, 0);
	bprop(&b, "ane-type", &at, 4);
	bprop(&b, "name", "ane", 4);
	bprop(&b, "segment-ranges", e, sizeof(e));

	return (struct ane_h15_adt){ .b = b.p, .len = b.len };
}

int main(void)
{
	struct ane_h15_adt a = build_blob();
	struct ane_h15_adt_node ane;
	struct ane_h15_seg segs[2] = { 0 };
	int err;
	int rc = 0;

	err = ane_h15_adt_find_ane(&a, 176, &ane);
	if (err != ANE_H15_OK) {
		fprintf(stderr, "find_ane: err=%d (want %d)\n", err, ANE_H15_OK);
		rc = 1;
		goto out;
	}
	err = ane_h15_adt_segments(&a, ane, segs, 0xc0000, 0x2ac000);
	if (err != ANE_H15_OK) {
		fprintf(stderr, "segments: err=%d (want %d)\n", err, ANE_H15_OK);
		rc = 1;
		goto out;
	}
	if (segs[0].phys != 0x10000000000ull || segs[0].iova != 0x10000000000ull ||
	    segs[0].size != 0xc0000ull || segs[1].phys != 0x10000800000ull ||
	    segs[1].iova != 0x10000800000ull || segs[1].size != 0x2ac000ull) {
		fprintf(stderr, "decoded values mismatch\n");
		rc = 1;
		goto out;
	}
	puts("test: ane_h15_adt — ok");
out:
	free((void *)a.b);
	return rc;
}
