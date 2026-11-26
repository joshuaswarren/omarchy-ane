/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _ANE_H18_RES_UAPI_H
#define _ANE_H18_RES_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define ANE_H18_RES_ABI_VERSION	1U
#define ANE_H18_RES_IOC_MAGIC	'H'

/*
 * ANE_H18_IOC_CALL_IO: execute one retained resident program with explicit
 * input and output surface payloads on the exclusive 0600 resident debugfs fd.
 *
 * Input fields:
 *   version        : must equal ANE_H18_RES_ABI_VERSION (1).
 *   flags          : must be 0.
 *   handle         : 64-bit resident handle returned by ASCII "retain".
 *   in_surface_id  : ANEP plain-input surface id (dir == 0, with both ANEP
 *                    data offset and data size equal to 0) of that program.
 *   out_surface_id : ANEP output surface id (dir == 1) of that program.
 *   in_len         : bytes to read from in_ptr (1 .. in_surface.size).
 *                    Writable input tail [in_len .. in_surface.size) is zeroed
 *                    before PROCEDURE_CALL.
 *   out_len        : bytes to write to out_ptr (1 .. out_surface.size).
 *                    Full output surface(s) are zeroed before PROCEDURE_CALL;
 *                    out_len bytes are copied to out_ptr only after matched
 *                    finish.
 *   in_ptr         : userspace pointer to in_len input bytes (non-zero).
 *   out_ptr        : userspace pointer to out_len output buffer (non-zero).
 *   reserved       : must be 0 on input; set to 0 on output.
 *
 * Output buffer and metadata fields are valid only when ioctl returns 0
 * (unspecified on any errno, as copy_to_user may partially copy before EFAULT):
 *   latency_ns     : matched-finish call latency in nanoseconds.
 *   digest         : FNV-1a 64-bit digest over all u32 words of out_surface_id.
 *   output_bytes   : exact bytes copied to out_ptr (always == out_len on 0).
 *
 * Status is reported strictly via syscall errno (0 on success; -EINVAL,
 * -ENOENT, -ESHUTDOWN, -EFAULT, -ENOMEM, -EIO, -ENOTTY).
 */
struct ane_h18_res_call_io {
	__u32 version;
	__u32 flags;
	__u64 handle;
	__u32 in_surface_id;
	__u32 out_surface_id;
	__u32 in_len;
	__u32 out_len;
	__u64 in_ptr;
	__u64 out_ptr;
	__u64 latency_ns;
	__u64 digest;
	__u32 output_bytes;
	__u32 reserved;
};

#define ANE_H18_IOC_CALL_IO	_IOWR(ANE_H18_RES_IOC_MAGIC, 1, struct ane_h18_res_call_io)

/*
 * ANE_H18_IOC_CALL_MULTI_IO: execute one retained resident program with
 * 1..ANE_H18_RES_MAX_USER_IO explicit writable input surfaces and
 * 1..ANE_H18_RES_MAX_USER_IO explicit output surfaces on the resident fd.
 *
 * Pre-loaded read-only input surfaces (ANEP dir == 0 with nonzero data) stay
 * untouched across calls; scratch surfaces (dir == 2) are zeroed before call.
 * Unused entries in inputs[in_count..] and outputs[out_count..] must be zero.
 * digest is FNV-1a 64-bit over all u32 words of outputs[0].surface_id.
 */
#define ANE_H18_RES_MAX_USER_IO	8U

struct ane_h18_res_buf_desc {
	__u32 surface_id;
	__u32 len;
	__u64 ptr;
};

struct ane_h18_res_call_multi_io {
	__u32 version;
	__u32 flags;
	__u64 handle;
	__u32 in_count;
	__u32 out_count;
	struct ane_h18_res_buf_desc inputs[ANE_H18_RES_MAX_USER_IO];
	struct ane_h18_res_buf_desc outputs[ANE_H18_RES_MAX_USER_IO];
	__u64 latency_ns;
	__u64 digest;
	__u32 reserved;
	__u32 pad;
};

#define ANE_H18_IOC_CALL_MULTI_IO	_IOWR(ANE_H18_RES_IOC_MAGIC, 2, struct ane_h18_res_call_multi_io)

#endif /* _ANE_H18_RES_UAPI_H */
