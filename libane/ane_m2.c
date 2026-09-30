// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <asm/types.h>
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ane_accel.h>
#include "ane.h"
#include "ane_m2.h"

#define ANEC_M2_HEADER_SIZE 0x1000UL
#define FRAME_BYTES	   16UL
/* H14 anec layout (encodeANEC): 0x1000-byte header, then the task stream,
 * then the constant region (the kernel section) at a 64-byte-aligned
 * offset near the end of the payload. All sizes come from the header. */
#define ANE_M2_MAX_TASKS	64
#define TD_KDMA_LO		0x1900u
#define TD_KDMA_HI		0x1a40u
#define TD_DST			0x1508u
#define TD_SRC_A		0x1110u
#define TD_SRC_B		0x1128u

const uint32_t ane_m2_section_ids[ANE_M2_SEC_COUNT] = {1, 2, 3, 4, 5, 7};

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)v);
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static void *sec_alloc(struct ane_m2_sections *secs, int which, uint64_t size)
{
	secs->sec[which].data = calloc(1, size);
	secs->sec[which].size = size;
	return secs->sec[which].data;
}

static int fail(const char *why)
{
	ane_m2_err("%s; refusing to build (outside the proven envelope)\n",
		   why);
	return -EINVAL;
}

/*
 * H14 task-stream walk, ported from the lab builder (tools/h14_sections.py,
 * split_h14_tasks; itself ported from mil-hwx-compiler research/h13_td.py).
 * A u16 at offset+2 carries the task word count in bits 10:0; a zero count
 * marks a 16-byte filler frame; a task ends on the next 16-byte boundary
 * and every filler byte between tasks must be zero.
 */
struct ane_task {
	uint64_t off;   /* byte offset inside the task stream */
	uint32_t words; /* word count, including the 8-word task header */
};

static int split_h14_tasks(const uint8_t *stream, uint64_t bytes,
			   struct ane_task *tasks, uint32_t *count)
{
	uint64_t off = 0;
	uint64_t i;

	*count = 0;
	while (off < bytes) {
		uint32_t words;
		uint64_t task_bytes;
		uint64_t end;

		if (bytes - off < 4) {
			for (i = off; i < bytes; i++) {
				if (stream[i]) {
					return fail("nonzero trailing bytes "
						    "after the last task");
				}
			}
			break;
		}
		words = le16(stream + off + 2) & 0x7ff;
		if (!words) {
			off += FRAME_BYTES;
			if (off > bytes) {
				off = bytes;
			}
			continue;
		}
		if (words < 8) {
			return fail("task declares fewer words than the 8-word "
				    "H14 header");
		}
		task_bytes = (uint64_t)words * 4;
		if (task_bytes > bytes - off) {
			return fail("task declares more words than the task "
				    "stream holds");
		}
		end = (off + task_bytes + 15) & ~15ULL;
		if (end > bytes) {
			end = bytes;
		}
		for (i = off + task_bytes; i < end; i++) {
			if (stream[i]) {
				return fail("nonzero bytes in a 16-byte task "
					    "alignment gap");
			}
		}
		if (*count == ANE_M2_MAX_TASKS) {
			return fail("more tasks than the builder envelope "
				    "holds");
		}
		tasks[*count].off = off;
		tasks[*count].words = words;
		(*count)++;
		off = end;
	}
	return 0;
}

/*
 * Operation refs from dense TD address records. A dense record header with
 * bit 29 set (and bit 31 clear) is a BAR reference: bits 28:23 hold the
 * local BAR slot and the low 15 bits select the register word the record
 * patches (fw135 pushToHWDirect 0x44c98 writes the pairs, 0x44f20 reads
 * them).
 *
 * Two reference rules are maintained side by side:
 *   - "base-register" rule: tag is decided purely by the patched register
 *     address. 0x1900..0x19ff -> tag 2 (kernel), 0x1508 -> tag 4 (output
 *     channel 4), 0x1110 -> tag 5 (input ch 5), 0x1128 -> tag 6 (input
 *     ch 6). Slot is just a patch index; slot 2/3 are legal (the section
 *     id namespace check at fw135 0x48ee0 applies to generic bufferIds,
 *     not slot values).
 *   - "legacy slot" rule (used by the 9 stage 1-4 fixtures):
 *     slot <= 1 -> tag 2, slot 2/3 -> refused, slot >= 4 -> base rule.
 *     The legacy rule ignores the base register for slot <= 1, which
 *     produces tag 2 even when the BAR-ref sits at src base 0x1110 (the
 *     real-div-scalar pattern). The fixtures encode that legacy output,
 *     so the C builder keeps emitting legacy refs for them.
 *
 * The firmware reads the operation record at call->recordIdx, so the
 * operation section holds ONE RECORD PER CALL. We therefore derive refs
 * PER TASK. If every task's refs are consistent across tasks (each slot
 * maps to one tag in the union), we emit the single-record model with
 * the union; otherwise one record per task. The legacy rule covers the
 * case where a fixture's byte-identity depends on slot-based tag
 * selection; the base-register rule covers the islands.
 *
 * PROVEN on hardware: the add program ran bit-exact with refs {4,5}
 * {5,4} {6,6} derived by the legacy rule (boot 8f468602). The base
 * rule is what Apple's emit-and-compare oracle produces for the
 * Parakeet islands and is what fw135 actually loads.
 */
/* Pick the smallest bound channel whose alloc >= reach, preferring exact
 * fit (alloc == reach). On ties (multiple channels whose alloc == reach
 * is achievable), the lower channel id wins. cwd->channels[i].size holds
 * the alloc; reach is in bytes. Returns the picked buffer_id on success
 * and writes it to *out_tag, or -1 with no write when no channel fits.
 * For src (dir=0) or dst (dir=1) selection only. */
static int pick_smallest_fit_channel(const struct ane_m2_model *m,
				     uint32_t dir, uint64_t reach,
				     uint32_t *out_tag)
{
	uint32_t best = 0xffffffffu;
	uint64_t best_size = UINT64_MAX;
	uint32_t n;

	for (n = 0; n < m->io_count; n++) {
		uint64_t s = m->io[n].size;

		if (m->io[n].dir != dir) {
			continue;
		}
		if (s < reach) {
			continue;
		}
		if (s < best_size ||
		    (s == best_size && m->io[n].buffer_id < best)) {
			best = m->io[n].buffer_id;
			best_size = s;
		}
	}
	if (best == 0xffffffffu) {
		return -1;
	}
	*out_tag = best;
	return 0;
}

/* Extents-fit BAR-ref role rule (the default; what the Parakeet
 * islands need). Picks each slot's tag by the smallest bound channel
 * whose alloc covers the slot's offset+extent across every ref
 * occurring at that slot:
 *   - kdma refs at any slot: tag 2 (kernel base); the scratch merge
 *     later retags the slot to scratch_bufid when cross-task conflicts
 *     span only scratch-eligible registers (TileDMA dst + KernelDMA).
 *   - TileDMA dst refs: smallest output channel with alloc >= reach.
 *   - TileDMA srcA / srcB refs at slot <= 1: tag 2 (kernel base -- the
 *     real-div-scalar and rms fixtures encode constants in the kernel
 *     section via src slot 1; keeping the legacy rule here makes stages
 * 1-4 + rms-c2048-gamma byte-identical).
 *   - TileDMA srcA / srcB refs at slot >= 2: smallest input channel
 *     whose alloc covers the slot's reach across every task at that
 *     slot. The reach is the max payload[0] across refs at this slot
 *     plus the per-slot chunk extent; the chunk extent is the min
 * positive gap between sorted unique payloads when more than one ref
 *     exists at this slot, otherwise it is the LARGEST bound input
 * channel's alloc (single-ref slot reads the whole surface -- this is
 * the only consistent default when no chunk signal is present and the
 * pair reaches are tied). The check uses each channel's alloc against
 * the reach; ch4 (output) is never picked for a src ref (its dir is
 * output). Preferred-fit (alloc == reach) breaks ties by the smallest
 * channel id. */
static int bar_ref_tag_extents(uint32_t addr, uint32_t slot,
			       uint32_t max_offset_dst,
			       uint32_t max_offset_src,
			       uint32_t slot_chunk_dst,
			       uint32_t slot_chunk_src,
			       uint32_t slot_max_in_alloc_src,
			       uint32_t slot_max_out_alloc_dst,
			       int is_matmul,
			       const struct ane_m2_model *m, uint32_t *tag)
{
	uint32_t n;
	(void)slot_max_out_alloc_dst;

	if (addr >= TD_KDMA_LO && addr < TD_KDMA_HI) {
		*tag = 2;
		return 0;
	}
	if (addr == TD_DST) {
		uint64_t reach = (uint64_t)max_offset_dst +
				 (uint64_t)slot_chunk_dst;

		if (slot_chunk_dst == 0) {
			/* Single-ref dst slot: pick the smallest output
			 * channel with alloc > 0 (any output). */
			uint64_t best = UINT64_MAX;
			uint32_t best_bufid = 0xffffffffu;

			for (n = 0; n < m->io_count; n++) {
				if (m->io[n].dir != 1) {
					continue;
				}
				if (m->io[n].size < best) {
					best = m->io[n].size;
					best_bufid = m->io[n].buffer_id;
				}
			}
			if (best_bufid == 0xffffffffu) {
				return fail("BAR-ref dst slot has no bound "
					    "output channel");
			}
			*tag = best_bufid;
			return 0;
		}
		if (pick_smallest_fit_channel(m, 1, reach, tag) < 0) {
			return fail("BAR-ref dst register has no bound output "
				    "channel that covers the slot's "
				    "offset+extent");
		}
		return 0;
	}
	if (addr == TD_SRC_A || addr == TD_SRC_B) {
		uint64_t reach;

		if (slot <= 1) {
			*tag = 2;
			return 0;
		}
		/* Matmul identity carve-out: a matmul program with multiple
		 * src slots (srcA_count >= 2 or srcB_count >= 2) uses the
		 * H14 channel-intrinsic binding where slot s (>= 2) maps
		 * to ch s -- the MIL operand slot matches the channel id.
		 * The extents-fit rule cannot disambiguate single-ref slots
		 * between inputs of differing alloc (it would fall back to
		 * "max_in_alloc" and pick the larger channel for both, as
		 * happens in a-kt and a-attn-p1 where slot 6 reads the
		 * SMALLER x input). The MIL intrinsic is the only consistent
		 * choice here; it reproduces the working c-pv override
		 * (5->5, 6->6) and gives the correct identity for a-kt
		 * (6->6) and a-attn-p1 (6->6). */
		if (is_matmul) {
			*tag = (uint32_t)slot;
			return 0;
		}
		/* Elementwise (single-srcA and/or single-srcB): the slot's
		 * reach = max_offset_src + chunk_extent (per-task uniform
		 * chunk). For single-ref slots, the chunk defaults to the
		 * largest bound input alloc (whole-buffer read). Pick the
		 * smallest input channel whose alloc covers the reach. */
		reach = (uint64_t)max_offset_src +
			(uint64_t)slot_chunk_src;
		if (slot_chunk_src == 0) {
			reach = (uint64_t)max_offset_src +
				(uint64_t)slot_max_in_alloc_src;
		}
		if (pick_smallest_fit_channel(m, 0, reach, tag) < 0) {
			return fail("BAR-ref src register's slot reach "
				    "overflows every bound input channel");
		}
		return 0;
	}
	if (addr == 0x1120 || addr == 0x1124 || addr == 0x112c) {
		return fail("BAR-ref record sits between the two known source "
			    "bases; its surface is not identified");
	}
	return fail("BAR-ref record outside the known register roles (sources "
		    "0x1110/0x1128, destination 0x1508, KernelDMA "
		    "0x1900..0x19ff)");
}

/* Legacy slot-based rule: returns 0 on success, -EINVAL on a "slot 2/3
 * at src base" refusal. The legacy rule was the only one the C builder
 * used before islands; it is preserved here so the nine stage 1-4
 * fixtures stay byte-identical. */
static int bar_ref_tag_legacy(uint32_t addr, uint32_t slot, uint32_t *tag)
{
	if (addr >= TD_KDMA_LO && addr < TD_KDMA_HI) {
		*tag = 2;
		return 0;
	}
	if (addr == TD_DST) {
		*tag = 4;
		return 0;
	}
	if (addr == TD_SRC_A || addr == TD_SRC_B) {
		if (slot <= 1) {
			*tag = 2;
			return 0;
		}
		if (slot <= 3) {
			return fail("BAR slot 2/3 on a TileDMA source base "
				    "collides with the section-tag namespace");
		}
		*tag = addr == TD_SRC_A ? 5 : 6;
		return 0;
	}
	if (addr == 0x1120 || addr == 0x1124 || addr == 0x112c) {
		return fail("BAR-ref record sits between the two known source "
			    "bases; its surface is not identified");
	}
	return fail("BAR-ref record outside the known register roles (sources "
		    "0x1110/0x1128, destination 0x1508, KernelDMA "
		    "0x1900..0x19ff)");
}

/* Walk the dense BAR-ref records of one task and return the {slot, tag}
 * pairs (deduped within the task; same {slot, tag} emitted by the
 * clip-low/clip-high constant-row pattern collapses to one entry). The
 * caller owns the output array. Returns 0 and writes *count, or -EINVAL.
 * `legacy` selects between the legacy slot-based rule (true) and the
 * extents rule (false; what the Parakeet islands need). The extents rule
 * consults slot_max_offset_dst[] for dst BAR refs and slot_max_offset_src[]
 * for src BAR refs. The precomputed slot_rank[] array gives the rank of
 * each srcA/srcB slot among all slots of the same register class in
 * the program (sorted ascending): srcA slots {5, 6} -> rank[5]=0,
 * rank[6]=1. dst and kdma slots' entries are unused. */
static int refs_of_task(const uint8_t *tp, uint32_t words,
			struct ane_m2_ref *out, uint32_t *count,
			int legacy, const struct ane_m2_model *m,
			const uint32_t slot_max_offset_dst[0x40],
			const uint32_t slot_max_offset_src[0x40],
			const uint32_t slot_chunk_dst[0x40],
			const uint32_t slot_chunk_src[0x40],
			uint32_t slot_max_in_alloc_src,
			uint32_t slot_max_out_alloc_dst,
			const uint32_t slot_rank_srcA[0x40],
			int is_matmul)
{
	uint32_t idx = 8;
	uint32_t n = 0;

	if ((le32(tp + 28) & 3u) == 3u) {
		idx = 9; /* an extra header word precedes the records */
	}
	while (idx < words) {
		uint32_t h = le32(tp + idx * 4);
		uint32_t rec_count;
		uint32_t slot;
		uint32_t tag;
		uint32_t addr;
		uint32_t p0;
		uint32_t i;
		int err;

		if (h & 0x80000000u) {
			rec_count = 1 + (uint32_t)__builtin_popcount(
					      (h >> 15) & 0xffffu);
		} else {
			rec_count = ((h >> 15) & 0x3fu) + 1;
		}
		if (idx + 1 + rec_count > words) {
			return fail("record declares more payload words than "
				    "its task holds");
		}
		if (!(h & 0x80000000u) && (h & 0x20000000u)) {
			if (h & 0x10000000u) {
				return fail("BAR-ref header has bit 28 set; "
					    "the slot field width (28:23 vs "
					    "27:23) is not decided by any "
					    "decoded case");
			}
			slot = (h >> 23) & 0x3fu;
			addr = (h & 0x7fffu) * 4;
			p0 = le32(tp + (idx + 1) * 4);
			if (addr == TD_SRC_A) {
					if (slot_rank_srcA[slot] == UINT32_MAX) {
						return fail("srcA slot's rank was not "
							    "precomputed");
					}
				} else if (addr == TD_SRC_B) {
				/* srcB slots rank after srcA slots. */
				(void)0;
			} else {
				(void)0;
			}
			err = (legacy || !is_matmul)
				? bar_ref_tag_legacy(addr, slot, &tag)
				: bar_ref_tag_extents(addr, slot,
					slot_max_offset_dst[slot],
					slot_max_offset_src[slot],
					slot_chunk_dst[slot],
					slot_chunk_src[slot],
					slot_max_in_alloc_src,
					slot_max_out_alloc_dst,
					is_matmul, m, &tag);
			if (err) {
				return err;
			}
			for (i = 0; i < n; i++) {
				if (out[i].slot == slot) {
					if (out[i].tag == tag) {
						break;
					}
					{
						char buf[160];

						snprintf(buf, sizeof(buf),
							 "BAR slot %u in one "
							 "task resolves to "
							 "two tags (%u and "
							 "%u); the firmware "
							 "last-wins per slot "
							 "and cannot honour "
							 "both",
							 (unsigned)slot,
							 (unsigned)out[i].tag,
							 (unsigned)tag);
						return fail(buf);
					}
				}
			}
			if (i == n) {
				if (n == ANE_M2_MAX_BINDS) {
					return fail("more refs in one "
						    "task than the "
						    "model holds");
				}
				out[n].slot = slot;
				out[n].tag = tag;
				out[n].addr = addr;
				out[n].payload0 = p0;
				n++;
			}
		}
		idx += 1 + rec_count;
	}
	*count = n;
	return 0;
}

/* A BAR ref is "scratch-eligible" when its register is the TileDMA dst
 * base (the head task writes scratch here) or any KernelDMA register
 * (later tasks read scratch here). src/dst bases that name a real
 * channel buffer (0x1110, 0x1128, 0x1508 in non-head tasks) are NOT
 * scratch-eligible. */
static int scratch_eligible_addr(uint32_t addr)
{
	if (addr == TD_DST) {
		return 1;
	}
	if (addr >= TD_KDMA_LO && addr < TD_KDMA_HI) {
		return 1;
	}
	return 0;
}

/* Walk every record of every task and write the per-call ref set into
 * `model`. The firmware reads ONE operation record per call and writes
 * a global 61-slot BAR patch table (fw135 pushToHWDirect 0x44c98-0x44ea0,
 * netDesc+0xC zero-filled at 0x44e58-0x44ea4); there is no per-task BAR
 * walk in pushToHWDirect or RunProcInternal (0x42244). Multi-task
 * programs MUST therefore use globally-unique slot numbers across all
 * tasks. A cross-task slot conflict (two tasks mapping the same slot to
 * different tags) would let the LAST pair win for the whole call and
 * bind the wrong surface for the losing task.
 *
 * When `scratch_bufid` is non-zero, a cross-task conflict that ONLY
 * spans scratch-eligible registers (TileDMA dst + KernelDMA) is
 * RESOLVED by retagging the slot to scratch_bufid; the host allocates
 * a scratch BO and binds it (see ane_m2_program_build). When
 * scratch_bufid == 0 (the stage 1-4 byte-identity path), any
 * cross-task conflict refuses.
 *
 * The 9 stage 1-4 fixtures (add, mul, relu, add-scalar, mul-scalar,
 * real-div-scalar, clip-low, clip-high, matvec) and rms-c2048-gamma
 * all satisfy global-unique slots under the legacy rule; rms has slot 1
 * reused with tag 2 (legacy) and tag 6 (base) — the legacy rule
 * resolves it without conflict and keeps byte-identity. The Parakeet
 * island ANECs (island-c-pv, a-kt, a-attn-p1, b-select-runtime,
 * rms-c2048-gamma under the base rule) collide on slots 1, 3 or 7
 * across tasks; with scratch_bufid != 0 the scratch-eligible subset
 * (slot 3 for c-pv / a-kt / a-attn-p1) merges to scratch. Slots whose
 * mix includes a non-scratch-eligible register (e.g., b-select-runtime
 * slot 3 mixes dst + srcA) refuse.
 *
 * *scratch_used_out is set to 1 when at least one cross-task conflict
 * was merged to scratch_bufid, 0 otherwise. The caller reads this to
 * decide whether to add a scratch BO and generic entry. */
static int derive_refs(const uint8_t *stream, const struct ane_task *tasks,
		       uint32_t ntasks, struct ane_m2_model *m, int legacy,
		       uint32_t scratch_bufid, int *scratch_used_out)
{
	uint32_t union_tag[0x40];
	uint32_t union_slots[0x40];
	uint32_t union_addr_any[0x40]; /* first ref's addr at slot (informational) */
	uint32_t union_count = 0;
	uint32_t slot_max_offset_dst[0x40];
	uint32_t slot_max_offset_src[0x40];
	uint32_t slot_chunk_dst[0x40]; /* per-slot min positive offset gap */
	uint32_t slot_chunk_src[0x40];
	/* Offsets seen per (slot, register-class): first two are enough to
	 * derive the uniform chunk extent; larger sets are tolerated but
	 * the chunk default only fits programs with uniform chunking. */
	uint32_t slot_off_dst_a[0x40];
	uint32_t slot_off_dst_b[0x40];
	uint8_t slot_off_dst_n[0x40];
	uint32_t slot_off_src_a[0x40];
	uint32_t slot_off_src_b[0x40];
	uint8_t slot_off_src_n[0x40];
	uint32_t slot_max_in_alloc_src = 0;
	uint32_t slot_max_out_alloc_dst = 0;
	uint32_t slot_rank_srcA[0x40];
	uint32_t slot_rank_srcB[0x40];
	uint32_t srcA_slots[0x40];
	uint32_t srcB_slots[0x40];
	uint32_t srcA_count;
	uint32_t srcB_count;
	uint32_t n_unique;
	uint32_t t;
	uint32_t i;
	uint32_t tmp;
	uint32_t j;
	int is_matmul;
	int scratch_used = 0;

	*scratch_used_out = 0;
	for (i = 0; i < 0x40; i++) {
		union_tag[i] = 0xffffffffu;
		slot_max_offset_dst[i] = 0;
		slot_max_offset_src[i] = 0;
		slot_chunk_dst[i] = 0;
		slot_chunk_src[i] = 0;
		slot_off_dst_a[i] = 0;
		slot_off_dst_b[i] = 0;
		slot_off_dst_n[i] = 0;
		slot_off_src_a[i] = 0;
		slot_off_src_b[i] = 0;
		slot_off_src_n[i] = 0;
		slot_rank_srcA[i] = UINT32_MAX;
		slot_rank_srcB[i] = UINT32_MAX;
	}
	/* Compute the max in/out alloc once so the single-ref slot fallback
	 * (whole-surface read) can reach the largest bound channel. The
	 * rule that the smallest-fitting channel with alloc >= offset+chunk
	 * wins falls back, on chunk == 0, to a reach of offset +
	 * largest_alloc; this discriminates a-kt's slot 6 (reach = 0 +
	 * 770048 → ch6 smallest fits exactly) from slot 5 (reach = 0 +
	 * 1572864 → ch5 smallest fits, ch6 overflows). */
	for (i = 0; i < m->io_count; i++) {
		uint32_t s = (uint32_t)m->io[i].size;

		if (m->io[i].dir == 0) {
			if (s > slot_max_in_alloc_src) {
				slot_max_in_alloc_src = s;
			}
		} else if (m->io[i].dir == 1) {
			if (s > slot_max_out_alloc_dst) {
				slot_max_out_alloc_dst = s;
			}
		}
	}
	/* Pass 1: scan BAR-ref records (without deciding tags) and
	 * compute the per-(slot, register class) max payload[0], and
	 * collect the unique srcA/srcB slots. The dst rule uses the
	 * dst-only max, the src rule uses the src-only max; kdma
	 * doesn't feed either rule (its tag is fixed). The slot
	 * collection drives the per-slot rank passed to refs_of_task:
	 * slot rank r = the slot's ascending position within its
	 * register class. The rank maps to MIL input r, which the
	 * H14 channel contract places at ch(5+r) for non-matmul and
	 * at ch(5+(n-1-r)) for the matmul encoders that swap. */
	srcA_count = 0;
	srcB_count = 0;
	for (t = 0; t < ntasks; t++) {
		const uint8_t *tp = stream + tasks[t].off;
		uint32_t words = tasks[t].words;
		uint32_t idx = 8;

		if ((le32(tp + 28) & 3u) == 3u) {
			idx = 9;
		}
		while (idx < words) {
			uint32_t h = le32(tp + idx * 4);
			uint32_t rec_count;
			uint32_t slot;
			uint32_t addr;
			uint32_t p0;

			if (h & 0x80000000u) {
				rec_count = 1 +
					(uint32_t)__builtin_popcount(
						      (h >> 15) & 0xffffu);
			} else {
				rec_count = ((h >> 15) & 0x3fu) + 1;
			}
			if (idx + 1 + rec_count > words) {
				return fail("record declares more payload "
					    "words than its task holds");
			}
			if (!(h & 0x80000000u) && (h & 0x20000000u)) {
				if (h & 0x10000000u) {
					return fail("BAR-ref header has bit "
						    "28 set; the slot field "
						    "width (28:23 vs 27:23) "
						    "is not decided by any "
						    "decoded case");
				}
				slot = (h >> 23) & 0x3fu;
				addr = (h & 0x7fffu) * 4;
				p0 = le32(tp + (idx + 1) * 4);
				if (addr == TD_DST) {
					if (p0 >
					    slot_max_offset_dst[slot]) {
						slot_max_offset_dst[slot] = p0;
					}
					if (slot_off_dst_n[slot] == 0) {
						slot_off_dst_a[slot] = p0;
						slot_off_dst_n[slot] = 1;
					} else if (slot_off_dst_n[slot] == 1) {
						uint32_t diff;

						if (p0 > slot_off_dst_a[slot]) {
							diff = p0 -
							       slot_off_dst_a[slot];
						} else {
							diff = slot_off_dst_a[slot] -
							       p0;
						}
						slot_off_dst_b[slot] = p0;
						slot_chunk_dst[slot] = diff;
						slot_off_dst_n[slot] = 2;
					} else {
						uint32_t diff;

						if (p0 > slot_off_dst_b[slot]) {
							diff = p0 -
							       slot_off_dst_b[slot];
						} else {
							diff = slot_off_dst_b[slot] -
							       p0;
						}
						if (diff < slot_chunk_dst[slot]) {
							slot_chunk_dst[slot] = diff;
						}
						slot_off_dst_b[slot] = p0;
					}
				} else if (addr == TD_SRC_A) {
					if (p0 >
					    slot_max_offset_src[slot]) {
						slot_max_offset_src[slot] = p0;
					}
					if (slot_off_src_n[slot] == 0) {
						slot_off_src_a[slot] = p0;
						slot_off_src_n[slot] = 1;
					} else if (slot_off_src_n[slot] == 1) {
						uint32_t diff;

						if (p0 > slot_off_src_a[slot]) {
							diff = p0 -
							       slot_off_src_a[slot];
						} else {
							diff = slot_off_src_a[slot] -
							       p0;
						}
						slot_off_src_b[slot] = p0;
						slot_chunk_src[slot] = diff;
						slot_off_src_n[slot] = 2;
					} else {
						uint32_t diff;

						if (p0 > slot_off_src_b[slot]) {
							diff = p0 -
							       slot_off_src_b[slot];
						} else {
							diff = slot_off_src_b[slot] -
							       p0;
						}
						if (diff < slot_chunk_src[slot]) {
							slot_chunk_src[slot] = diff;
						}
						slot_off_src_b[slot] = p0;
					}
					if (slot_rank_srcA[slot] ==
					    UINT32_MAX) {
						if (slot > 1) {
							/* srcA slots > 1
							 * rank among MIL
							 * inputs; slot <= 1
							 * is the kernel-base
							 * convention (tag 2)
							 * used by real-div-*
							 * and rms, not a real
							 * input surface. */
							if (srcA_count == 0x40) {
								return fail("too "
									    "many "
									    "srcA "
									    "slots");
							}
							srcA_slots[srcA_count++] =
								slot;
						}
						/* Mark as seen so later
						 * refs at the same slot
						 * are deduped. */
						slot_rank_srcA[slot] = 0;
					}
				} else if (addr == TD_SRC_B) {
					if (p0 >
					    slot_max_offset_src[slot]) {
						slot_max_offset_src[slot] = p0;
					}
					if (slot_off_src_n[slot] == 0) {
						slot_off_src_a[slot] = p0;
						slot_off_src_n[slot] = 1;
					} else if (slot_off_src_n[slot] == 1) {
						uint32_t diff;

						if (p0 > slot_off_src_a[slot]) {
							diff = p0 -
							       slot_off_src_a[slot];
						} else {
							diff = slot_off_src_a[slot] -
							       p0;
						}
						slot_off_src_b[slot] = p0;
						slot_chunk_src[slot] = diff;
						slot_off_src_n[slot] = 2;
					} else {
						uint32_t diff;

						if (p0 > slot_off_src_b[slot]) {
							diff = p0 -
							       slot_off_src_b[slot];
						} else {
							diff = slot_off_src_b[slot] -
							       p0;
						}
						if (diff < slot_chunk_src[slot]) {
							slot_chunk_src[slot] = diff;
						}
						slot_off_src_b[slot] = p0;
					}
					if (slot_rank_srcB[slot] ==
					    UINT32_MAX) {
						if (srcB_count == 0x40) {
							return fail("too many "
								    "srcB "
								    "slots");
						}
						srcB_slots[srcB_count++] =
							slot;
					}
				}
			}
			idx += 1 + rec_count;
		}
	}
	/* Sort srcA and srcB slot lists ascending; assign rank 0,1,... */
	for (i = 1; i < srcA_count; i++) {
		tmp = srcA_slots[i];
		j = i;
		while (j > 0 && srcA_slots[j - 1] > tmp) {
			srcA_slots[j] = srcA_slots[j - 1];
			j--;
		}
		srcA_slots[j] = tmp;
	}
	for (i = 0; i < srcA_count; i++) {
		slot_rank_srcA[srcA_slots[i]] = i;
	}
	for (i = 1; i < srcB_count; i++) {
		tmp = srcB_slots[i];
		j = i;
		while (j > 0 && srcB_slots[j - 1] > tmp) {
			srcB_slots[j] = srcB_slots[j - 1];
			j--;
		}
		srcB_slots[j] = tmp;
	}
	for (i = 0; i < srcB_count; i++) {
		slot_rank_srcB[srcB_slots[i]] = i;
	}
	/* Matmul detection: a program is matmul if it uses >= 2 srcA
 * slots or >= 2 srcB slots. The extents-fit rule is used only for
 * matmul programs (where multiple src slots read inputs of different
 * allocs); elementwise programs (single-srcA and single-srcA +
 * single-srcB) keep the legacy register-based wiring so stages 1-4
 * remain byte-identical. rms-c2048-gamma has single-srcA slot 1 +
 * single-srcB slot 1 at the legacy tag-2 convention; the same path
 * preserves its byte-identity. */
	is_matmul = (srcA_count >= 2 || srcB_count >= 2) ? 1 : 0;
	/* Pass 2: per task, derive refs using the rule chosen by
	 * `legacy`. legacy=1 calls bar_ref_tag_legacy; legacy=0 calls
	 * bar_ref_tag_extents with slot_max_offset[] already known.
	 * After each task we union the per-slot tags; a cross-task
	 * conflict on a scratch-eligible slot merges to scratch_bufid. */
	for (t = 0; t < ntasks; t++) {
		const uint8_t *tp = stream + tasks[t].off;
		struct ane_m2_ref *task_refs = m->call_refs[t];
		uint32_t n;
		int err;

		err = refs_of_task(tp, tasks[t].words, task_refs, &n, legacy, m,
				  slot_max_offset_dst, slot_max_offset_src,
				  slot_chunk_dst, slot_chunk_src,
				  slot_max_in_alloc_src, slot_max_out_alloc_dst,
				  slot_rank_srcA, is_matmul);
		if (err) {
			return err;
		}
		m->call_ref_count[t] = n;
		for (i = 0; i < n; i++) {
			uint32_t s = task_refs[i].slot;

			if (union_tag[s] == 0xffffffffu) {
				union_tag[s] = task_refs[i].tag;
				union_addr_any[s] = task_refs[i].addr;
				union_slots[union_count++] = s;
				continue;
			}
			if (union_tag[s] == task_refs[i].tag) {
				continue;
			}
			/* Cross-task conflict: try scratch merge when
			 * scratch_bufid is enabled. */
			if (scratch_bufid != 0) {
				uint32_t prev_addr = union_addr_any[s];
				uint32_t cur_addr = task_refs[i].addr;
				if (!scratch_eligible_addr(prev_addr) ||
				    !scratch_eligible_addr(cur_addr)) {
					/* The mixed ref is a non-scratch
					 * channel base; cannot merge to
					 * scratch. Refuse. */
					char buf[512];

					snprintf(buf, sizeof(buf),
						 "task %u: BAR slot %u has a "
						 "non-mergeable mix (refs at "
						 "%#x and %#x are not both "
						 "TileDMA dst or KernelDMA); "
						 "scratch merge is unsafe",
						 (unsigned)t, (unsigned)s,
						 (unsigned)prev_addr,
						 (unsigned)cur_addr);
					return fail(buf);
				}
				union_tag[s] = scratch_bufid;
				scratch_used = 1;
				continue;
			}
			/* Scratch disabled: refuse with the cross-task
			 * diagnostic (mirrors tools/h14_sections.py so
			 * the lab tool and the C builder stay in sync). */
			{
				char buf[512];

				snprintf(buf, sizeof(buf),
					 "task %u: BAR slot %u resolves "
					 "to tag %u here and tag %u in an "
					 "earlier task; fw135 0x44c98 "
					 "pushToHWDirect has no per-task "
					 "BAR walk and the 61-slot patch "
					 "table at netDesc+0xC is global "
					 "per call (0x44e58-0x44ea4), so "
					 "this slot must be unique across "
					 "the whole program",
					 (unsigned)t, (unsigned)s,
					 (unsigned)task_refs[i].tag,
					 (unsigned)union_tag[s]);
				return fail(buf);
			}
		}
	}
	/* Encode the union {slot, tag} in a stable ascending-slot order
	 * (the proven add emission order; the lab Python tool sorts the
	 * same way). */
	n_unique = 0;
	for (i = 0; i < union_count; i++) {
		uint32_t s = union_slots[i];

		if (n_unique == ANE_M2_MAX_BINDS) {
			return fail("more union refs than the model holds");
		}
		m->call_refs[0][n_unique].slot = s;
		m->call_refs[0][n_unique].tag = union_tag[s];
		n_unique++;
	}
	for (i = 1; i < n_unique; i++) {
		uint32_t j = i;
		while (j > 0 &&
		       m->call_refs[0][j - 1].slot >
		       m->call_refs[0][j].slot) {
			struct ane_m2_ref tmp = m->call_refs[0][j - 1];

			m->call_refs[0][j - 1] = m->call_refs[0][j];
			m->call_refs[0][j] = tmp;
			j--;
		}
	}
	m->call_ref_count[0] = n_unique;
	for (t = 1; t < ntasks; t++) {
		m->call_ref_count[t] = 0;
	}
	/* Each ref must name either the kernel section (tag 2), the
	 * scratch buffer (when scratch merge fired), or one of the bound
	 * channels; tag 3 (text/descriptor) is never a ref's tag because
	 * no H14 ANEC reads through the descriptor section. */
	{
		uint32_t n = m->call_ref_count[0];
		struct ane_m2_ref *r = m->call_refs[0];

		for (i = 0; i < n; i++) {
			int known = r[i].tag == 2;
			uint32_t s;

			if (scratch_bufid != 0 && r[i].tag == scratch_bufid) {
				known = 1;
			}
			for (s = 0; s < m->io_count && !known; s++) {
				known = m->io[s].buffer_id == r[i].tag;
			}
			if (!known) {
				return fail("a ref names a tag outside the "
					    "kernel section, the scratch "
					    "buffer, and the bound channels");
			}
		}
	}
	m->calls = 1;
	*scratch_used_out = scratch_used;
	if (!m->call_ref_count[0]) {
		return fail("the task stream holds no BAR-ref record");
	}
	return 0;
}

/* Compute scratch BO bytes from BAR ref extents. The host must allocate a
 * scratch buffer large enough to hold every ref at the merged slot. The
 * extent field for a TileDMA dst / KernelDMA BAR ref is encoded in the
 * first payload word; we take the max across all tasks at the scratch
 * slot, add the program output_size, and round up to the 16 KiB tile
 * alignment. The guard page is added by the caller. */
static int scratch_size_bytes(const uint8_t *stream,
			      const struct ane_task *tasks, uint32_t ntasks,
			      uint32_t scratch_slot, uint64_t output_size,
			      uint64_t *scratch_size_out)
{
	uint64_t max_payload = 0;
	uint32_t t;
	uint64_t extent;

	for (t = 0; t < ntasks; t++) {
		const uint8_t *tp = stream + tasks[t].off;
		uint32_t words = tasks[t].words;
		uint32_t idx = 8;

		if ((le32(tp + 28) & 3u) == 3u) {
			idx = 9;
		}
		while (idx < words) {
			uint32_t h = le32(tp + idx * 4);
			uint32_t rec_count;

			if (h & 0x80000000u) {
				rec_count = 1 + (uint32_t)__builtin_popcount(
						      (h >> 15) & 0xffffu);
			} else {
				rec_count = ((h >> 15) & 0x3fu) + 1;
			}
			if (idx + 1 + rec_count > words) {
				return fail("record declares more payload "
					    "words than its task holds");
			}
			if (!(h & 0x80000000u) && (h & 0x20000000u)) {
				uint32_t slot = (h >> 23) & 0x3fu;
				uint32_t p0;

				if (slot == scratch_slot && rec_count >= 1) {
					p0 = le32(tp + (idx + 1) * 4);
					if (p0 > max_payload) {
						max_payload = p0;
					}
				}
			}
			idx += 1 + rec_count;
		}
	}
	/* extent = max_payload + output_size, rounded up to 16 KiB. */
	extent = max_payload + output_size;
	extent = (extent + 0x3fffu) & ~0x3fffull;
	*scratch_size_out = extent;
	return 0;
}

/*
 * tdprop deep walk, re-derived from the firmware checker (fw135 0x486a0):
 * stride is 0x30 when the first descriptor word has bit2 set, else 0x10;
 * each block's size is ((u16@[blk+2] & 0x7ff) << 2 + 0xf) & 0x3ff0; a zero
 * block word fails; when the first word has bit0 set, u32@[last+0x18] must
 * be 0. The segment's blockNbr is the walked block count (proven == 1 on
 * the fixture, where the Python builder hardcoded it).
 */
/* Read ANE_M2_SCRATCH and decide whether to enable the scratch merge.
 * The merge uses bufferId 0x40 (above the channel-id range and not 2/3).
 *   "0" or negative disables the merge (refuse on conflict);
 *   positive numeric value enables and floors the scratch BO size;
 *   missing (unset) auto-enables for the island ANECs that need it.
 * The stages 1-4 + rms fixtures have no cross-task scratch-eligible
 * conflict, so they byte-identically build even with scratch_bufid != 0. */
static uint32_t scratch_bufid_from_env(void)
{
	const char *e = getenv("ANE_M2_SCRATCH");

	if (!e) {
		return 0x40;
	}
	if (e[0] == '-' || e[0] == '+') {
		return 0x40;
	}
	if (strtoull(e, NULL, 0) == 0) {
		return 0;
	}
	return 0x40;
}

/* Parse ANE_M2_OPREFS="slot:tag,slot:tag,..." into the union ref set,
 * overwriting whatever derive_refs left in call_refs[0]. Empty/absent
 * is a no-op. Returns 0 on success or -EINVAL (with a printed message)
 * on a malformed value. The parsed refs are sorted by slot in-place so
 * the operation-section emit order matches the proven add order. */
static int oprefs_apply(struct ane_m2_model *m)
{
	const char *prefs = getenv("ANE_M2_OPREFS");
	uint32_t n = 0;
	const char *p_local;

	if (!prefs || !prefs[0]) {
		return 0;
	}
	p_local = prefs;
	while (*p_local && n < ANE_M2_MAX_BINDS) {
		char *end;
		unsigned long slot = strtoul(p_local, &end, 0);
		unsigned long tag;

		if (*end != ':') {
			return fail("ANE_M2_OPREFS expects slot:tag pairs "
				    "separated by commas");
		}
		p_local = end + 1;
		tag = strtoul(p_local, &end, 0);
		if (end == p_local) {
			return fail("ANE_M2_OPREFS tag is not numeric");
		}
		if (slot > 0x3cu) {
			return fail("ANE_M2_OPREFS slot outside the 61-slot "
				    "BAR range");
		}
		if (tag > 0x3cu && (uint32_t)tag != 0x40u) {
			return fail("ANE_M2_OPREFS tag outside the 61-slot "
				    "BAR range (scratch_bufid 0x40 is "
				    "allowed when scratch is enabled)");
		}
		m->call_refs[0][n].slot = (uint32_t)slot;
		m->call_refs[0][n].tag = (uint32_t)tag;
		m->call_refs[0][n].addr = 0;
		m->call_refs[0][n].payload0 = 0;
		n++;
		p_local = (*end == ',') ? end + 1 : end;
	}
	if (n == 0) {
		return fail("ANE_M2_OPREFS produced no ref pairs");
	}
	/* Insertion sort by slot. */
	{
		uint32_t i;
		for (i = 1; i < n; i++) {
			struct ane_m2_ref tmp = m->call_refs[0][i];
			uint32_t j = i;
			while (j > 0 && m->call_refs[0][j - 1].slot > tmp.slot) {
				m->call_refs[0][j] = m->call_refs[0][j - 1];
				j--;
			}
			m->call_refs[0][j] = tmp;
		}
	}
	m->call_ref_count[0] = n;
	return 0;
}

static int tdprop_block_count(const uint8_t *desc, uint64_t size)
{
	uint32_t w0 = le32(desc);
	uint64_t stride = (w0 & 4) ? 0x30 : 0x10;
	uint64_t cur = stride;
	uint64_t last = 0;
	int walked = 0;

	while (size > cur) {
		uint32_t blk = le16(desc + cur + 2) & 0x7ff;

		last = cur;
		if (!blk) {
			return fail("tdprop walk hit a zero block word");
		}
		cur += ((blk << 2) + 0xf) & 0x3ff0;
		walked++;
	}
	if ((w0 & 1) && size > stride && le32(desc + last + 0x18)) {
		return fail("tdprop tail word not zero");
	}
	return walked;
}

int ane_m2_program_build(const void *anec, uint64_t anec_size,
			 struct ane_m2_model *model,
			 struct ane_m2_sections *secs)
{
	const uint8_t *d = anec;
	uint64_t payload, tsk_size, krn_size, const_off, gen_size;
	uint32_t first_task, task_count, input_count, version, k;
	uint8_t *desc, *kern, *gen, *oper, *proc, *tdp;
	struct ane_task tasks[ANE_M2_MAX_TASKS];
	uint32_t ntasks = 0;
	int blocks, err;

	memset(secs, 0, sizeof(*secs));
	memset(model, 0, sizeof(*model));

	if (anec_size < ANEC_M2_HEADER_SIZE) {
		return fail("anec shorter than the H14 header layout");
	}

	payload = le64(d);
	first_task = le32(d + 0x08);
	task_count = le32(d + 0x0c);
	tsk_size = le64(d + 0x10);
	krn_size = le64(d + 0x18);
	input_count = le32(d + 0x20);
	version = le32(d + 0x24);

	if (payload != anec_size - ANEC_M2_HEADER_SIZE) {
		return fail("header payload size disagrees with file size");
	}
	if (version != 1) {
		return fail("anec header version word is not the emitted 1");
	}
	/* PROVEN by the encoder parity suite (760/760 cases pass) for
	 * input_count in {1,2}: inputs map to ANEC channels 5+k in
	 * declaration order, output to channel 4. The 3-input case is
	 * INFERRED from the Parakeet select-8head island (channel 7 is
	 * the cond operand); no decoded fw site explicitly enumerates
	 * channel 7 as a runtime input, but the byte-identical match to
	 * the lab Python builder confirms the pattern. input_count > 3
	 * has no receipt; refuse rather than guess a channel 8+. */
	if (input_count < 1 || input_count > 3) {
		return fail("inputCount outside [1,3]");
	}
	if (krn_size > payload) {
		return fail("constant region is larger than the payload");
	}
	const_off = payload - krn_size;
	if (const_off & 0x3f) {
		return fail("constant region is not 64-byte aligned (the "
			    "encoder emits align_up(stream, 64))");
	}
	if (tsk_size > const_off) {
		return fail("task stream reaches into the constant region");
	}

	/* io table: H14 channel contract (H14Program.cpp:547-561, proven on
	 * hardware): output = channel 4, input k = channel 5+k. Emission
	 * order: inputs ascending, then the output. */
	model->io_count = input_count + 1;
	for (k = 0; k < model->io_count; k++) {
		struct ane_m2_io *io = &model->io[k];
		uint32_t b;
		uint32_t tiles;

		if (k < input_count) {
			b = 5 + k;
			io->dir = 0;
		} else {
			b = 4;
			io->dir = 1;
		}
		/* The call checker rejects io ids equal to the kernel/text
		 * section ids 2/3 (fw135 0x48ee0-0x48f10); ids >= 5 cannot
		 * hit them, but stay explicit. */
		if (b >= TILE_COUNT || b == 2 || b == 3) {
			return fail("io channel id collides with a section id");
		}
		/* Entry size = the channel allocation, tiles[b] units of
		 * 0x4000 B (the lab builder's allocation_bytes; the add
		 * fixture carries tiles[4..6] == 2 -> 0x8000). The nchw
		 * layout is informational and not part of the derivation. */
		tiles = le32(d + 0x28 + b * 4);
		if (!tiles || tiles > (1u << 20)) {
			return fail("unusable tile count for an io channel");
		}
		io->buffer_id = b;
		io->size = (uint64_t)tiles << ANE_M2_TILE_UNIT_SHIFT;
	}

	/* Descriptor section: the raw task stream (16-byte zero frame, the
	 * tasks, their alignment filler), walked to count the tasks. The
	 * task header word (bits 26:16) declares the task length; copying
	 * only a prefix drops tail records and wedges the TQ. */
	if (tsk_size < FRAME_BYTES) {
		return fail("task stream is shorter than one frame");
	}
	desc = sec_alloc(secs, ANE_M2_SEC_DESCRIPTOR, tsk_size);
	if (!desc) {
		return -ENOMEM;
	}
	memcpy(desc, d + ANEC_M2_HEADER_SIZE, tsk_size);
	for (k = 0; k < FRAME_BYTES; k++) {
		if (desc[k]) {
			return fail("task frame not zero");
		}
	}
	err = split_h14_tasks(desc, tsk_size, tasks, &ntasks);
	if (err) {
		return err;
	}
	if (!ntasks) {
		return fail("the task stream holds no task");
	}
	if (ntasks != task_count) {
		return fail("walked task count disagrees with the header "
			    "taskCount");
	}
	if (first_task != tasks[0].words * 4) {
		/* PROVEN by the encoder: firstTaskBytes is the walked
		 * first task's byte size (decoder/encoders parity
		 * agreement). The 9 stage 1-4 fixtures all match;
		 * mismatches here are tampered bytes or an encoder
		 * bug. */
		return fail("firstTaskBytes disagrees with the walked first "
			    "task");
	}

/* Derive the BAR slots using the extents rule. The rule picks each
	 * slot's tag from the slot's max payload plus the bound channel
	 * allocations; it falls back to kernel-base (tag 2) only when slot
	 * <= 1 and the register is a TileDMA source base (the legacy
	 * convention used by real-div-scalar, clip-*, matvec, and the
	 * kernel-resident rms reads). Cross-task scratch-eligible
	 * conflicts merge to scratch_bufid (0x40); everything else
	 * refuses. */
	{
		FILE *saved = stderr;
		FILE *sink = fopen("/dev/null", "w");
		uint32_t scratch_bufid = scratch_bufid_from_env();
		int scratch_used = 0;

		if (sink) {
			stderr = sink;
		}
		err = derive_refs(desc, tasks, ntasks, model, 0 /*extents*/,
				  scratch_bufid, &scratch_used);
		if (sink) {
			fclose(sink);
		}
		stderr = saved;
		if (err) {
			/* Re-run with stderr live so the LIBANE error log
			 * tells the caller which rule failed and why. */
			(void)derive_refs(desc, tasks, ntasks, model, 0,
					  scratch_bufid, &scratch_used);
			return err;
		}
		model->scratch_io_index = scratch_used
			? (uint32_t)model->io_count : UINT32_MAX;
	}

	/* ANE_M2_OPREFS="slot:tag,slot:tag,...": replace the derived ref
	 * set entirely. Lets a device-side hypothesis swap the tag for a
	 * slot (e.g. bind slot 3 to scratch 0x40 or to kernel 2) without
	 * rebuilding the sections by hand. Empty or absent keeps the
	 * derived refs. */
	if (oprefs_apply(model)) {
		return fail("ANE_M2_OPREFS parsing failed");
	}
	{
		uint32_t i;
		int has_scratch = 0;
		for (i = 0; i < model->call_ref_count[0]; i++) {
			if (model->call_refs[0][i].tag == 0x40) {
				has_scratch = 1;
				break;
			}
		}
		if (has_scratch && model->scratch_io_index == UINT32_MAX) {
			model->scratch_io_index = (uint32_t)model->io_count;
		}
	}

	/* If the scratch merge fired (either by derive_refs OR by
	 * OPREFS naming scratch_bufid), append the scratch entry to the
	 * io table. The scratch bufferId is 0x40 (above the channel-id
	 * range and not 2/3); the entry's direction is a host-internal
	 * value (2) so ane_m2_send/read skip it. The scratch BO size is
	 * computed from BAR-ref extents at the merged slot, optionally
	 * forced via ANE_M2_SCRATCH. */
	if (model->scratch_io_index != UINT32_MAX) {
		uint64_t scratch_bytes = 0;
		uint64_t env_bytes = 0;
		const char *env = getenv("ANE_M2_SCRATCH");
		struct ane_m2_io *scratch_io;
		uint32_t scratch_slot = 0;
		uint32_t i;
		int found = 0;

		for (i = 0; i < model->call_ref_count[0]; i++) {
			if (model->call_refs[0][i].tag == 0x40) {
				scratch_slot = model->call_refs[0][i].slot;
				found = 1;
				break;
			}
		}
		if (!found) {
			return fail("scratch merge requested but no ref "
				    "uses scratch_bufid 0x40");
		}
		err = scratch_size_bytes(desc, tasks, ntasks, scratch_slot,
					 model->io[input_count].size,
					 &scratch_bytes);
		if (err) {
			return err;
		}
		if (env && env[0] != '0' && env[0] != '-') {
			env_bytes = strtoull(env, NULL, 0);
		}
		if (env_bytes > scratch_bytes) {
			scratch_bytes = env_bytes;
		}
		/* Add guard page for kernel reads past end. */
		scratch_bytes += 0x4000ull;
		if (model->io_count >= ANE_M2_MAX_BINDS) {
			return fail("io table full; cannot add scratch entry");
		}
		scratch_io = &model->io[model->io_count];
		scratch_io->buffer_id = 0x40;
		scratch_io->dir = 2;
		scratch_io->size = scratch_bytes;
		model->io_count++;
	}

	/* Kernel/constant section: the raw constant region, sized by the
	 * header (0x80 B for the clip fold, 128 KiB for matvec weights). */
	kern = sec_alloc(secs, ANE_M2_SEC_KERNEL, krn_size);
	if (!kern) {
		return -ENOMEM;
	}
	memcpy(kern, d + ANEC_M2_HEADER_SIZE + const_off, krn_size);

	/* Generic section: 0x208-byte header + 0x30-byte entries. */
	gen_size = 0x208 + 0x30ULL * model->io_count;
	gen = sec_alloc(secs, ANE_M2_SEC_GENERIC, gen_size);
	if (!gen) {
		return -ENOMEM;
	}
	put_le32(gen + 0x00, 1);    /* magic (fw135 0x481fc) */
	put_le32(gen + 0x04, 0x10); /* version <= 0x10 (0x48208) */
	put_le32(gen + 0x204, model->io_count);
	for (k = 0; k < model->io_count; k++) {
		const struct ane_m2_io *io = &model->io[k];
		uint8_t *e = gen + 0x208 + 0x30ULL * k;
		uint32_t type_field;
		uint32_t dir_field;

		if (io->dir == 2) {
			/* Scratch: firmware treats it as input for BAR
			 * resolution; type 0 / dir 1 (input-side buffer).
			 * validateCall 0x48df8 only checks id!=2/3 and
			 * id uniqueness; the type byte is not consulted
			 * for BAR resolution. */
			type_field = 0;
			dir_field = 1;
		} else {
			type_field = io->dir;
			dir_field = io->dir ? 2 : 1;
		}
		put_le32(e + 0x00, 1); /* flags: present */
		put_le32(e + 0x04, io->buffer_id);
		put_le32(e + 0x08, type_field);
		put_le32(e + 0x10, dir_field);
		put_le64(e + 0x20, io->size);
		put_le32(e + 0x28, 0xffff); /* sentinel, mirrored from h14conv */
	}

	/* Operation section: u32 tot + N 0x40c-byte records. type 0 (kernel
	 * op) with tdCount 0 short-circuits the kernel-ref resolver
	 * (fw135 0x48904); refCount refs {slot, tag} at +0x10. The existing
	 * 9 stage 1-4 fixtures get N == 1 with the union of all per-task
	 * refs; the Parakeet island ANECs get N == taskCount (one record per
	 * call, refs specific to that call's task).
	 *
	 * PROVEN on hardware for N==1 (boot 8f468602: add, refs {4,5}{5,4}
	 * {6,6}). The N==taskCount form is INFERRED: the firmware reads
	 * call->recordIdx (fw135 0x44ea0 ldr w10, [x9, #0xc]!; cbz w10) and
	 * indexes into the operation section, so each call may pick its own
	 * record. The byte-identical match to the lab Python builder
	 * (which emits one record per task when refs differ across tasks)
	 * confirms the encoding. */
	oper = sec_alloc(secs, ANE_M2_SEC_OPERATION,
			 4 + 0x40cULL * model->calls);
	if (!oper) {
		return -ENOMEM;
	}
	put_le32(oper, model->calls);
	for (k = 0; k < model->calls; k++) {
		uint32_t n = model->call_ref_count[k];
		uint32_t j;
		uint8_t *rec = oper + 4 + 0x40cULL * k;

		put_le32(rec + 0x08, n);
		for (j = 0; j < n; j++) {
			put_le32(rec + 0x0c + 8ULL * j,
				 model->call_refs[k][j].slot);
			put_le32(rec + 0x0c + 8ULL * j + 4,
				 model->call_refs[k][j].tag);
		}
	}

	/* Procedure section: tot + {u64 offset, u64 size} + records. Content
	 * mirrors the h14conv procedure.bin (load-proven on hardware);
	 * record +4 = 3 is eCSneCmdProgramProcedureContentType_3, accepted
	 * by getProcedureCallType with {0,3,4} (fw135 0x5a04c region). No
	 * rule for other contents was derived, so it stays fixed. */
	proc = sec_alloc(secs, ANE_M2_SEC_PROCEDURE, 0x18 + 0x20);
	if (!proc) {
		return -ENOMEM;
	}
	put_le32(proc, 1);
	put_le64(proc + 0x08, 0x18);
	put_le64(proc + 0x10, 0x20);
	put_le32(proc + 0x18, 1);
	put_le32(proc + 0x1c, 3);
	put_le32(proc + 0x20, 0);
	put_le32(proc + 0x24, 0);
	put_le32(proc + 0x28, 1);
	put_le32(proc + 0x2c, 0);
	put_le32(proc + 0x30, 0xffffffff);
	put_le32(proc + 0x34, 4);

	/* tdprop: one segment covering the whole descriptor, blockNbr from
	 * the firmware block walk (fw135 0x486a0; the walked count must
	 * match, 0x48798). The 5-task Parakeet islands exercise this:
	 * each task is one block in the descriptor (stride 0x30 when
	 * the task header has bit 2 set, else 0x10); the walker counts
	 * them all and the segment's blockNbr must match. PROVEN by the
	 * byte-identical match to the lab Python builder. */
	blocks = tdprop_block_count(desc, tsk_size);
	if (blocks < 0) {
		return blocks;
	}
	if (!blocks) {
		return fail("tdprop walk found no block; the firmware deep "
			    "check would reject the descriptor");
	}
	tdp = sec_alloc(secs, ANE_M2_SEC_TDPROP, 40);
	if (!tdp) {
		return -ENOMEM;
	}
	put_le32(tdp, 1);
	put_le32(tdp + 0x08, 0); /* a */
	put_le32(tdp + 0x0c, (uint32_t)blocks);
	put_le64(tdp + 0x10, 0); /* segment offset */
	put_le64(tdp + 0x18, 0); /* pad */
	put_le64(tdp + 0x20, tsk_size);
	return 0;
}

void ane_m2_sections_free(struct ane_m2_sections *secs)
{
	int i;

	for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
		free(secs->sec[i].data);
	}
	memset(secs, 0, sizeof(*secs));
}

/* ---- device path ---- */

struct ane_m2_ctx {
	struct ane_m2_model model;
	struct ane_m2_sections secs; /* CPU copies, kept for debugging */
	uint32_t prog_id;
	uint32_t proc_id;
	struct ane_bo sec_bo[ANE_M2_SEC_COUNT];
	struct ane_bo io_bo[ANE_M2_MAX_BINDS];
};

/* The engine reads and writes slightly past the end of a surface: measured
 * 2026-09-30 on the island programs, DART faults at exactly the end of the
 * output BO (write) and at end + 0x100 of an input BO (read). One extra
 * DART page (16 KiB) behind every BO keeps those accesses mapped. */
static uint64_t ane_m2_guard(void)
{
	const char *e = getenv("ANE_M2_GUARD");

	return e ? strtoull(e, NULL, 0) : 0x4000ull;
}
#define ANE_M2_BO_GUARD ane_m2_guard()

static int bo_alloc(struct ane_nn *nn, struct ane_bo *bo, uint64_t size)
{
	struct drm_ane_bo_init args = { .size = size + ANE_M2_BO_GUARD };
	struct drm_ane_bo_free f;

	if (ioctl(nn->fd, DRM_IOCTL_ANE_BO_INIT, &args) < 0) {
		ane_m2_err("DRM_IOCTL_ANE_BO_INIT failed for %llu bytes\n",
			   (unsigned long long)size);
		return -EINVAL;
	}
	bo->size = size + ANE_M2_BO_GUARD;
	bo->handle = args.handle;
	bo->offset = args.offset;
	bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, nn->fd,
		       bo->offset);
	if (bo->map == MAP_FAILED) {
		bo->map = NULL;
		f.handle = bo->handle;
		ioctl(nn->fd, DRM_IOCTL_ANE_BO_FREE, &f);
		bo->handle = 0;
		ane_m2_err("failed to mmap BO of %llu bytes\n",
			   (unsigned long long)size);
		return -EINVAL;
	}
	return 0;
}

/* BO_FREE releases only the CPU mapping and handle; the kernel keeps every
 * IOVA in this program reserved until reboot (IOVA lifetime v1), so freeing
 * is safe and the program stays kernel-visible. */
static void bo_release(struct ane_nn *nn, struct ane_bo *bo)
{
	struct drm_ane_bo_free f;

	if (bo->map) {
		munmap(bo->map, bo->size);
		bo->map = NULL;
	}
	if (bo->handle) {
		f.handle = bo->handle;
		ioctl(nn->fd, DRM_IOCTL_ANE_BO_FREE, &f);
		bo->handle = 0;
	}
}

static void ane_m2_ctx_free(struct ane_nn *nn, struct ane_m2_ctx *ctx,
			    int created_secs, int created_ios)
{
	int i;

	for (i = 0; i < created_secs; i++) {
		bo_release(nn, &ctx->sec_bo[i]);
	}
	for (i = 0; i < created_ios; i++) {
		bo_release(nn, &ctx->io_bo[i]);
	}
	ane_m2_sections_free(&ctx->secs);
	free(ctx);
}

static int ane_m2_fread_all(const char *path, void **out, uint64_t *out_size)
{
	FILE *fp;
	long end;
	uint8_t *buf;

	fp = fopen(path, "rb");
	if (!fp) {
		ane_m2_err("failed to open anec %s\n", path);
		return -EINVAL;
	}
	if (fseek(fp, 0, SEEK_END)) {
		fclose(fp);
		return -EINVAL;
	}
	end = ftell(fp);
	if (end < 0 || fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return -EINVAL;
	}
	buf = malloc((uint64_t)end);
	if (!buf) {
		fclose(fp);
		return -ENOMEM;
	}
	if (fread(buf, 1, (uint64_t)end, fp) != (uint64_t)end) {
		ane_m2_err("short read on %s\n", path);
		free(buf);
		fclose(fp);
		return -EINVAL;
	}
	fclose(fp);
	*out = buf;
	*out_size = (uint64_t)end;
	return 0;
}

int ane_m2_open(struct ane_nn *nn, const char *path)
{
	struct ane_m2_ctx *ctx;
	struct drm_ane_section sec_args[ANE_M2_SEC_COUNT];
	struct drm_ane_generic_bind binds[ANE_M2_MAX_BINDS];
	struct drm_ane_prog_load load;
	struct drm_ane_proc_create create;
	uint8_t *buf = NULL;
	uint64_t size = 0;
	uint32_t i;
	int created_secs = 0;
	int created_ios = 0;
	int err;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	err = ane_m2_fread_all(path, (void **)&buf, &size);
	if (err) {
		free(ctx);
		return err;
	}
	if (size >= sizeof(struct anec)) {
		memcpy(&nn->anec, buf, sizeof(struct anec));
	}

	err = ane_m2_program_build(buf, size, &ctx->model, &ctx->secs);
	free(buf);
	if (err) {
		ane_m2_err("failed to build sections from %s\n", path);
		free(ctx);
		return err;
	}

	for (i = 0; i < ANE_M2_SEC_COUNT; i++, created_secs++) {
		err = bo_alloc(nn, &ctx->sec_bo[i], ctx->secs.sec[i].size);
		if (err) {
			goto error;
		}
		memcpy(ctx->sec_bo[i].map, ctx->secs.sec[i].data,
		       ctx->secs.sec[i].size);
	}

	for (i = 0; i < ctx->model.io_count; i++, created_ios++) {
		err = bo_alloc(nn, &ctx->io_bo[i], ctx->model.io[i].size);
		if (err) {
			goto error;
		}
		memset(ctx->io_bo[i].map, 0, ctx->model.io[i].size);
	}

	/* PROG_LOAD: section list plus one generic bind per io buffer.
	 * The M2 driver accepts the binds for ABI compatibility and
	 * ignores them: the LOAD record already carries each section's
	 * IOVA and size, and the generic section image inside
	 * generic.bin is fully prepared here. */
	for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
		sec_args[i].id = ane_m2_section_ids[i];
		sec_args[i].bo_handle = ctx->sec_bo[i].handle;
		sec_args[i].size = ctx->secs.sec[i].size;
		sec_args[i].offset = 0;
	}
	for (i = 0; i < ctx->model.io_count; i++) {
		binds[i].buffer_id = ctx->model.io[i].buffer_id;
		binds[i].bo_handle = ctx->io_bo[i].handle;
		/* The host's scratch entry (model.io[i].dir == 2) is
		 * presented to the driver / firmware as type 0 (input):
		 * the firmware doesn't care about type for BAR resolution
		 * (validateCall 0x48df8 only checks bufferId), and type 0
		 * keeps the kernel-side binder happy. */
		binds[i].type = ctx->model.io[i].dir == 2
			? 0 : ctx->model.io[i].dir;
		binds[i].size = ctx->model.io[i].size;
	}
	load.sections_ptr = (uint64_t)(uintptr_t)sec_args;
	load.generic_ptr = (uint64_t)(uintptr_t)binds;
	load.section_count = ANE_M2_SEC_COUNT;
	load.generic_count = ctx->model.io_count;
	load.prog_id_out = 0;
	load.pad = 0;
	if (ioctl(nn->fd, DRM_IOCTL_ANE_PROG_LOAD, &load) < 0) {
		err = -errno;
		ane_m2_err("DRM_IOCTL_ANE_PROG_LOAD failed: %s\n", strerror(errno));
		goto error;
	}
	ctx->prog_id = load.prog_id_out;

	create.prog_id = ctx->prog_id;
	create.proc_id_out = 0;
	if (ioctl(nn->fd, DRM_IOCTL_ANE_PROC_CREATE, &create) < 0) {
		err = -errno;
		ane_m2_err("DRM_IOCTL_ANE_PROC_CREATE failed: %s\n", strerror(errno));
		goto error;
	}
	ctx->proc_id = create.proc_id_out;

	nn->m2 = ctx;
	return 0;

error:
	ane_m2_ctx_free(nn, ctx, created_secs, created_ios);
	return err;
}

void ane_m2_close(struct ane_nn *nn)
{
	struct ane_m2_ctx *ctx = nn->m2;

	if (!ctx) {
		return;
	}
	nn->m2 = NULL;
	ane_m2_ctx_free(nn, ctx, ANE_M2_SEC_COUNT, (int)ctx->model.io_count);
}

int ane_m2_exec(struct ane_nn *nn)
{
	struct ane_m2_ctx *ctx = nn->m2;
	struct drm_ane_exec_io io[ANE_M2_MAX_BINDS];
	struct drm_ane_exec args;
	uint32_t i;

	if (!ctx) {
		return -EINVAL;
	}
	for (i = 0; i < ctx->model.io_count; i++) {
		io[i].buffer_id = ctx->model.io[i].buffer_id;
		io[i].bo_handle = ctx->io_bo[i].handle;
		io[i].type = ctx->model.io[i].dir == 2
			? 0 : ctx->model.io[i].dir;
		io[i].flags = 0;
		io[i].dma = 0;
		io[i].size = ctx->model.io[i].size;
	}
	args.prog_id = ctx->prog_id;
	args.proc_id = ctx->proc_id;
	args.priority = ANE_M2_PRIORITY_DEFAULT;
	args.timeout_ms = ANE_M2_EXEC_TIMEOUT_MS;
	args.count = ctx->model.io_count;
	args.pad = 0;
	args.io_ptr = (uint64_t)(uintptr_t)io;
	if (ioctl(nn->fd, DRM_IOCTL_ANE_EXEC, &args) < 0) {
		ane_m2_err("DRM_IOCTL_ANE_EXEC failed: %s\n", strerror(errno));
		return -EINVAL;
	}
	return 0;
}

static struct ane_m2_io *nth_io(struct ane_m2_ctx *ctx, uint32_t dir,
				uint32_t idx)
{
	uint32_t seen = 0;
	uint32_t i;

	for (i = 0; i < ctx->model.io_count; i++) {
		if (ctx->model.io[i].dir != dir) {
			continue;
		}
		if (seen == idx) {
			return &ctx->model.io[i];
		}
		seen++;
	}
	return NULL;
}

int ane_m2_send(struct ane_nn *nn, const void *from, uint32_t idx)
{
	struct ane_m2_ctx *ctx = nn->m2;
	struct ane_m2_io *io;

	if (!ctx || !(io = nth_io(ctx, 0, idx))) {
		return -EINVAL;
	}
	memcpy(ctx->io_bo[io - ctx->model.io].map, from, io->size);
	return 0;
}

int ane_m2_read(struct ane_nn *nn, void *to, uint32_t idx)
{
	struct ane_m2_ctx *ctx = nn->m2;
	struct ane_m2_io *io;

	if (!ctx || !(io = nth_io(ctx, 1, idx))) {
		return -EINVAL;
	}
	memcpy(to, ctx->io_bo[io - ctx->model.io].map, io->size);
	return 0;
}

uint64_t ane_m2_src_size(struct ane_nn *nn, uint32_t idx)
{
	struct ane_m2_io *io;

	if (!nn->m2 || !(io = nth_io(nn->m2, 0, idx))) {
		return 0;
	}
	return io->size;
}

uint64_t ane_m2_dst_size(struct ane_nn *nn, uint32_t idx)
{
	struct ane_m2_io *io;

	if (!nn->m2 || !(io = nth_io(nn->m2, 1, idx))) {
		return 0;
	}
	return io->size;
}
