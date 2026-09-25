// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#ifndef __ANE_BIND_H__
#define __ANE_BIND_H__

#include <stdint.h>
#include <string.h>

#include "ane.h"

/*
// Role-to-channel binding, read out of the task stream.
//
// The engine reads and writes the tile channels its own task descriptor
// names. The ANEC header's tiles[] and nchw[] are indexed by that same
// channel number, and the driver binds handles[bdx] to channel bdx, so the
// numbering is shared all the way down. The one fact the header never states
// is which channel carries "input 0" and which carries "output 0" -- the
// role order. A host that assumes an order is guessing.
//
// H13 task descriptor: ten header words, eleven when word 9 has both of its
// low bits set, then register records headed by
// ((word count - 1) << 26) | register byte address. Header word 8 packs the
// three tile-DMA surface selectors, five bits each, and each surface's DMA
// configuration register says whether that surface is live:
//
//   0x13800  source 1     bits 0:4
//   0x13804  source 2     bits 6:10
//   0x17800  destination  bits 12:16
//
// Channels below 4 are never runtime surfaces: 0 is the command channel, 1
// is the kernel BAR a folded constant blob reads through, 3 is scratch. That
// filter is also what separates a real output write from a fused chain's
// intermediate: an in-flight result keeps its destination selector on
// channel 0 and stays in L2, so only a write naming an allocated surface
// channel is the model's output. The destination configuration word itself
// carries no such distinction -- 0x000000c1, 0x000000c0 and 0x040000c1 all
// appear on real output writes across the decoded encoders.
//
// Roles are ordered by ascending channel number, which is the order both
// proven H13 program families already carry: their task selector fields
// appear in either order (0x00024966 and 0x000249a5 both place the
// destination on channel 4 with sources on 5 and 6), so field position
// carries no role and the channel number does.
*/

#define ANE_BIND_FIRST_SURFACE	4
#define ANE_BIND_DMA_DISABLED	0x00008880u
#define ANE_BIND_DST_REGISTER	0x17800u
#define ANE_BIND_SELECTOR_MASK	0x1fu
#define ANE_BIND_MIN_TASK_BYTES	40

/* The three selectors: DMA configuration register, selector bit shift in word
 * 8, and the two registers that hold the surface's byte size. Source 1 and the
 * destination program their surface size into both words (the larger is the
 * extent the engine moves): on all 990 live accesses of the 38 staged-Qwen
 * programs it equals the compiled surface size the HWX declares. Source 2's
 * size registers are not decoded (0 = unknown). */
static const uint32_t ane_bind_selectors[3][4] = {
	{ 0x13800u, 0, 0x13814u, 0x13818u },
	{ 0x13804u, 6, 0, 0 },
	{ ANE_BIND_DST_REGISTER, 12, 0x17810u, 0x17814u },
};

static inline uint32_t ane_bind_word(const uint8_t *task, uint64_t index)
{
	return (uint32_t)task[index * 4] | ((uint32_t)task[index * 4 + 1] << 8) |
	       ((uint32_t)task[index * 4 + 2] << 16) |
	       ((uint32_t)task[index * 4 + 3] << 24);
}

/* Collect the three tile-DMA configuration words of one task image and the
 * byte extent each slot programs (0 when not decoded). Returns 0 when the
 * register-record walk stays inside the task, -1 when it does not. */
static inline int ane_bind_task_dma(const uint8_t *task, uint64_t bytes,
				    uint32_t dma[3], uint32_t extent[3])
{
	const uint64_t words = bytes / 4;
	uint64_t index;
	uint64_t count;
	uint64_t offset;
	uint32_t header;
	uint32_t base;
	uint32_t reg;
	uint32_t value;
	int slot;

	for (slot = 0; slot < 3; slot++) {
		dma[slot] = ANE_BIND_DMA_DISABLED;
		extent[slot] = 0;
	}

	if (bytes < ANE_BIND_MIN_TASK_BYTES || bytes % 4)
		return -1;

	index = 10 + (((ane_bind_word(task, 9) & 3) == 3) ? 1 : 0);

	while (index < words) {
		header = ane_bind_word(task, index);
		count = (header >> 26) + 1;
		base = header & 0x03ffffffu;
		if (index + count >= words)
			return -1;
		for (offset = 0; offset < count; offset++) {
			reg = base + (uint32_t)offset * 4;
			value = ane_bind_word(task, index + 1 + offset);
			for (slot = 0; slot < 3; slot++) {
				if (reg == ane_bind_selectors[slot][0])
					dma[slot] = value;
				else if (ane_bind_selectors[slot][2] &&
					 (reg == ane_bind_selectors[slot][2] ||
					  reg == ane_bind_selectors[slot][3]) &&
					 value > extent[slot])
					extent[slot] = value;
			}
		}
		index += 1 + count;
	}

	return 0;
}

/* Mark every runtime surface channel the linked task stream selects, and
 * record in extent[] (TILE_COUNT entries, may be NULL) the largest byte extent
 * any task moves through each live surface channel, allocated or not. */
static inline int ane_bind_walk(const struct anec *anec, const uint8_t *stream,
				uint64_t stream_size, uint8_t *is_src,
				uint8_t *is_dst, uint64_t *extent)
{
	uint64_t offset = 0;
	uint64_t next;
	uint64_t bytes = anec->td_size;
	uint32_t index;
	uint32_t dma[3];
	uint32_t size[3];
	uint32_t selectors;
	uint32_t channel;
	int slot;

	if (!anec->td_count)
		return -1;

	for (index = 0; index < anec->td_count; index++) {
		if (bytes < ANE_BIND_MIN_TASK_BYTES || offset > stream_size ||
		    bytes > stream_size - offset)
			return -1;
		if (ane_bind_task_dma(stream + offset, bytes, dma, size) < 0)
			return -1;

		selectors = ane_bind_word(stream + offset, 8);
		for (slot = 0; slot < 3; slot++) {
			channel = (selectors >> ane_bind_selectors[slot][1]) &
				  ANE_BIND_SELECTOR_MASK;
			if (dma[slot] == ANE_BIND_DMA_DISABLED)
				continue;
			if (channel < ANE_BIND_FIRST_SURFACE || channel >= TILE_COUNT)
				continue;
			if (extent && size[slot] > extent[channel])
				extent[channel] = size[slot];
			if (!anec->tiles[channel])
				continue;
			if (ane_bind_selectors[slot][0] == ANE_BIND_DST_REGISTER)
				is_dst[channel] = 1;
			else
				is_src[channel] = 1;
		}

		if (index + 1 == anec->td_count)
			break;
		/* Each descriptor points at the next one and carries its word
		 * count: word 7 is the next task's stream offset, word 1 bits
		 * 16:24 are that task's word count less one. */
		next = ane_bind_word(stream + offset, 7);
		bytes = (((ane_bind_word(stream + offset, 1) >> 16) & 0x1ff) +
			 1) * 4;
		if (next % 4 || next > stream_size)
			return -1;
		offset = next;
	}

	return 0;
}

/* The positional layout libane shipped before the task stream was read: it
 * holds for every program whose producer normalized its selectors into it. */
static inline void ane_bind_positional(const struct anec *anec,
				       struct ane_bind *bind)
{
	uint32_t idx;

	for (idx = 0; idx < TILE_COUNT; idx++) {
		bind->dst[idx] = (uint8_t)(ANE_BIND_FIRST_SURFACE + idx);
		bind->src[idx] = (uint8_t)(ANE_BIND_FIRST_SURFACE +
					   anec->dst_count + idx);
	}
}

/* Derive the role-to-channel map. Falls back to the positional layout when the
 * task stream does not name exactly the surfaces the header counts, so a
 * program shape this decode does not cover keeps its previous binding rather
 * than acquiring a new guess. Returns 1 when the map was derived, 0 when it
 * was assumed. */
static inline int ane_bind_init(const struct anec *anec, const void *stream,
				uint64_t stream_size, struct ane_bind *bind)
{
	uint8_t is_src[TILE_COUNT];
	uint8_t is_dst[TILE_COUNT];
	struct ane_bind derived;
	uint32_t channel;
	uint32_t srcs = 0;
	uint32_t dsts = 0;

	ane_bind_positional(anec, bind);

	if (anec->src_count > TILE_COUNT || anec->dst_count > TILE_COUNT)
		return 0;

	memset(is_src, 0, sizeof(is_src));
	memset(is_dst, 0, sizeof(is_dst));
	if (ane_bind_walk(anec, (const uint8_t *)stream, stream_size, is_src,
			  is_dst, NULL) < 0)
		return 0;

	for (channel = ANE_BIND_FIRST_SURFACE; channel < TILE_COUNT;
	     channel++) {
		if (is_dst[channel]) {
			if (dsts == TILE_COUNT)
				return 0;
			derived.dst[dsts++] = (uint8_t)channel;
		} else if (is_src[channel]) {
			if (srcs == TILE_COUNT)
				return 0;
			derived.src[srcs++] = (uint8_t)channel;
		}
	}

	/* Apple's streams leave some surfaces unnamed by the selector
	 * registers (an island program never enables its second source
	 * selector, yet binds it on the next allocated channel). Such
	 * surfaces bind on the first unused allocated channel ascending,
	 * destinations first, then sources -- mirroring the mlx-omarchy
	 * overlay bundle parser's derivation, which the 458 MB
	 * whole-encoder program runs hardware-proven. */
	for (channel = ANE_BIND_FIRST_SURFACE;
	     channel < TILE_COUNT &&
	     (dsts < anec->dst_count || srcs < anec->src_count);
	     channel++) {
		if (is_dst[channel] || is_src[channel] ||
		    anec->tiles[channel] == 0)
			continue;
		if (dsts < anec->dst_count)
			derived.dst[dsts++] = (uint8_t)channel;
		else
			derived.src[srcs++] = (uint8_t)channel;
	}

	if (srcs != anec->src_count || dsts != anec->dst_count)
		return 0;

	memcpy(bind->src, derived.src, srcs);
	memcpy(bind->dst, derived.dst, dsts);

	return 1;
}

/* The last byte offset (exclusive) ane_tile/ane_untile touch in a channel for
 * one fp16 header geometry: N * C * plane on the dense fast path and for the
 * memset, else the end of the last row they copy. UINT64_MAX when the
 * geometry cannot be tiled at all (rows narrower than W fp16 elements --
 * e.g. a 1-byte bool surface, which only raw consumers may move). */
static inline uint64_t ane_bind_tile_span(const uint64_t nchw[6])
{
	const uint64_t n = nchw[0], c = nchw[1], h = nchw[2], w = nchw[3];
	const uint64_t plane = nchw[4], row = nchw[5];
	uint64_t rows, cols, last;

	if (!n || !c || !h || !w)
		return 0;
	if (row < w * sizeof(uint16_t))
		return UINT64_MAX;
	rows = plane / row;
	cols = row / sizeof(uint16_t);
	last = (((n - 1) * c + (c - 1)) * rows * cols + (h - 1) * cols + w) *
	       sizeof(uint16_t);
	return last > n * c * plane ? last : n * c * plane;
}

/* Find a surface channel the engine would overrun: a live source-1 or
 * destination transfer whose programmed byte size exceeds the channel's
 * allocation (tiles << tile_shift, the BO the driver maps -- a write past it
 * lands in whatever the DART maps next). Returns 1 with the channel, the
 * bytes the task moves and the bytes allocated; 0 when every measured
 * transfer fits, or when the stream cannot be walked (nothing measured).
 * Source 2's size registers are not decoded. */
static inline int ane_bind_overrun(const struct anec *anec, const void *stream,
				   uint64_t stream_size, uint32_t tile_shift,
				   uint32_t *channel, uint64_t *need,
				   uint64_t *have)
{
	uint8_t is_src[TILE_COUNT];
	uint8_t is_dst[TILE_COUNT];
	uint64_t extent[TILE_COUNT];
	uint64_t alloc;
	uint32_t ch;

	memset(is_src, 0, sizeof(is_src));
	memset(is_dst, 0, sizeof(is_dst));
	memset(extent, 0, sizeof(extent));
	if (ane_bind_walk(anec, (const uint8_t *)stream, stream_size, is_src,
			  is_dst, extent) < 0)
		return 0;

	for (ch = ANE_BIND_FIRST_SURFACE; ch < TILE_COUNT; ch++) {
		alloc = (uint64_t)anec->tiles[ch] << tile_shift;
		if (extent[ch] > alloc) {
			*channel = ch;
			*need = extent[ch];
			*have = alloc;
			return 1;
		}
	}
	return 0;
}

#endif /* __ANE_BIND_H__ */
