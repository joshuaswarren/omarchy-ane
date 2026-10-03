/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h15_adt.h — pure Apple boot-ADT (IODeviceTree) walker for the H15
 * (M3) bring-up module. No kernel API: the same header compiles into
 * the module and into the host self test (ane_h15_adt_test.c, `make -C
 * ane/h15 check`), the executable check for this parser.
 *
 * Format (ane/h16/ane_h16_main.c, same layout): a node is
 *   u32 nprops; u32 nchildren;
 *   nprops x { char name[32]; u32 len_bit31; data padded to 4; }
 *   nchildren x node
 * A node's subtree is contiguous, so one overflow check per step keeps
 * every access inside the blob.
 */
#ifndef __ANE_H15_ADT_H__
#define __ANE_H15_ADT_H__

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define ANE_H15_ADT_NAME_LEN	32
#define ANE_H15_ADT_SEG_MAX	2
#define ANE_H15_ADT_PROP_HDR	(ANE_H15_ADT_NAME_LEN + 4)
#define ANE_H15_ADT_PROP_ALIGN	4u

#define ANE_H15_OK		0
#define ANE_H15_ENOENT		1	/* node or property absent */
#define ANE_H15_EFORMAT		2	/* blob malformed */
#define ANE_H15_ENOSPACE	3	/* caller buffer too small */

struct ane_h15_adt {
	const u8 *b;
	size_t len;
};

struct ane_h15_adt_node {
	u32 off;
	u32 nprops;
	u32 nchildren;
};

struct ane_h15_seg {
	u64 phys;	/* segment-ranges word 0 */
	u64 iova;	/* the ASC-side address (remap; word 1 or 2) */
	u64 size;	/* last word */
};

static inline u32 ane_h15_adt_pad4(u32 n) { return (n + 3u) & ~3u; }

static inline u32 ane_h15_adt_rd32(const struct ane_h15_adt *a, size_t off)
{
	return (u32)a->b[off] | (u32)a->b[off + 1] << 8 |
	       (u32)a->b[off + 2] << 16 | (u32)a->b[off + 3] << 24;
}

static inline u64 ane_h15_adt_rd64(const struct ane_h15_adt *a, size_t off)
{
	return (u64)ane_h15_adt_rd32(a, off) |
	       (u64)ane_h15_adt_rd32(a, off + 4) << 32;
}

static struct ane_h15_adt_node ane_h15_adt_node_at(const struct ane_h15_adt *a,
						   u32 off, int *err)
{
	struct ane_h15_adt_node n = { .off = off };

	if (off + 8 > a->len) {
		*err = ANE_H15_EFORMAT;
		return n;
	}
	n.nprops = ane_h15_adt_rd32(a, off);
	n.nchildren = ane_h15_adt_rd32(a, off + 4);
	if (n.nprops == 0 || n.nprops > 4096 || n.nchildren > 4096) {
		*err = ANE_H15_EFORMAT;
		n.nprops = 0;
		n.nchildren = 0;
	}
	return n;
}

static u32 ane_h15_adt_subtree_end(const struct ane_h15_adt *a,
				   struct ane_h15_adt_node n, int *err)
{
	u32 off = n.off + 8;
	unsigned int i;

	for (i = 0; i < n.nprops; i++) {
		u32 len;

		if (off + ANE_H15_ADT_PROP_HDR > a->len) {
			*err = ANE_H15_EFORMAT;
			return 0;
		}
		len = ane_h15_adt_rd32(a, off + ANE_H15_ADT_NAME_LEN) & 0x7fffffffu;
		off += ANE_H15_ADT_PROP_HDR + ane_h15_adt_pad4(len);
	}
	for (i = 0; i < n.nchildren; i++) {
		struct ane_h15_adt_node c = ane_h15_adt_node_at(a, off, err);

		if (*err)
			return 0;
		off = ane_h15_adt_subtree_end(a, c, err);
		if (*err)
			return 0;
	}
	if (off > a->len) {
		*err = ANE_H15_EFORMAT;
		return 0;
	}
	return off;
}

/* Offset of the i-th child of @parent in @a. */
static int ane_h15_adt_nth_child_off(const struct ane_h15_adt *a,
				     struct ane_h15_adt_node parent,
				     unsigned int i, u32 *out)
{
	u32 off = parent.off + 8;
	unsigned int k;

	if (i >= parent.nchildren)
		return ANE_H15_ENOENT;
	for (k = 0; k < parent.nprops; k++) {
		u32 len;

		if (off + ANE_H15_ADT_PROP_HDR > a->len)
			return ANE_H15_EFORMAT;
		len = ane_h15_adt_rd32(a, off + ANE_H15_ADT_NAME_LEN) & 0x7fffffffu;
		off += ANE_H15_ADT_PROP_HDR + ane_h15_adt_pad4(len);
	}
	for (k = 0; k < i; k++) {
		int e = ANE_H15_OK;
		struct ane_h15_adt_node s = ane_h15_adt_node_at(a, off, &e);

		if (e)
			return e;
		off = ane_h15_adt_subtree_end(a, s, &e);
		if (e)
			return e;
	}
	*out = off;
	return ANE_H15_OK;
}

static int ane_h15_adt_prop(const struct ane_h15_adt *a,
			    struct ane_h15_adt_node n, const char *name,
			    const u8 **data, u32 *size)
{
	u32 off = n.off + 8;
	unsigned int i;

	for (i = 0; i < n.nprops; i++) {
		u32 len;

		if (off + ANE_H15_ADT_PROP_HDR > a->len)
			return ANE_H15_EFORMAT;
		len = ane_h15_adt_rd32(a, off + ANE_H15_ADT_NAME_LEN) & 0x7fffffffu;
		if (off + ANE_H15_ADT_PROP_HDR + ane_h15_adt_pad4(len) > a->len)
			return ANE_H15_EFORMAT;
		if (!strncmp((const char *)a->b + off, name, ANE_H15_ADT_NAME_LEN)) {
			*data = a->b + off + ANE_H15_ADT_PROP_HDR;
			*size = len;
			return ANE_H15_OK;
		}
		off += ANE_H15_ADT_PROP_HDR + ane_h15_adt_pad4(len);
	}
	return ANE_H15_ENOENT;
}

/* Root child whose "name" property equals @name ("arm-io"). */
static int ane_h15_adt_child_named(const struct ane_h15_adt *a,
				   struct ane_h15_adt_node parent,
				   const char *name,
				   struct ane_h15_adt_node *out)
{
	unsigned int i;

	for (i = 0; i < parent.nchildren; i++) {
		struct ane_h15_adt_node c;
		const u8 *nm;
		u32 size, off;
		int err;

		err = ane_h15_adt_nth_child_off(a, parent, i, &off);
		if (err)
			return err;
		c = ane_h15_adt_node_at(a, off, &err);
		if (err)
			return err;
		if (ane_h15_adt_prop(a, c, "name", &nm, &size))
			continue;
		if (size > 0 && !strncmp((const char *)nm, name, size) &&
		    !name[size - 1])
			continue; /* names match by length, but check exact NUL */
		if (strncmp((const char *)nm, name, strlen(name)) == 0 &&
		    (size <= strlen(name) || nm[strlen(name)] == 0)) {
			*out = c;
			return ANE_H15_OK;
		}
	}
	return ANE_H15_ENOENT;
}

/* /arm-io's child whose ane-type is @ane_type. */
static int ane_h15_adt_find_ane(const struct ane_h15_adt *a, u32 ane_type,
				struct ane_h15_adt_node *out)
{
	struct ane_h15_adt_node root, arm_io;
	unsigned int i;
	int err;

	if (a->len < 8)
		return ANE_H15_EFORMAT;
	root = ane_h15_adt_node_at(a, 0, &err);
	if (err)
		return err;
	err = ane_h15_adt_subtree_end(a, root, &err);
	if (err)
		return err;
	err = ane_h15_adt_child_named(a, root, "arm-io", &arm_io);
	if (err)
		return err;
	for (i = 0; i < arm_io.nchildren; i++) {
		struct ane_h15_adt_node c;
		const u8 *data;
		u32 size, off;
		int perr;

		err = ane_h15_adt_nth_child_off(a, arm_io, i, &off);
		if (err == ANE_H15_ENOENT)
			break;
		if (err)
			return err;
		c = ane_h15_adt_node_at(a, off, &err);
		if (err)
			return err;
		perr = ane_h15_adt_prop(a, c, "ane-type", &data, &size);
		if (perr == ANE_H15_ENOENT)
			continue;
		if (perr)
			return perr;
		if (size >= 4 &&
		    ((u32)data[0] | (u32)data[1] << 8 |
		     (u32)data[2] << 16 | (u32)data[3] << 24) == ane_type) {
			*out = c;
			return ANE_H15_OK;
		}
	}
	return ANE_H15_ENOENT;
}

/* Decode "segment-ranges": either two {phys, iova, size} entries (24
 * bytes each) or two {phys, x, iova, size} entries (32). The caller
 * logs the decoded numbers so a wrong pick is visible. @text_expect
 * is the pinned TEXT size (0 skips); @data_min the pinned DATA size
 * floor (0 skips). */
static int ane_h15_adt_segments(const struct ane_h15_adt *a,
				struct ane_h15_adt_node ane,
				struct ane_h15_seg *segs,
				u64 text_expect, u64 data_min)
{
	const u8 *data;
	u32 size, stride;
	size_t base, i;
	unsigned int k;
	int err;

	err = ane_h15_adt_prop(a, ane, "segment-ranges", &data, &size);
	if (err)
		return err;
	if (size % 2)
		return ANE_H15_EFORMAT;
	stride = size / 2;
	if (stride != 24 && stride != 32)
		return ANE_H15_EFORMAT;
	base = (size_t)(data - a->b);
	for (i = 0; i < ANE_H15_ADT_SEG_MAX; i++) {
		u64 v[4] = { 0, 0, 0, 0 };
		size_t row = base + i * stride;

		if (row + stride > a->len)
			return ANE_H15_EFORMAT;
		for (k = 0; k < stride / 4; k++)
			v[k] = ane_h15_adt_rd32(a, row + 4u * k);
		segs[i].phys = v[0];
		segs[i].iova = stride == 32 ? v[2] : v[1];
		segs[i].size = stride == 32 ? v[3] : v[2];
		if (!segs[i].phys || !segs[i].iova || !segs[i].size)
			return ANE_H15_EFORMAT;
		if (segs[i].size & (ANE_H15_ADT_PROP_ALIGN - 1))
			return ANE_H15_EFORMAT;
	}
	if (text_expect && segs[0].size != text_expect)
		return ANE_H15_EFORMAT;
	if (data_min && segs[1].size < data_min)
		return ANE_H15_EFORMAT;
	return ANE_H15_OK;
}

#endif /* __ANE_H15_ADT_H__ */
