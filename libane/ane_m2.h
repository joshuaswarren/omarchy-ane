// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#ifndef __ANE_M2_H__
#define __ANE_M2_H__

#include <stdio.h>
#include <stdint.h>

#include <asm/types.h>
#include <ane_accel.h>
#include "ane.h"

/*
// ABI-2 backend for the T6021 (M2) ANE, H14 firmware programs.
//
// The section payloads are built by ane_m2_program_build(), which is
// device-independent and is also the core of the host byte-identity
// self-check (tools/ane-selfcheck.c): on every fixture under
// fixtures/h14-anec it must reproduce the lab section builder's output
// (lab tools/h14_sections.py) byte for byte.
//
// PROVEN on hardware means a bit-exact y == a+b run through the
// scratch-module sequencer path (LOAD_PROGRAM -> CREATE_PROCEDURE ->
// PROCEDURE_CALL, boot 3ab812a3). Everything the builder emits was in
// that run; every structural rule it applies is labelled below.
*/

#ifndef LIBANE_CONFIG_NO_ERR
#define ane_m2_err(a, ...) fprintf(stderr, "LIBANE: M2: ERR: " a, ##__VA_ARGS__)
#else
#define ane_m2_err(...) \
	do {            \
	} while (0)
#endif

#define ANE_M2_PRIORITY_DEFAULT 2 /* cmd priority, valid [2,7]; the proven value */
#define ANE_M2_EXEC_TIMEOUT_MS 30000
/* H14 anec tile counts are denominated in 0x4000-byte units: the add fixture
 * carries tiles[4..6] == 2 and its allocation is 2 << 14 == 0x8000. Proven on
 * one fixture; the builder refuses a disagreement rather than guess. */
#define ANE_M2_TILE_UNIT_SHIFT 14

/* Firmware section ids for LOAD_PROGRAM (driver-replayed bytes). */
enum {
	ANE_M2_SEC_GENERIC = 0,
	ANE_M2_SEC_KERNEL,
	ANE_M2_SEC_DESCRIPTOR,
	ANE_M2_SEC_OPERATION,
	ANE_M2_SEC_PROCEDURE,
	ANE_M2_SEC_TDPROP,
	ANE_M2_SEC_COUNT
};

/* One runtime buffer: an ANEC channel id == tile index, its direction and its
 * allocation size (the generic-entry size). Emission order: inputs in
 * ascending channel order, then the output. */
struct ane_m2_io {
	uint32_t buffer_id;
	uint32_t dir; /* 0 = input, 1 = output */
	uint64_t size;
};

/* One operation-section ref: the firmware copies the IOVA of buffer `tag`
 * into local BAR `slot` (fw135 pushToHWDirect 0x44c98). `addr` is the TD
 * register offset the ref patches (kept here for scratch-merge
 * eligibility; not part of the emitted op-section bytes). `payload0` is
 * the BAR-ref's first payload word (the byte offset inside the bound
 * surface for src/dst refs; the kdma reference offset for kernel reads),
 * kept so the extents-based tag rule can spot the largest payload across
 * all tasks at a slot. */
struct ane_m2_ref {
	uint32_t slot;
	uint32_t tag;
	uint32_t addr;
	uint32_t payload0;
};

/* ANE_M2_MAX_CALLS bounds the per-call ref set: one record per task in the
 * worst case, with ANE_M2_MAX_BINDS deduped refs each. For islands the
 * largest emitted program is `pv-rank4` with 5 tasks; the limit is set
 * to 64 for headroom. */
#define ANE_M2_MAX_CALLS 64

struct ane_m2_model {
	struct ane_m2_io io[ANE_M2_MAX_BINDS];
	uint32_t io_count;
	/* Index of the scratch buffer inside io[], or UINT32_MAX when no
	 * scratch merge was needed (the byte-identity path for stages
	 * 1-4 + rms-c2048-gamma). The scratch entry's dir is set to 2
	 * (a host-internal value) so ane_m2_send/read skip it. */
	uint32_t scratch_io_index;
	/* One ref-set per call; the operation section emits calls records,
	 * each with call_ref_count[j] {slot, tag} pairs. Single-call programs
	 * (the existing stage 1-4 fixtures) collapse to calls == 1. */
	uint32_t calls;
	uint32_t call_ref_count[ANE_M2_MAX_CALLS];
	struct ane_m2_ref call_refs[ANE_M2_MAX_CALLS][ANE_M2_MAX_BINDS];
};

struct ane_m2_sections {
	struct {
		void *data;
		uint64_t size;
	} sec[ANE_M2_SEC_COUNT];
};

extern const uint32_t ane_m2_section_ids[ANE_M2_SEC_COUNT];

/* Parse the H14 anec and build all six LOAD_PROGRAM section payloads.
 * Returns 0 and fills `model` + `secs` (malloc'd, free with
 * ane_m2_sections_free), or -EINVAL/-ENOMEM and fills nothing.
 * Host-usable: no device, no fd. */
int ane_m2_program_build(const void *anec, uint64_t anec_size,
			 struct ane_m2_model *model,
			 struct ane_m2_sections *secs);
void ane_m2_sections_free(struct ane_m2_sections *secs);

/* Device path on an ABI-2 accel node (nn->fd already open). Returns 0 and
 * sets nn->m2, or negative. */
int ane_m2_open(struct ane_nn *nn, const char *path);
void ane_m2_close(struct ane_nn *nn);

int ane_m2_exec(struct ane_nn *nn);
int ane_m2_send(struct ane_nn *nn, const void *from, uint32_t idx);
int ane_m2_read(struct ane_nn *nn, void *to, uint32_t idx);
uint64_t ane_m2_src_size(struct ane_nn *nn, uint32_t idx);
uint64_t ane_m2_dst_size(struct ane_nn *nn, uint32_t idx);

#endif /* __ANE_M2_H__ */
