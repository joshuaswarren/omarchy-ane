// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */
#ifndef ANE_SHA256_H
#define ANE_SHA256_H

#include <stdint.h>

#define ANE_SHA256_LEN 32

struct ane_sha256_ctx {
	uint32_t st[8];
	uint8_t buf[64];
	unsigned buflen;
	uint64_t total;
	int neon;		/* per-context compression path */
	int portable_forced;	/* per-context test override */
};

void ane_sha256_init(struct ane_sha256_ctx *c);
void ane_sha256_update(struct ane_sha256_ctx *c, const void *data,
		       uint64_t len);
void ane_sha256_final(struct ane_sha256_ctx *c, uint8_t out[ANE_SHA256_LEN]);

/* Runtime capability query: the ARMv8 SHA-2 crypto extension is usable
 * (always 0 off arm64). */
int ane_sha256_hw_supported(void);

/* Per-context test override: force the reference C compression even
 * when the hardware path is available. Cannot race: the field lives in
 * the caller's context, not in shared state. */
void ane_sha256_ctx_force_portable(struct ane_sha256_ctx *c, int force);

/* One-shot convenience. */
void ane_sha256(const void *data, uint64_t len, uint8_t out[ANE_SHA256_LEN]);

#endif /* ANE_SHA256_H */
