// SPDX-License-Identifier: MIT
#include "ane_h18_resident.h"
#include "../ane/h18/ane_h18_res_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define FW_NAME_MAX 63
#define REPLY_SIZE 64
#define HEX_DIGITS "0123456789abcdefABCDEF"

static int send_command(int fd, const char *command, size_t len)
{
	/* Never retry a write: the firmware operation may already have run. */
	ssize_t n = write(fd, command, len);

	if (n < 0)
		return -errno;
	return (size_t)n == len ? 0 : -EIO;
}

static int read_reply(int fd, char reply[REPLY_SIZE])
{
	ssize_t n;

	do {
		n = read(fd, reply, REPLY_SIZE - 1);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -errno;
	if (!n || reply[n - 1] != '\n' || memchr(reply, '\0', n))
		return -EPROTO;
	reply[n - 1] = '\0';
	return 0;
}

static int parse_hex(const char *s, uint64_t *value)
{
	if (strlen(s) != 16 || strspn(s, HEX_DIGITS) != 16)
		return -EPROTO;
	*value = strtoull(s, NULL, 16);
	return 0;
}

static int valid_name(const char *name)
{
	const char *chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" "0123456789._-";
	size_t len;

	if (!name)
		return 0;
	len = strnlen(name, FW_NAME_MAX + 1);
	return len && len <= FW_NAME_MAX && strspn(name, chars) == len &&
	       strcmp(name, ".") && !strstr(name, "..");
}

int ane_h18_resident_open(const char *path)
{
	int fd;

	if (!path || !*path)
		return -EINVAL;
	fd = open(path, O_RDWR | O_CLOEXEC);
	return fd < 0 ? -errno : fd;
}

int ane_h18_resident_retain(int fd, const char *name,
			   const struct ane_h18_resident_layout *layout,
			   uint64_t *handle)
{
	static const struct ane_h18_resident_layout dynamic;
	char command[FW_NAME_MAX + 88], reply[REPLY_SIZE];
	uint64_t id;
	int n, ret;

	if (!layout)
		layout = &dynamic;
	if (!handle || !valid_name(name) || layout->kern_slot > 1)
		return -EINVAL;
	n = snprintf(command, sizeof(command), "retain %s %" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 " %u\n",
		     name, layout->bufs, layout->text, layout->text_const, layout->kern, layout->kern_slot);
	ret = send_command(fd, command, n);
	if (!ret)
		ret = read_reply(fd, reply);
	if (ret)
		return ret;
	if (parse_hex(reply, &id) || !id)
		return -EPROTO;
	*handle = id;
	return 0;
}

static int send_handle(int fd, const char *op, uint64_t handle)
{
	char command[24];
	int n;

	if (!handle)
		return -EINVAL;
	n = snprintf(command, sizeof(command), "%s %016" PRIx64 "\n", op, handle);
	return send_command(fd, command, n);
}

int ane_h18_resident_call(int fd, uint64_t handle,
			 struct ane_h18_resident_result *result)
{
	char reply[REPLY_SIZE], *end;
	struct ane_h18_resident_result info;
	int ret;

	if (!result)
		return -EINVAL;
	ret = send_handle(fd, "call", handle);
	if (!ret)
		ret = read_reply(fd, reply);
	if (ret)
		return ret;
	if (strncmp(reply, "ns=", 3) || reply[3] < '0' || reply[3] > '9')
		return -EPROTO;
	errno = 0;
	info.latency_ns = strtoull(reply + 3, &end, 10);
	if (errno == ERANGE || strncmp(end, " digest=", 8) || parse_hex(end + 8, &info.digest))
		return -EPROTO;
	*result = info;
	return 0;
}

int ane_h18_resident_call_io(int fd, uint64_t handle,
			    const struct ane_h18_resident_io *io,
			    struct ane_h18_resident_result *result)
{
	struct ane_h18_res_call_io request;
	int ret;

	if (!handle || !io || !result || !io->input || !io->output ||
	    !io->input_bytes || !io->output_bytes || io->input_id == io->output_id)
		return -EINVAL;
	request = (struct ane_h18_res_call_io) {
		.version = ANE_H18_RES_ABI_VERSION, .handle = handle,
		.in_surface_id = io->input_id, .out_surface_id = io->output_id,
		.in_len = io->input_bytes, .out_len = io->output_bytes,
		.in_ptr = (uintptr_t)io->input, .out_ptr = (uintptr_t)io->output,
	};
	/* A failed ioctl may already have run the firmware operation. */
	ret = ioctl(fd, ANE_H18_IOC_CALL_IO, &request);
	if (ret < 0)
		return -errno;
	if (ret || request.output_bytes != io->output_bytes)
		return -EPROTO;
	*result = (struct ane_h18_resident_result) {
		.latency_ns = request.latency_ns, .digest = request.digest,
	};
	return 0;
}

int ane_h18_resident_call_multi_io(int fd, uint64_t handle,
				  const struct ane_h18_resident_in_buf *inputs,
				  unsigned int in_count,
				  const struct ane_h18_resident_out_buf *outputs,
				  unsigned int out_count,
				  struct ane_h18_resident_result *result)
{
	struct ane_h18_res_call_multi_io request;
	unsigned int i;
	int ret;

	if (!handle || !inputs || !outputs || !result ||
	    !in_count || in_count > ANE_H18_RES_MAX_USER_IO ||
	    !out_count || out_count > ANE_H18_RES_MAX_USER_IO)
		return -EINVAL;
	memset(&request, 0, sizeof(request));
	request.version = ANE_H18_RES_ABI_VERSION;
	request.handle = handle;
	request.in_count = in_count;
	request.out_count = out_count;
	for (i = 0; i < in_count; i++) {
		if (!inputs[i].data || !inputs[i].bytes)
			return -EINVAL;
		request.inputs[i].surface_id = inputs[i].id;
		request.inputs[i].len = inputs[i].bytes;
		request.inputs[i].ptr = (uintptr_t)inputs[i].data;
	}
	for (i = 0; i < out_count; i++) {
		if (!outputs[i].data || !outputs[i].bytes)
			return -EINVAL;
		request.outputs[i].surface_id = outputs[i].id;
		request.outputs[i].len = outputs[i].bytes;
		request.outputs[i].ptr = (uintptr_t)outputs[i].data;
	}
	ret = ioctl(fd, ANE_H18_IOC_CALL_MULTI_IO, &request);
	if (ret < 0)
		return -errno;
	if (ret)
		return -EPROTO;
	*result = (struct ane_h18_resident_result) {
		.latency_ns = request.latency_ns, .digest = request.digest,
	};
	return 0;
}

int ane_h18_resident_free(int fd, uint64_t handle)
{
	char reply[REPLY_SIZE];
	int ret = send_handle(fd, "free", handle);

	if (!ret)
		ret = read_reply(fd, reply);
	if (ret)
		return ret;
	return strcmp(reply, "ok") ? -EPROTO : 0;
}
