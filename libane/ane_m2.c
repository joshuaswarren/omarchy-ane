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
static int bar_ref_tag(uint32_t addr, uint32_t slot, uint32_t *tag)
{
	(void)slot;
	if (addr >= TD_KDMA_LO && addr < TD_KDMA_HI) {
		*tag = 2;
		return 0;
	}
	if (addr == TD_DST) {
		*tag = 4;
		return 0;
	}
	if (addr == TD_SRC_A || addr == TD_SRC_B) {
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
 * `legacy` selects between the legacy slot-based rule (true; preserves
 * the 9 stage 1-4 fixture bytes) and the base-register rule (false; what
 * fw135 actually loads and what the Parakeet islands need). */
static int refs_of_task(const uint8_t *tp, uint32_t words,
			struct ane_m2_ref *out, uint32_t *count, int legacy)
{
	uint32_t idx = 8;
	uint32_t n = 0;
	uint32_t i;

	if ((le32(tp + 28) & 3u) == 3u) {
		idx = 9; /* an extra header word precedes the records */
	}
	while (idx < words) {
		uint32_t h = le32(tp + idx * 4);
		uint32_t rec_count;
		uint32_t slot;
		uint32_t tag;
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
				/* INFERRED refusal: no decoded fw site
				 * distinguishes the slot field width when
				 * bit 28 is set. The eight-decoded case
				 * always carries bit 28 clear; the
				 * bit-28-set form would extend the slot
				 * field to 7 bits and is not exercised by
				 * any H14 mint receipt. Refuse rather
				 * than guess a 7-bit slot. */
				return fail("BAR-ref header has bit 28 set; "
					    "the slot field width (28:23 vs "
					    "27:23) is not decided by any "
					    "decoded case");
			}
			slot = (h >> 23) & 0x3fu;
			err = (legacy ? bar_ref_tag_legacy(
						(h & 0x7fffu) * 4, slot, &tag)
				      : bar_ref_tag((h & 0x7fffu) * 4, slot,
						    &tag));
			if (err) {
				return err;
			}
			/* Dedup by (slot, tag) within this task. */
			for (i = 0; i < n; i++) {
				if (out[i].slot == slot && out[i].tag == tag) {
					break;
				}
			}
			if (i == n) {
				if (n == ANE_M2_MAX_BINDS) {
					return fail("more refs in one task "
						    "than the model holds");
				}
				out[n].slot = slot;
				out[n].tag = tag;
				n++;
			}
		}
		idx += 1 + rec_count;
	}
	*count = n;
	return 0;
}

/* Walk every record of every task and write the per-call ref set into
 * `model`. The existing 9 stage 1-4 fixtures are preserved byte-identical
 * because their per-task refs are consistent (no slot→tag cross-task
 * conflict) and we fall back to the single-record model in that case.
 * Multi-call programs (the Parakeet islands) emit one record per task. */
static int derive_refs(const uint8_t *stream, const struct ane_task *tasks,
		       uint32_t ntasks, struct ane_m2_model *m, int legacy)
{
	uint32_t union_tag[0x40];
	uint32_t union_slots[0x40];
	uint32_t union_count = 0;
	uint32_t n_unique;
	uint32_t calls;
	uint32_t t;
	uint32_t i;

	for (i = 0; i < 0x40; i++) {
		union_tag[i] = 0xffffffffu;
	}
	calls = 0;
	for (t = 0; t < ntasks; t++) {
		const uint8_t *tp = stream + tasks[t].off;
		struct ane_m2_ref *task_refs = m->call_refs[t];
		uint32_t n;
		int err;

		err = refs_of_task(tp, tasks[t].words, task_refs, &n, legacy);
		if (err) {
			return err;
		}
		m->call_ref_count[t] = n;
		for (i = 0; i < n; i++) {
			uint32_t s = task_refs[i].slot;

			if (union_tag[s] == 0xffffffffu) {
				union_tag[s] = task_refs[i].tag;
				union_slots[union_count++] = s;
			} else if (union_tag[s] != task_refs[i].tag) {
				/* Cross-task conflict: caller must emit
				 * one record per task. */
				calls = ntasks;
			}
		}
	}
	/* Consistent: caller emits the single-record model. */
	if (!calls) {
		calls = 1;
	}
	if (calls > ANE_M2_MAX_CALLS) {
		return fail("more calls than the model holds");
	}
	/* Encode the union {slot, tag} in a stable ascending-slot order
	 * when the single-record model applies. */
	n_unique = 0;
	if (calls == 1) {
		for (i = 0; i < union_count; i++) {
			uint32_t s = union_slots[i];

			if (n_unique == ANE_M2_MAX_BINDS) {
				return fail("more union refs than the model "
					    "holds");
			}
			m->call_refs[0][n_unique].slot = s;
			m->call_refs[0][n_unique].tag = union_tag[s];
			n_unique++;
		}
		/* Sort ascending by slot (the proven add emission order). */
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
	}
	/* Each ref must name either the kernel section (tag 2) or one of
	 * the bound channels; tag 3 (text/descriptor) is never a ref's tag
	 * because no island ANEC reads through the descriptor section. */
	for (t = 0; t < calls; t++) {
		uint32_t n = m->call_ref_count[t];
		struct ane_m2_ref *r = m->call_refs[t];

		for (i = 0; i < n; i++) {
			int known = r[i].tag == 2;
			uint32_t s;

			for (s = 0; s < m->io_count && !known; s++) {
				known = m->io[s].buffer_id == r[i].tag;
			}
			if (!known) {
				return fail("a ref names a tag outside the "
					    "kernel section and the bound "
					    "channels");
			}
		}
	}
	m->calls = calls;
	if (!calls) {
		return fail("the task stream holds no BAR-ref record");
	}
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

	/* Try the legacy rule first; the 9 stage 1-4 fixtures need it. The
	 * base-register rule below is what the Parakeet islands need. If
	 * the legacy attempt fails, we reset model and retry with the base
	 * rule. Both attempts print the LIBANE error log to stderr unless
	 * we redirect it: the legacy attempt's failure is expected and not
	 * a bug for the islands, so we silence both attempts and only
	 * surface a final failure. */
	{
		FILE *saved = stderr;
		FILE *sink = fopen("/dev/null", "w");

		if (sink) {
			stderr = sink;
		}
		err = derive_refs(desc, tasks, ntasks, model, 1 /*legacy*/);
		if (!err) {
			if (sink) {
				fclose(sink);
			}
			stderr = saved;
			goto refs_done;
		}
		/* The legacy rule refuses BAR slot 2/3 at a TileDMA source
		 * base and forces tag 2 for slot <= 1. The island ANECs
		 * need the base-register rule: tag is decided purely by
		 * the patched register address. Reset model and rebuild
		 * the io table. The 9 stage 1-4 fixtures don't hit the
		 * legacy refusal, so they keep their bytes. */
		memset(model, 0, sizeof(*model));
		/* io table was zeroed too; rebuild it. */
		model->io_count = input_count + 1;
		err = 0;
		for (k = 0; k < model->io_count && !err; k++) {
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
			/* The call checker rejects io ids equal to the
			 * kernel/text section ids 2/3 (fw135 0x48ee0-
			 * 0x48f10); ids >= 5 cannot hit them, but stay
			 * explicit. */
			if (b >= TILE_COUNT || b == 2 || b == 3) {
				err = fail("io channel id collides with a "
					   "section id");
				break;
			}
			tiles = le32(d + 0x28 + b * 4);
			if (!tiles || tiles > (1u << 20)) {
				err = fail("unusable tile count for an io "
					   "channel");
				break;
			}
			io->buffer_id = b;
			io->size = (uint64_t)tiles <<
				   ANE_M2_TILE_UNIT_SHIFT;
		}
		if (!err) {
			err = derive_refs(desc, tasks, ntasks, model,
					  0 /*base*/);
		}
		if (sink) {
			fclose(sink);
		}
		stderr = saved;
		/* If the base rule also failed, re-run the failing path
		 * with stderr live so the LIBANE error log explains why. */
		if (err) {
			(void)derive_refs(desc, tasks, ntasks, model, 0);
		}
	}
refs_done:
	;

	if (err) {
		return err;
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

		put_le32(e + 0x00, 1); /* flags: present */
		put_le32(e + 0x04, io->buffer_id);
		put_le32(e + 0x08, io->dir); /* type: 0 in / 1 out */
		put_le32(e + 0x10, io->dir ? 2 : 1); /* io direction */
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

static int bo_alloc(struct ane_nn *nn, struct ane_bo *bo, uint64_t size)
{
	struct drm_ane_bo_init args = { .size = size };
	struct drm_ane_bo_free f;

	if (ioctl(nn->fd, DRM_IOCTL_ANE_BO_INIT, &args) < 0) {
		ane_m2_err("DRM_IOCTL_ANE_BO_INIT failed for %llu bytes\n",
			   (unsigned long long)size);
		return -EINVAL;
	}
	bo->size = size;
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
		binds[i].type = ctx->model.io[i].dir;
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
		io[i].type = ctx->model.io[i].dir;
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
