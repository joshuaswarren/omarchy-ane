// SPDX-License-Identifier: MIT
#ifndef LIBANE_H18_RESIDENT_H
#define LIBANE_H18_RESIDENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ane_h18_resident_layout {
	uint64_t bufs, text, text_const, kern;
	unsigned int kern_slot;
};

struct ane_h18_resident_result {
	uint64_t latency_ns, digest;
};

/* Experimental H18 diagnostic session, not a DRM ABI. Open a resident debugfs
 * file and serialize calls on the returned fd; close it to end the session.
 * Other operations return 0 or -errno. Handles are local to this session.
 * If a reply is malformed or a write is short, close the session instead of
 * retrying: a retain may have succeeded without returning its handle.
 */
int ane_h18_resident_open(const char *path);
int ane_h18_resident_retain(int fd, const char *name,
			   const struct ane_h18_resident_layout *layout,
			   uint64_t *handle);
/* Call the selected resident program with its embedded ANEP inputs. */
int ane_h18_resident_call(int fd, uint64_t handle,
			 struct ane_h18_resident_result *result);
int ane_h18_resident_free(int fd, uint64_t handle);

#ifdef __cplusplus
}
#endif
#endif
