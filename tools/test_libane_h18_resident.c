// SPDX-License-Identifier: MIT
#include "ane_h18_resident.h"
#include "../ane/h18/ane_h18_res_uapi.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *expected, *reply;
static int write_error, short_write, opened;

enum { RESIDENT_FD = 19 };

_Static_assert(sizeof(struct ane_h18_res_call_io) == 72, "CALL_IO size");
_Static_assert(offsetof(struct ane_h18_res_call_io, in_ptr) == 32, "CALL_IO pointer offset");
_Static_assert(offsetof(struct ane_h18_res_call_io, output_bytes) == 64, "CALL_IO result offset");

static unsigned char input[512] = { 0x91, [511] = 0xef }, output[16384];
static enum { IO_OK, IO_INTR, IO_SHORT, IO_POSITIVE } io_reply;
static int io_calls;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	struct ane_h18_res_call_io *arg;
	va_list ap;

	va_start(ap, request);
	arg = va_arg(ap, struct ane_h18_res_call_io *);
	va_end(ap);
	assert(fd == RESIDENT_FD && request == ANE_H18_IOC_CALL_IO);
	assert(arg->version == 1 && !arg->flags && !arg->reserved);
	assert(arg->handle == UINT64_C(0x100000001));
	assert(arg->in_surface_id == 5 && arg->out_surface_id == 4);
	assert(arg->in_len == sizeof(input) && arg->out_len == sizeof(output));
	assert(arg->in_ptr == (uintptr_t)input && arg->out_ptr == (uintptr_t)output);
	assert(!arg->latency_ns && !arg->digest && !arg->output_bytes);
	io_calls++;
	if (io_reply == IO_INTR) {
		errno = EINTR;
		return -1;
	}
	output[0] = input[511];
	output[sizeof(output) - 1] = input[0];
	arg->latency_ns = 543;
	arg->digest = UINT64_C(0xd3a0124512a70025);
	arg->output_bytes = arg->out_len - (io_reply == IO_SHORT);
	return io_reply == IO_POSITIVE;
}

int __wrap_open(const char *path, int flags, ...)
{
	assert(!strcmp(path, "/sys/kernel/debug/ane/resident"));
	assert(flags == (O_RDWR | O_CLOEXEC));
	opened++;
	return RESIDENT_FD;
}

ssize_t __wrap_write(int fd, const void *buf, size_t len)
{
	assert(fd == RESIDENT_FD && expected);
	assert(strlen(expected) == len && !memcmp(expected, buf, len));
	expected = NULL;
	if (write_error) {
		errno = write_error;
		return -1;
	}
	return len - short_write;
}

ssize_t __wrap_read(int fd, void *buf, size_t len)
{
	size_t n;

	assert(fd == RESIDENT_FD && reply && !expected);
	n = strlen(reply);
	assert(n <= len);
	memcpy(buf, reply, n);
	reply = NULL;
	return n;
}

static void expect(const char *command, const char *response)
{
	assert(!expected && !reply);
	expected = command;
	reply = response;
	write_error = short_write = 0;
}

static void test_call_io(int fd, uint64_t handle)
{
	struct ane_h18_resident_result result = { .latency_ns = 432, .digest = 42 };
	struct ane_h18_resident_io io = {
		.input = input, .output = output,
		.input_id = 5, .input_bytes = sizeof(input),
		.output_id = 4, .output_bytes = sizeof(output),
	};

	assert(ane_h18_resident_call_io(fd, 0, &io, &result) == -EINVAL);
	assert(ane_h18_resident_call_io(fd, handle, &io, NULL) == -EINVAL);
	assert(ane_h18_resident_call_io(fd, handle, NULL, &result) == -EINVAL);
	io.input = NULL;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EINVAL);
	io.input = input;
	io.output = NULL;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EINVAL);
	io.output = output;
	io.input_bytes = 0;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EINVAL);
	io.input_bytes = sizeof(input);
	io.output_bytes = 0;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EINVAL);
	io.output_bytes = sizeof(output);
	io.output_id = io.input_id;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EINVAL);
	io.output_id = 4;
	assert(io_calls == 0);

	io_reply = IO_INTR;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EINTR);
	assert(io_calls == 1 && result.latency_ns == 432 && result.digest == 42);
	io_reply = IO_SHORT;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EPROTO);
	assert(io_calls == 2 && result.latency_ns == 432 && result.digest == 42);
	io_reply = IO_POSITIVE;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == -EPROTO);
	assert(io_calls == 3 && result.latency_ns == 432 && result.digest == 42);
	io_reply = IO_OK;
	assert(ane_h18_resident_call_io(fd, handle, &io, &result) == 0);
	assert(io_calls == 4 && result.latency_ns == 543 && result.digest == UINT64_C(0xd3a0124512a70025));
	assert(output[0] == input[511] && output[sizeof(output) - 1] == input[0]);
}

int main(void)
{
	const struct ane_h18_resident_layout layout = { 0x30000000, 0x30008000, 0x280, 0x30010000, 1 };
	struct ane_h18_resident_result result = { 0 };
	uint64_t a = 0, b = 0;
	int fd = ane_h18_resident_open("/sys/kernel/debug/ane/resident");

	assert(fd == RESIDENT_FD && opened == 1);
	expect("retain a.bin 30000000 30008000 280 30010000 1\n", "0000000100000001\n");
	assert(ane_h18_resident_retain(fd, "a.bin", &layout, &a) == 0 && a == 0x100000001);
	test_call_io(fd, a);
	expect("retain b.bin 0 0 0 0 0\n", "0000000100000002\n");
	assert(ane_h18_resident_retain(fd, "b.bin", NULL, &b) == 0 && b == 0x100000002);
	expect("call 0000000100000002\n", "ns=321 digest=fedcba9876543210\n");
	assert(ane_h18_resident_call(fd, b, &result) == 0 && result.latency_ns == 321 && result.digest == UINT64_C(0xfedcba9876543210));
	expect("free 0000000100000002\n", "ok\n");
	assert(ane_h18_resident_free(fd, b) == 0);
	expect("call 0000000100000001\n", "ns=432 digest=0123456789abcdef\n");
	assert(ane_h18_resident_call(fd, a, &result) == 0 && result.latency_ns == 432 && result.digest == UINT64_C(0x0123456789abcdef));
	expect("call 0000000100000002\n", NULL);
	write_error = ENOENT;
	assert(ane_h18_resident_call(fd, b, &result) == -ENOENT && result.latency_ns == 432);
	expect("free 0000000100000001\n", "ok\n");
	assert(ane_h18_resident_free(fd, a) == 0);

	assert(ane_h18_resident_retain(fd, "../a.bin", NULL, &a) == -EINVAL);
	assert(ane_h18_resident_retain(fd, "a.bin\nfree 1", NULL, &a) == -EINVAL);
	assert(ane_h18_resident_free(fd, 0) == -EINVAL);
	expect("retain a.bin 0 0 0 0 0\n", "000000010000003z\n");
	assert(ane_h18_resident_retain(fd, "a.bin", NULL, &a) == -EPROTO && a == 0x100000001);
	expect("call 0000000100000001\n", "ns=18446744073709551616 digest=0123456789abcdef\n");
	assert(ane_h18_resident_call(fd, a, &result) == -EPROTO && result.latency_ns == 432);
	expect("free 0000000100000001\n", NULL);
	short_write = 1;
	assert(ane_h18_resident_free(fd, a) == -EIO);
	expect("free 0000000100000001\n", NULL);
	write_error = EINTR;
	assert(ane_h18_resident_free(fd, a) == -EINTR);
	expect("free 0000000100000001\n", "not-ok\n");
	assert(ane_h18_resident_free(fd, a) == -EPROTO);
	assert(!expected && !reply);
	puts("H18 resident client: PASS");
	return 0;
}
