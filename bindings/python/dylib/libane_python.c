// SPDX-License-Identifier: MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#include <stddef.h>

#include <ane.h>

void *pyane_init(char *path, int dev_id)
{
	return __ane_init(path, dev_id);
}

void *pyane_chan_map(struct ane_nn *nn, int bdx)
{
	if (!nn || bdx < 0 || bdx >= TILE_COUNT)
		return NULL;
	return nn->chans[bdx].map;
}

int pyane_free(struct ane_nn *nn)
{
	ane_free(nn);
	return 0;
}

/* Explicit role-to-channel binding: src/dst hold the channel of each input and
 * output role, in role order (io_layout.py derives them from the compiled HWX's
 * channel table). Every role must land on an allocated surface channel; the
 * load-time overrun check has already bounded each such channel's geometry. */
int pyane_bind_load(struct ane_nn *nn, const unsigned int *src,
		    const unsigned int *dst)
{
	const uint32_t counts[2] = { nn ? nn->anec.src_count : 0,
				     nn ? nn->anec.dst_count : 0 };
	const unsigned int *maps[2] = { src, dst };
	uint32_t i;
	int role;

	if (!nn)
		return -1;
	for (role = 0; role < 2; role++)
		for (i = 0; i < counts[role]; i++)
			if (maps[role][i] < 4 || maps[role][i] >= TILE_COUNT ||
			    !nn->anec.tiles[maps[role][i]])
				return -1;
	for (i = 0; i < TILE_COUNT; i++) {
		nn->bind.src[i] = (uint8_t)src[i];
		nn->bind.dst[i] = (uint8_t)dst[i];
	}
	return 0;
}

int pyane_exec(struct ane_nn *nn)
{
	int err = ane_exec(nn);
	return err;
}

/* I've concluded this is the sanest way to handle (not) variadic args */

int pyane_send(struct ane_nn *nn, void *x0, void *x1, void *x2, void *x3,
	       void *x4, void *x5, void *x6, void *x7, void *x8, void *x9,
	       void *x10, void *x11, void *x12, void *x13, void *x14, void *x15,
	       void *x16, void *x17, void *x18, void *x19, void *x20, void *x21,
	       void *x22, void *x23, void *x24, void *x25, void *x26, void *x27,
	       void *x28, void *x29, void *x30, void *x31)
{
	void *xs[TILE_COUNT] = { x0,  x1,  x2,	x3,  x4,  x5,  x6,  x7,
				 x8,  x9,  x10, x11, x12, x13, x14, x15,
				 x16, x17, x18, x19, x20, x21, x22, x23,
				 x24, x25, x26, x27, x28, x29, x30, x31 };
	for (uint32_t i = 0; i < ane_src_count(nn); i++)
		if (__ane_tile_send(nn, xs[i], i) < 0)
			return -1;
	return 0;
}

int pyane_read(struct ane_nn *nn, void *x0, void *x1, void *x2, void *x3,
	       void *x4, void *x5, void *x6, void *x7, void *x8, void *x9,
	       void *x10, void *x11, void *x12, void *x13, void *x14, void *x15,
	       void *x16, void *x17, void *x18, void *x19, void *x20, void *x21,
	       void *x22, void *x23, void *x24, void *x25, void *x26, void *x27,
	       void *x28, void *x29, void *x30, void *x31)
{
	void *xs[TILE_COUNT] = { x0,  x1,  x2,	x3,  x4,  x5,  x6,  x7,
				 x8,  x9,  x10, x11, x12, x13, x14, x15,
				 x16, x17, x18, x19, x20, x21, x22, x23,
				 x24, x25, x26, x27, x28, x29, x30, x31 };
	for (uint32_t i = 0; i < ane_dst_count(nn); i++)
		if (__ane_tile_read(nn, xs[i], i) < 0)
			return -1;
	return 0;
}
