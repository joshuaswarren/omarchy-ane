/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef SIM_CRYPTO_SHA2_H
#define SIM_CRYPTO_SHA2_H
/* The sim never executes the owned-payload digest check: staging refuses earlier on the
 * loader's size mismatch, and the fake firmware cannot supply the pinned payload. The
 * kernel build uses crypto/sha2.h; this stub only keeps the include graph compiling. */
#include <linux/types.h>
static inline void sha256(const u8 *data, size_t len, u8 *out)
{
	unsigned int i;

	(void)data;
	(void)len;
	for (i = 0; i < 32; i++)
		out[i] = 0;
}
#endif
