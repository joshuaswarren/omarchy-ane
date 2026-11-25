// SPDX-License-Identifier: MIT
#include "ane_h18_resident.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *expected, *reply;
static int write_error, short_write, opened;

enum { RESIDENT_FD = 19 };

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

int main(void)
{
	const struct ane_h18_resident_layout layout = { 0x30000000, 0x30008000, 0x280, 0x30010000, 1 };
	struct ane_h18_resident_result result = { 0 };
	uint64_t a = 0, b = 0;
	int fd = ane_h18_resident_open("/sys/kernel/debug/ane/resident");

	assert(fd == RESIDENT_FD && opened == 1);
	expect("retain a.bin 30000000 30008000 280 30010000 1\n", "0000000100000001\n");
	assert(ane_h18_resident_retain(fd, "a.bin", &layout, &a) == 0 && a == 0x100000001);
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
