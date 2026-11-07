/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Use libc's public errno entry point; include_next avoids intercepting
 * glibc's own include of linux/errno.h with an empty recursive wrapper. */
#include_next <linux/errno.h>
