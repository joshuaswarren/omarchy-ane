// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __ANE_ACCEL_H__
#define __ANE_ACCEL_H__

#if defined(__cplusplus)
extern "C" {
#endif

#define ANE_TILE_COUNT	0x20
#define ANE_FIFO_NID	0x40
#define ANE_CMD_GRAN	0x10

/* ABI 1: successful SUBMIT guarantees terminal completion and CPU visibility. */
#define ANE_ABI_MAJOR 1

/*
 * ABI 2 (T6021 / M2, H14 firmware programs): the driver reports
 * DRM_IOCTL_VERSION major 2 and rejects DRM_ANE_SUBMIT. The M1 libane
 * requires major 1, so it cannot submit M1 frames to an M2 device.
 * A successful DRM_ANE_EXEC guarantees terminal completion (firmware ack
 * and task-queue idle) and CPU visibility of the output buffers.
 */
#define ANE_ABI_M2_MAJOR 2
#define ANE_M2_MAX_SECTIONS	16
#define ANE_M2_MAX_BINDS	64

/*
 * ABI 3 (in-tree aurora ane_t6021 H15/H16/H17 line): the driver reports
 * DRM_IOCTL_VERSION major 3 and answers GET_CAPS with abi_version 3 and
 * chip_family 15 (T8122 / M3), 16 (H16) or 17 (T6050 / M5).
 * DRM_IOCTL_VERSION minor 1 adds surface mmap (DRM_ANE_H16_MMAP_OFFSET)
 * and DRM_ANE_H16_EXEC_IN_PLACE.
 */
#define ANE_ABI_V3_MAJOR	3
#define ANE_CHIP_H15		15
#define ANE_CHIP_H16		16
#define ANE_CHIP_H17		17
#define ANE_V3_GET_CAPS		0x00 /* DRM_COMMAND_BASE + 0, the aurora node */

/* The aurora GET_CAPS argument (aurora include/uapi/drm/ane_accel.h):
 * DRM_IOWR(DRM_COMMAND_BASE + 0x00); eight __u32, size says how many the
 * driver filled. DRM_COMMAND_BASE is 0x40 (drm_mode.h). */
struct ane_v3_get_caps {
	__u32 flags;
	__u32 size;
	__u32 abi_version;
	__u32 chip_family;
	__u32 section_size;
	__u32 bind_size;
	__u32 exec_io_size;
	__u32 pad;
};
#define ANE_V3_GET_CAPS_IOCTL \
	DRM_IOWR(0x40 + ANE_V3_GET_CAPS, struct ane_v3_get_caps)

#define DRM_ANE_BO_INIT 0x1
#define DRM_ANE_BO_FREE 0x2
#define DRM_ANE_SUBMIT	0x3
#define DRM_ANE_PROG_LOAD	0x4
#define DRM_ANE_PROC_CREATE	0x5
#define DRM_ANE_EXEC		0x6
#define DRM_ANE_PROG_LOOKUP	0x7

struct drm_ane_bo_init {
	__u32 handle;
	__u32 pad;
	__u64 size;
	__u64 offset;
};

struct drm_ane_bo_free {
	__u32 handle;
	__u32 pad;
};

struct drm_ane_submit {
	__u64 tsk_size;
	__u32 td_count;
	__u32 td_size;
	__u32 handles[ANE_TILE_COUNT];
	__u32 btsp_handle;
	__u32 pad;
};

/* One firmware program section, already built by userspace, held in a BO. */
struct drm_ane_section {
	__u32 id;		/* firmware section id */
	__u32 bo_handle;	/* BO holding the bytes */
	__u64 size;
	__u64 offset;		/* byte offset inside the BO */
};

/* A buffer whose IOVA and size the driver patches into a generic entry. */
struct drm_ane_generic_bind {
	__u32 buffer_id;
	__u32 bo_handle;
	__u32 type;
	__u32 pad;
	__u64 size;
};

struct drm_ane_prog_load {
	__u64 sections_ptr;	/* struct drm_ane_section[section_count] */
	__u64 generic_ptr;	/* struct drm_ane_generic_bind[generic_count] */
	__u32 section_count;
	__u32 generic_count;
	__u32 prog_id_out;
	__u32 pad;
};

struct drm_ane_proc_create {
	__u32 prog_id;
	__u32 proc_id_out;
};

/* Per-call buffer binding: an input or output for one procedure call. */
struct drm_ane_exec_io {
	__u32 buffer_id;
	__u32 bo_handle;
	__u32 type;
	__u32 flags;
	__u64 dma;		/* in only, reserved: the driver binds the
				 * IOVA from bo_handle */
	__u64 size;
};

struct drm_ane_exec {
	__u32 prog_id;
	__u32 proc_id;
	__u32 priority;		/* 2..7 */
	__u32 timeout_ms;
	__u32 count;		/* 1..ANE_M2_MAX_BINDS */
	__u32 pad;		/* t6021 EXEC: 0 or 1 = one call (as before);
				 * 2..16 = batched call count: the driver
				 * posts pad PROCEDURE_CALLs back-to-back
				 * on the channel-1 IO ring and returns
				 * after pad finish events. All calls
				 * bind the same buffers. */
	__u64 io_ptr;		/* struct drm_ane_exec_io[count] */
};

/*
 * DRM_ANE_PROG_LOOKUP: is this exact program already loaded? The key is
 * the same digest PROG_LOAD computes (SHA-256 over every section's id,
 * size and bytes, sections in ascending id order). On a hit the caller
 * may skip section BO allocation entirely and go straight to
 * PROC_CREATE + EXEC with the returned ProgramId -- the firmware keeps
 * reading the sections the first loader supplied. A miss publishes
 * nothing; the caller loads as before.
 */
struct drm_ane_prog_lookup {
	__u64 digest_ptr;	/* 32-byte SHA-256 */
	__u32 digest_len;	/* must be 32 */
	__u32 pad;
	__u32 found_out;	/* 1 when prog_id_out is usable */
	__u32 prog_id_out;
};

#define DRM_IOCTL_ANE_BO_INIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_BO_INIT, struct drm_ane_bo_init)
#define DRM_IOCTL_ANE_BO_FREE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_BO_FREE, struct drm_ane_bo_free)
#define DRM_IOCTL_ANE_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_SUBMIT, struct drm_ane_submit)

#define DRM_IOCTL_ANE_PROG_LOAD \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_PROG_LOAD, struct drm_ane_prog_load)
#define DRM_IOCTL_ANE_PROC_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_PROC_CREATE, struct drm_ane_proc_create)
#define DRM_IOCTL_ANE_EXEC \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_EXEC, struct drm_ane_exec)
#define DRM_IOCTL_ANE_PROG_LOOKUP \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_PROG_LOOKUP, \
		 struct drm_ane_prog_lookup)

/*
 * ABI version 3 (aurora-silicon/linux include/uapi/drm/ane_accel.h):
 * H15 (T8122), H16 and H17 programs load an HWX plus four host-built
 * sections via DRM_IOCTL_ANE_H16_LOAD and execute via DRM_IOCTL_ANE_H16_EXEC.
 * Command 0x07 overlaps ABI-2 PROG_LOOKUP numerically, with distinct struct
 * size and ABI major gating (DRM_IOCTL_VERSION major == 3).
 */
#define DRM_ANE_H16_LOAD	0x07
#define DRM_ANE_H16_EXEC	0x08

#define DRM_ANE_H16_SECTION_GENERIC		1
#define DRM_ANE_H16_SECTION_OPERATION		2
#define DRM_ANE_H16_SECTION_PROCEDURE		3
#define DRM_ANE_H16_SECTION_TEXT_PROPERTIES	4
#define DRM_ANE_H16_SECTIONS			4
#define DRM_ANE_H16_SECTION_MIN			0x28
#define DRM_ANE_H16_SECTION_MAX			0x10000

#define DRM_ANE_H16_RESOURCE_GENERIC		1002
#define DRM_ANE_H16_RESOURCE_OPERATION		1003
#define DRM_ANE_H16_RESOURCE_PROCEDURE		1004
#define DRM_ANE_H16_RESOURCE_TEXT_PROPERTIES	1005
#define DRM_ANE_H16_RESOURCE_TEXT		1006
#define DRM_ANE_H16_RESOURCE_KERNEL		1007
#define DRM_ANE_H16_SURFACES			4

#define DRM_ANE_H16_MMAP_OFFSET(i)	(((__u64)((i) + 1)) << 32)
#define DRM_ANE_H16_EXEC_IN_PLACE	(1u << 0)

struct drm_ane_h16_section {
	__u32 kind;
	__u32 size;
	__u64 data_ptr;
};

struct drm_ane_h16_load {
	__u64 hwx_ptr;
	__u64 hwx_size;
	__u64 sections_ptr;
	__u32 section_count;
	__u32 flags;
	__u32 surface_count;
	__u32 pad;
	__u64 surface_size[DRM_ANE_H16_SURFACES];
	__u32 surface_direction[DRM_ANE_H16_SURFACES];
};

struct drm_ane_h16_buffer {
	__u64 ptr;
	__u64 size;
};

struct drm_ane_h16_exec {
	__u64 buffers_ptr;
	__u32 count;
	__u32 flags;
};

#define DRM_IOCTL_ANE_H16_LOAD \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_ANE_H16_LOAD, struct drm_ane_h16_load)
#define DRM_IOCTL_ANE_H16_EXEC \
	DRM_IOW(DRM_COMMAND_BASE + DRM_ANE_H16_EXEC, struct drm_ane_h16_exec)

#if defined(__cplusplus)
}
#endif

#endif /* __ANE_ACCEL_H__ */
